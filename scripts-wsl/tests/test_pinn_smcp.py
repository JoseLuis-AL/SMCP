"""Unit tests of the PINN compensation trainer; skipped when PyTorch is not installed."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path

try:
    import numpy as np
    import torch
except ImportError:  # the launcher tests run with the system Python, which has no PyTorch
    torch = None

TRAINER = Path(__file__).resolve().parents[1] / "trainers/pinn-smcp/train_pinn_smcp.py"


@unittest.skipIf(torch is None, "PyTorch is not installed")
class PinnTrainerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("train_pinn_smcp", TRAINER)
        cls.trainer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.trainer)

    def sphere(self, center, radius, count=800, seed=0):
        rng = np.random.default_rng(seed)
        directions = rng.normal(size=(count, 3))
        directions /= np.linalg.norm(directions, axis=1, keepdims=True)
        directions = directions[directions[:, 2] < 0.2]  # a cap, as a camera sees it
        return np.asarray(center) + radius * directions

    def test_batched_sphere_fit_recovers_center_and_radius(self):
        points = [self.sphere((10, -5, 900), 50, seed=1), self.sphere((-40, 30, 1000), 8, seed=2)]
        count = min(len(p) for p in points)
        tensor = torch.tensor(np.stack([p[:count] for p in points]), dtype=torch.float64)
        center, radius = self.trainer.fit_spheres(tensor, torch.ones(tensor.shape[:2], dtype=torch.float64))
        np.testing.assert_allclose(center.numpy(), [[10, -5, 900], [-40, 30, 1000]], atol=1e-6)
        np.testing.assert_allclose(radius.numpy(), [50, 8], atol=1e-6)

    def test_model_starts_from_the_fixed_scale(self):
        model = self.trainer.Compensation(950.0, 100.0, np.array([-1.0, -1, 800]), np.array([1.0, 1, 1100]), 8, 0.5,
                                          True, 0.99)
        point = torch.tensor([[12.0, -7.0, 950.0]], dtype=torch.float64)
        np.testing.assert_allclose(model(point).detach().numpy(), 0.99 * point.numpy(), atol=1e-12)

    def test_training_writes_an_evaluable_model(self):
        names = ["A", "B", "C"]
        layout = np.array([[0.0, 0, 0], [200, 0, 0], [80, 150, 0]])
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            scans = []
            for index, offset in enumerate([(0, 0, 900), (-60, 40, 1000), (40, -50, 950)]):
                lines = []
                for label, center in zip(names, layout + np.asarray(offset)):
                    for x, y, z in self.sphere(center, 50, count=600, seed=index) * 1.008:
                        lines.append(f"{x} {y} {z} {label}")
                (folder / f"s{index}.spheres.xyz").write_text("\n".join(lines), encoding="utf-8")
                scans.append({"name": f"s{index}", "points_file": f"s{index}.spheres.xyz"})
            distances = [{"from": names[i], "to": names[j], "value": float(np.linalg.norm(layout[i] - layout[j]))}
                         for i in range(3) for j in range(i + 1, 3)]
            (folder / "manifest.json").write_text(json.dumps({"artifact": {
                "spheres": [{"name": n, "diameter": 100.0} for n in names], "distances": distances}, "scans": scans}),
                encoding="utf-8")
            options = Namespace(dataset=folder, output=folder / "model.json", calibration=None, folds=3, epochs=40,
                                hidden=8, points_per_sphere=200, learning_rate=1e-3, physics_learning_rate=2e-2,
                                regularization=1e-2, smoothness=1e-2, output_bound_mm=0.5, seed=1, device="cpu")
            self.assertEqual(self.trainer.run(options), 0)
            document = json.loads((folder / "model.json").read_text(encoding="utf-8"))
        self.assertEqual(document["format"], "smcp-compensation")
        self.assertEqual(len(document["physics"]["coefficients"]), 6)
        self.assertAlmostEqual(document["fixed_scale"], 1 / 1.008, places=4)
        self.assertEqual([len(layer["weight"]) for layer in document["network"]["layers"]], [8, 8, 3])
        self.assertIn("pinn", document["training"]["cross_validation"])


if __name__ == "__main__":
    unittest.main()
