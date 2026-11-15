#!/usr/bin/env python3

"""Physics-informed compensation (PINN) for the SMCP structured-light system.

Trains a correction of each reconstructed point, in the camera frame of the scan,

    p' = p * (1 + s(p)) + r(p)

where the physical block s moves the point along its viewing ray (the camera sits at the origin),

    s = a0 + a1*zn + a2*zn^2 + a3*rho2 + a4*rho2^2 + a5*zn*rho2,
    zn = (z - z_center) / z_scale,  rho2 = (x^2 + y^2) / z^2,

and r is a small multilayer perceptron for the error the physics does not explain. Its input is the
point normalized to the volume covered by the training data (and clamped to it), and its output is
bounded to +/- output_bound_mm per axis. Only the network is regularized (magnitude and spatial
smoothness), and a0 starts at the fixed scale correction, so the physical block carries the global
scale instead of the network.

There is no point-wise ground truth: the loss compares the diameters and center distances of the
spheres refitted on the corrected points with the CMM values of the artifact. The sphere fits are
differentiable (algebraic fit followed by Gauss-Newton steps), batched over every sphere.

Input: the folder written by "Export Training Data" in the SMCP editor (manifest.json and one
<scan>.spheres.xyz per scan with "x y z label" lines). Output: a JSON compensation model that the SMCP
application evaluates. Scans are validated by cross-validation over scans (each scan is tested by a
model that never saw it) comparing the fixed scale, the physical block alone, and the full PINN.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import torch
from torch import nn


FORMAT = "smcp-compensation"
FORMAT_VERSION = 1
PHYSICS_FORM = "s = a0 + a1*zn + a2*zn^2 + a3*rho2 + a4*rho2^2 + a5*zn*rho2"
COEFFICIENT_UNIT = 1e-3  # physical coefficients are optimized in thousandths for well-scaled steps


# --- Data ------------------------------------------------------------------------------------------

class Dataset:
    """Sphere points of every usable scan, sampled to a fixed count per sphere, plus the measurands."""

    def __init__(self, names, diameters, distances, scans, points_per_sphere, seed):
        self.names = names
        self.reference_diameters = np.asarray(diameters, dtype=float)
        self.reference_distances = np.asarray(distances, dtype=float)
        self.scan_names = [scan["name"] for scan in scans]
        rng = np.random.default_rng(seed)

        count = len(names)
        sphere_points, sphere_weights, self.sphere_scan, self.sphere_label = [], [], [], []
        for scan_index, scan in enumerate(scans):
            for label in range(count):
                points = scan["spheres"][label]
                if points is None:
                    continue  # this scan does not show that sphere of the artifact
                if len(points) >= points_per_sphere:
                    chosen = points[rng.choice(len(points), points_per_sphere, replace=False)]
                    weights = np.ones(points_per_sphere)
                else:
                    # Padding carries zero weight, so every sphere tensor has the same shape.
                    chosen = np.zeros((points_per_sphere, 3))
                    chosen[: len(points)] = points
                    chosen[len(points):] = points[0]
                    weights = np.zeros(points_per_sphere)
                    weights[: len(points)] = 1.0
                sphere_points.append(chosen)
                sphere_weights.append(weights)
                self.sphere_scan.append(scan_index)
                self.sphere_label.append(label)
        self.points = torch.tensor(np.stack(sphere_points), dtype=torch.float64)
        self.weights = torch.tensor(np.stack(sphere_weights), dtype=torch.float64)
        self.sphere_scan = np.asarray(self.sphere_scan)
        self.sphere_label = np.asarray(self.sphere_label)

    @property
    def scan_count(self):
        return len(self.scan_names)

    def measurands(self, scan_indices):
        """Diameter and distance measurands of the given scans as index and reference arrays."""
        wanted = set(int(i) for i in scan_indices)
        diameter_sphere, diameter_reference = [], []
        pair_a, pair_b, pair_reference, pair_scan, diameter_scan = [], [], [], [], []
        by_scan = {}
        for sphere, (scan, label) in enumerate(zip(self.sphere_scan, self.sphere_label)):
            if scan in wanted:
                by_scan.setdefault(scan, {})[label] = sphere
        for scan, spheres in sorted(by_scan.items()):
            for label, sphere in spheres.items():
                if self.reference_diameters[label] > 0:
                    diameter_sphere.append(sphere)
                    diameter_reference.append(self.reference_diameters[label])
                    diameter_scan.append(scan)
            labels = sorted(spheres)
            for i, a in enumerate(labels):
                for b in labels[i + 1:]:
                    if self.reference_distances[a][b] > 0:
                        pair_a.append(spheres[a])
                        pair_b.append(spheres[b])
                        pair_reference.append(self.reference_distances[a][b])
                        pair_scan.append(scan)
        return {
            "diameter_sphere": torch.tensor(diameter_sphere, dtype=torch.long),
            "diameter_reference": torch.tensor(diameter_reference, dtype=torch.float64),
            "diameter_scan": np.asarray(diameter_scan, dtype=int),
            "pair_a": torch.tensor(pair_a, dtype=torch.long),
            "pair_b": torch.tensor(pair_b, dtype=torch.long),
            "pair_reference": torch.tensor(pair_reference, dtype=torch.float64),
            "pair_scan": np.asarray(pair_scan, dtype=int),
            "spheres": torch.tensor(sorted(s for scan in by_scan.values() for s in scan.values()), dtype=torch.long),
        }


def load_dataset(folder: Path, points_per_sphere: int, seed: int) -> Dataset:
    manifest = json.loads((folder / "manifest.json").read_text(encoding="utf-8"))
    artifact = manifest.get("artifact")
    if not artifact:
        raise ValueError("manifest.json has no artifact: export the training data with a reference artifact")
    names = [sphere["name"] for sphere in artifact["spheres"]]
    diameters = [float(sphere.get("diameter", 0.0)) for sphere in artifact["spheres"]]
    distances = np.zeros((len(names), len(names)))
    for distance in artifact.get("distances", []):
        a, b = names.index(distance["from"]), names.index(distance["to"])
        distances[a][b] = distances[b][a] = float(distance["value"])

    scans, skipped = [], []
    for scan in manifest["scans"]:
        groups = {name: [] for name in names}
        with open(folder / scan["points_file"], encoding="utf-8") as handle:
            for line in handle:
                parts = line.split()
                if len(parts) == 4 and parts[3] in groups:
                    groups[parts[3]].append([float(parts[0]), float(parts[1]), float(parts[2])])
        # A scan may show only some spheres of the artifact (a long artifact measured in parts): the
        # spheres it has give their diameters and the known distances between them.
        spheres = [np.asarray(groups[name]) if len(groups[name]) >= 10 else None for name in names]
        if all(points is None for points in spheres):
            skipped.append(scan["name"])
            continue
        scans.append({"name": scan["name"], "spheres": spheres})
    if skipped:
        print(f"Skipped {len(skipped)} scan(s) without any reference sphere: {', '.join(skipped)}", flush=True)
    if not scans:
        raise ValueError("no scan has a reference sphere")
    return Dataset(names, diameters, distances, scans, points_per_sphere, seed)


# --- Sphere fit ------------------------------------------------------------------------------------

def fit_spheres(points: torch.Tensor, weights: torch.Tensor, gauss_newton_steps: int = 3):
    """Weighted least-squares spheres for a batch (S, M, 3): algebraic fit, then geometric Gauss-Newton.

    Every operation is differentiable, so the loss can flow back to the corrected points."""
    total = weights.sum(1, keepdim=True)
    mean = (weights[..., None] * points).sum(1) / total
    q = points - mean[:, None, :]  # centered for conditioning

    # |q|^2 = 2 c.q + d, with r^2 = d + |c|^2.
    a = torch.cat([2.0 * q, torch.ones_like(q[..., :1])], dim=-1)
    b = (q * q).sum(-1)
    ata = torch.einsum("smi,sm,smj->sij", a, weights, a)
    atb = torch.einsum("smi,sm,sm->si", a, weights, b)
    solution = torch.linalg.solve(ata, atb)
    center = solution[:, :3]
    radius = torch.sqrt(torch.clamp(solution[:, 3] + (center * center).sum(-1), min=1e-12))

    eye = torch.eye(4, dtype=points.dtype, device=points.device)
    for _ in range(gauss_newton_steps):
        d = q - center[:, None, :]
        length = torch.sqrt((d * d).sum(-1) + 1e-18)
        residual = length - radius[:, None]
        jacobian = torch.cat([-d / length[..., None], -torch.ones_like(length[..., None])], dim=-1)
        jtj = torch.einsum("smi,sm,smj->sij", jacobian, weights, jacobian) + 1e-12 * eye
        jtr = torch.einsum("smi,sm,sm->si", jacobian, weights, residual)
        step = torch.linalg.solve(jtj, -jtr)
        center = center + step[:, :3]
        radius = radius + step[:, 3]
    return center + mean, radius


def measurand_errors(points, weights, measurands):
    """Errors (measured - reference, in mm) of the diameters and center distances."""
    center, radius = fit_spheres(points, weights)
    diameters = 2.0 * radius[measurands["diameter_sphere"]] - measurands["diameter_reference"]
    distances = (center[measurands["pair_a"]] - center[measurands["pair_b"]]).norm(dim=-1) - measurands["pair_reference"]
    return diameters, distances


# --- Model -----------------------------------------------------------------------------------------

class Compensation(nn.Module):
    def __init__(self, z_center, z_scale, lower, upper, hidden, output_bound, use_network, fixed_scale):
        super().__init__()
        self.register_buffer("z_center", torch.tensor(float(z_center), dtype=torch.float64))
        self.register_buffer("z_scale", torch.tensor(float(z_scale), dtype=torch.float64))
        self.register_buffer("lower", torch.tensor(lower, dtype=torch.float64))
        self.register_buffer("upper", torch.tensor(upper, dtype=torch.float64))
        self.output_bound = float(output_bound)
        self.use_network = use_network
        coefficients = torch.zeros(6, dtype=torch.float64)
        coefficients[0] = (fixed_scale - 1.0) / COEFFICIENT_UNIT  # start from the fixed scale
        self.coefficients = nn.Parameter(coefficients)
        self.network = nn.Sequential(
            nn.Linear(3, hidden), nn.Tanh(), nn.Linear(hidden, hidden), nn.Tanh(), nn.Linear(hidden, 3)
        ).double()
        # The last layer starts at zero, so training starts from the physical block alone.
        nn.init.zeros_(self.network[-1].weight)
        nn.init.zeros_(self.network[-1].bias)

    def scale(self, p):
        a = self.coefficients * COEFFICIENT_UNIT
        zn = (p[..., 2] - self.z_center) / self.z_scale
        rho2 = (p[..., 0] ** 2 + p[..., 1] ** 2) / (p[..., 2] ** 2)
        return a[0] + a[1] * zn + a[2] * zn ** 2 + a[3] * rho2 + a[4] * rho2 ** 2 + a[5] * zn * rho2

    def normalized(self, p):
        return torch.clamp(2.0 * (p - self.lower) / (self.upper - self.lower) - 1.0, -1.0, 1.0)

    def residual_from_normalized(self, u):
        return self.output_bound * torch.tanh(self.network(u))

    def residual(self, p):
        if not self.use_network:
            return torch.zeros_like(p)
        return self.residual_from_normalized(self.normalized(p))

    def forward(self, p):
        return p * (1.0 + self.scale(p))[..., None] + self.residual(p)


def fixed_scale(dataset: Dataset, measurands) -> float:
    with torch.no_grad():
        center, radius = fit_spheres(dataset.points, dataset.weights)
    measured = torch.cat([2.0 * radius[measurands["diameter_sphere"]],
                          (center[measurands["pair_a"]] - center[measurands["pair_b"]]).norm(dim=-1)])
    reference = torch.cat([measurands["diameter_reference"], measurands["pair_reference"]])
    return float((measured * reference).sum() / (measured * measured).sum())


def train(dataset: Dataset, measurands, options, use_network: bool, scale_start: float, device):
    spheres = measurands["spheres"]
    points = dataset.points[spheres].to(device)
    weights = dataset.weights[spheres].to(device)
    # Re-index the measurands to the selected spheres.
    position = {int(s): i for i, s in enumerate(spheres.tolist())}
    local = {
        "diameter_sphere": torch.tensor([position[int(s)] for s in measurands["diameter_sphere"]], dtype=torch.long, device=device),
        "diameter_reference": measurands["diameter_reference"].to(device),
        "pair_a": torch.tensor([position[int(s)] for s in measurands["pair_a"]], dtype=torch.long, device=device),
        "pair_b": torch.tensor([position[int(s)] for s in measurands["pair_b"]], dtype=torch.long, device=device),
        "pair_reference": measurands["pair_reference"].to(device),
    }

    real = points[weights > 0]
    lower = real.min(0).values.cpu().numpy()
    upper = real.max(0).values.cpu().numpy()
    upper = np.maximum(upper, lower + 1e-6)
    z_center = float((lower[2] + upper[2]) / 2.0)
    z_scale = float(max((upper[2] - lower[2]) / 2.0, 1.0))
    model = Compensation(z_center, z_scale, lower, upper, options.hidden, options.output_bound_mm, use_network,
                         scale_start).to(device)

    groups = [{"params": [model.coefficients], "lr": options.physics_learning_rate}]
    if use_network:
        groups.append({"params": model.network.parameters(), "lr": options.learning_rate})
    optimizer = torch.optim.Adam(groups)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, options.epochs, eta_min=1e-3 * options.learning_rate)
    generator = torch.Generator(device="cpu").manual_seed(options.seed)

    for _ in range(options.epochs):
        optimizer.zero_grad()
        corrected = model(points)
        diameters, distances = measurand_errors(corrected, weights, local)
        loss = torch.cat([diameters, distances]).pow(2).mean()
        if use_network:
            residual = model.residual(real)
            loss = loss + options.regularization * residual.pow(2).sum(-1).mean()
            # Spatial smoothness over the whole training volume, not only where the spheres were.
            u = (torch.rand((1024, 3), generator=generator, dtype=torch.float64) * 2.0 - 1.0).to(device).requires_grad_(True)
            output = model.residual_from_normalized(u)
            gradient = sum(torch.autograd.grad(output[:, k].sum(), u, create_graph=True)[0].pow(2).sum(-1).mean()
                           for k in range(3))
            loss = loss + options.smoothness * gradient
        loss.backward()
        optimizer.step()
        scheduler.step()
    return model


def evaluate(model, dataset: Dataset, measurands, device):
    """Measurand errors (mm) of the given scans after the model; model None means no correction."""
    with torch.no_grad():
        points = dataset.points.to(device)
        if model is not None:
            points = model(points)
        diameters, distances = measurand_errors(points, dataset.weights.to(device),
                                                {k: (v.to(device) if torch.is_tensor(v) else v) for k, v in measurands.items()})
    return diameters.cpu().numpy(), distances.cpu().numpy()


# --- Statistics ------------------------------------------------------------------------------------

def statistics(diameters, distances):
    errors = np.concatenate([diameters, distances]) * 1000.0
    absolute = np.abs(errors)
    return {
        "rms_um": float(np.sqrt(np.mean(errors ** 2))) if errors.size else 0.0,
        "diameter_rms_um": float(np.sqrt(np.mean((diameters * 1000.0) ** 2))) if diameters.size else 0.0,
        "distance_rms_um": float(np.sqrt(np.mean((distances * 1000.0) ** 2))) if distances.size else 0.0,
        "median_abs_um": float(np.median(absolute)) if errors.size else 0.0,
        "p95_abs_um": float(np.percentile(absolute, 95)) if errors.size else 0.0,
        "count": int(errors.size),
    }


def per_scan_rms(diameters, distances, measurands):
    rms = {}
    scans = np.concatenate([measurands["diameter_scan"], measurands["pair_scan"]])
    errors = np.concatenate([diameters, distances]) * 1000.0
    for scan in np.unique(scans):
        rms[int(scan)] = float(np.sqrt(np.mean(errors[scans == scan] ** 2)))
    return rms


# --- Workflow --------------------------------------------------------------------------------------

def cross_validate(dataset: Dataset, options, device):
    folds = min(options.folds, dataset.scan_count)
    if folds < 2:
        print("Cross-validation needs two or more scans; skipping it.", flush=True)
        return None
    order = np.random.default_rng(options.seed).permutation(dataset.scan_count)
    assignment = {int(scan): fold for fold, scans in enumerate(np.array_split(order, folds)) for scan in scans}

    collected = {name: ([], []) for name in ("fixed_scale", "physics_only", "pinn")}
    per_scan = {name: {} for name in collected}
    folds_report = []
    for fold in range(folds):
        train_scans = [s for s in range(dataset.scan_count) if assignment[s] != fold]
        test_scans = [s for s in range(dataset.scan_count) if assignment[s] == fold]
        train_measurands = dataset.measurands(train_scans)
        test_measurands = dataset.measurands(test_scans)
        start = time.monotonic()

        k = fixed_scale(dataset, train_measurands)
        raw_diameters, raw_distances = evaluate(None, dataset, test_measurands, device)
        reference = torch.cat([test_measurands["diameter_reference"], test_measurands["pair_reference"]]).numpy()
        measured = np.concatenate([raw_diameters, raw_distances]) + reference
        scaled = k * measured - reference
        results = {"fixed_scale": (scaled[: raw_diameters.size], scaled[raw_diameters.size:])}

        physics = train(dataset, train_measurands, options, False, k, device)
        results["physics_only"] = evaluate(physics, dataset, test_measurands, device)
        pinn = train(dataset, train_measurands, options, True, k, device)
        results["pinn"] = evaluate(pinn, dataset, test_measurands, device)

        report = {"fold": fold + 1, "test_scans": [dataset.scan_names[s] for s in test_scans],
                  "a0": float(pinn.coefficients[0].item() * COEFFICIENT_UNIT), "fixed_scale": k}
        for name, (d, t) in results.items():
            collected[name][0].append(d)
            collected[name][1].append(t)
            per_scan[name].update(per_scan_rms(d, t, test_measurands))
            report[name + "_rms_um"] = statistics(d, t)["rms_um"]
        folds_report.append(report)
        print(f"Fold {fold + 1}/{folds} ({len(test_scans)} test scan(s), {time.monotonic() - start:.1f} s): "
              f"fixed scale {report['fixed_scale_rms_um']:.1f} um, physics only {report['physics_only_rms_um']:.1f} um, "
              f"PINN {report['pinn_rms_um']:.1f} um, a0 {report['a0']:.5f}", flush=True)

    summary = {name: statistics(np.concatenate(d), np.concatenate(t)) for name, (d, t) in collected.items()}
    improved = sum(1 for scan, rms in per_scan["pinn"].items() if rms < per_scan["fixed_scale"][scan])
    summary["scans_improved_over_fixed_scale"] = f"{improved}/{dataset.scan_count}"
    summary["folds"] = folds_report
    return summary


def export(model: Compensation, path: Path, dataset: Dataset, options, k, validation, in_sample, calibration):
    layers = []
    for module in model.network:
        if isinstance(module, nn.Linear):
            layers.append({"weight": module.weight.detach().cpu().tolist(), "bias": module.bias.detach().cpu().tolist()})
    # Points inside the covered volume and their corrections, so an evaluator can verify on load that it
    # reproduces this model exactly.
    lower, upper = model.lower.cpu().numpy(), model.upper.cpu().numpy()
    samples = lower + np.random.default_rng(options.seed).uniform(0.05, 0.95, size=(8, 3)) * (upper - lower)
    with torch.no_grad():
        corrected = model(torch.tensor(samples, dtype=torch.float64, device=model.lower.device)).cpu().numpy()
    document = {
        "format": FORMAT,
        "version": FORMAT_VERSION,
        "model": "pinn-smcp",
        "units": "mm",
        "frame": "camera (raw reconstruction: camera at the origin, z = depth)",
        "formula": "p' = p * (1 + s) + r",
        "physics": {
            "form": PHYSICS_FORM,
            "zn": "(z - z_center) / z_scale",
            "rho2": "(x^2 + y^2) / z^2",
            "coefficients": [float(c) * COEFFICIENT_UNIT for c in model.coefficients.detach().cpu().tolist()],
            "z_center": float(model.z_center),
            "z_scale": float(model.z_scale),
        },
        "network": {
            "input": "u = clamp(2 * (p - lower) / (upper - lower) - 1, -1, 1)",
            "hidden_activation": "tanh",
            "output": "r = output_bound_mm * tanh(W3 * h2 + b3)",
            "lower": model.lower.cpu().tolist(),
            "upper": model.upper.cpu().tolist(),
            "output_bound_mm": model.output_bound,
            "layers": layers,
            # Beyond the covered volume the network is not trusted: evaluators fade it linearly to zero
            # over this fraction of the volume size outside each face, leaving the physical block.
            "extrapolation_margin": 0.1,
        },
        "test_vectors": {"input": samples.tolist(), "output": corrected.tolist()},
        "fixed_scale": k,
        "calibration_sha256": calibration,
        "training": {
            "created": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "scans": dataset.scan_names,
            "artifact": {"names": dataset.names, "diameters": dataset.reference_diameters.tolist()},
            "options": vars(options) | {"dataset": str(options.dataset), "output": str(options.output)},
            "in_sample": in_sample,
            "cross_validation": validation,
        },
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, default=str), encoding="utf-8")


def run(options) -> int:
    device = torch.device("cuda" if options.device == "cuda" or (options.device == "auto" and torch.cuda.is_available())
                          else "cpu")
    torch.manual_seed(options.seed)
    dataset = load_dataset(Path(options.dataset), options.points_per_sphere, options.seed)
    print(f"Loaded {dataset.scan_count} scan(s) with {len(dataset.sphere_scan)} spheres of the artifact "
          f"({', '.join(dataset.names)}); device {device}.", flush=True)

    validation = cross_validate(dataset, options, device)
    all_measurands = dataset.measurands(range(dataset.scan_count))
    k = fixed_scale(dataset, all_measurands)
    model = train(dataset, all_measurands, options, True, k, device)
    raw = evaluate(None, dataset, all_measurands, device)
    in_sample = {"uncorrected": statistics(*raw), "pinn": statistics(*evaluate(model, dataset, all_measurands, device))}

    calibration = None
    if options.calibration:
        calibration = hashlib.sha256(Path(options.calibration).read_bytes()).hexdigest()
    export(model, Path(options.output), dataset, options, k, validation, in_sample, calibration)

    coefficients = ", ".join(f"{c * COEFFICIENT_UNIT:+.6f}" for c in model.coefficients.detach().cpu().tolist())
    print(f"Final model on all scans: fixed scale {k:.6f}; coefficients a0..a5 = {coefficients}.", flush=True)
    if validation:
        print("Cross-validation (each scan tested by a model that did not see it): "
              f"fixed scale {validation['fixed_scale']['rms_um']:.1f} um, "
              f"physics only {validation['physics_only']['rms_um']:.1f} um, "
              f"PINN {validation['pinn']['rms_um']:.1f} um; PINN better than the fixed scale on "
              f"{validation['scans_improved_over_fixed_scale']} scans.", flush=True)
    print(f"Wrote {options.output}", flush=True)
    return 0


# --- Synthetic self-test ---------------------------------------------------------------------------

def self_test(options) -> int:
    """Builds a synthetic campaign with a known distortion and checks that the model recovers it."""
    import tempfile

    rng = np.random.default_rng(options.seed)
    names = ["A", "B", "C"]
    layout = np.array([[0.0, 0.0, 0.0], [220.0, 20.0, 0.0], [90.0, 170.0, -30.0]])
    radius = 50.0

    def distort(p):
        # Depth-dependent scale (what the physical block can model) plus a smooth lateral field.
        zn = (p[:, 2] - 950.0) / 150.0
        rho2 = (p[:, 0] ** 2 + p[:, 1] ** 2) / p[:, 2] ** 2
        s = 0.008 + 0.0015 * zn + 0.004 * rho2
        field = 0.12 * np.stack([np.sin(p[:, 1] / 90.0), np.cos(p[:, 0] / 110.0), np.sin((p[:, 0] + p[:, 1]) / 150.0)], 1)
        return p * (1.0 + s)[:, None] + field

    folder = Path(tempfile.mkdtemp(prefix="smcp-pinn-selftest-"))
    scans = []
    for index in range(options.self_test_scans):
        offset = np.array([rng.uniform(-250, 150), rng.uniform(-200, 50), rng.uniform(800, 1100)])
        lines = []
        for label, center in zip(names, layout + offset):
            directions = rng.normal(size=(4000, 3))
            directions /= np.linalg.norm(directions, axis=1, keepdims=True)
            toward_camera = -center / np.linalg.norm(center)
            directions = directions[directions @ toward_camera > 0.1]
            points = center + (radius + rng.normal(0.0, 0.05, len(directions)))[:, None] * directions
            for x, y, z in distort(points):
                lines.append(f"{x:.6f} {y:.6f} {z:.6f} {label}")
        name = f"synthetic_{index + 1:02d}"
        (folder / f"{name}.spheres.xyz").write_text("\n".join(lines) + "\n", encoding="utf-8")
        scans.append({"name": name, "points_file": f"{name}.spheres.xyz"})
    distances = [{"from": names[i], "to": names[j], "value": float(np.linalg.norm(layout[i] - layout[j]))}
                 for i in range(3) for j in range(i + 1, 3)]
    manifest = {"artifact": {"spheres": [{"name": n, "diameter": 2 * radius} for n in names], "distances": distances},
                "scans": scans}
    (folder / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

    options.dataset = folder
    options.output = folder / "compensation.json"
    run(options)
    document = json.loads(Path(options.output).read_text(encoding="utf-8"))
    validation = document["training"]["cross_validation"]
    a0 = document["physics"]["coefficients"][0]
    fixed, pinn = validation["fixed_scale"]["rms_um"], validation["pinn"]["rms_um"]
    checks = {
        "global scale recovered (a0 near -0.008)": abs(a0 + 0.008) < 0.0015,
        "PINN beats the fixed scale on unseen scans": pinn < 0.6 * fixed,
        "PINN beats the physical block alone": pinn <= validation["physics_only"]["rms_um"] * 1.05,
    }
    for label, passed in checks.items():
        print(f"[{'PASS' if passed else 'FAIL'}] {label}", flush=True)
    print(f"Self-test data and model: {folder}", flush=True)
    return 0 if all(checks.values()) else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dataset", help="folder exported by 'Export Training Data' (manifest.json)")
    parser.add_argument("--output", help="compensation model to write (.json)")
    parser.add_argument("--calibration", help="calibration.yml the scans were reconstructed with (stored as a hash)")
    parser.add_argument("--folds", type=int, default=5)
    # 400 epochs: on the campaigns available so far (10 scans) longer training fits the training scans
    # better but the scans left out of it worse.
    parser.add_argument("--epochs", type=int, default=400)
    parser.add_argument("--hidden", type=int, default=32)
    parser.add_argument("--points-per-sphere", type=int, default=600)
    parser.add_argument("--learning-rate", type=float, default=1e-3)
    parser.add_argument("--physics-learning-rate", type=float, default=2e-2)
    parser.add_argument("--regularization", type=float, default=1e-2)
    parser.add_argument("--smoothness", type=float, default=1e-2)
    parser.add_argument("--output-bound-mm", type=float, default=0.5)
    parser.add_argument("--seed", type=int, default=2026)
    parser.add_argument("--device", choices=["auto", "cpu", "cuda"], default="cpu")
    parser.add_argument("--self-test", action="store_true", help="train on a synthetic campaign with a known error")
    parser.add_argument("--self-test-scans", type=int, default=12)
    return parser


def main() -> int:
    options = build_parser().parse_args()
    try:
        if options.self_test:
            return self_test(options)
        if not options.dataset or not options.output:
            print("error: --dataset and --output are required", file=sys.stderr)
            return 2
        return run(options)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
