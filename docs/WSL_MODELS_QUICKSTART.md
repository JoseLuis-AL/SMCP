# WSL models quick start

| Component | Validated configuration | Notes |
| --- | --- | --- |
| WSL distribution | Ubuntu 22.04.5 LTS (Jammy) | WSL 2 |
| WSL kernel | `6.18.33.2-microsoft-standard-WSL2` | Newer WSL 2 kernels should also work |
| Miniconda / conda | Miniconda, conda `26.7.1` | Installed in `~/miniconda3` |
| Python | Base `3.14.7`; models `3.11.16` | Environment definition permits Python `3.11.x` |
| `smcp` launcher | System `python3` `3.10.12` | Uses only the standard library; independent of Conda |
| GPU | NVIDIA GeForce RTX 4050 Laptop GPU, 6 GB | Windows driver `610.88` in the validated machine |
| PyTorch / CUDA runtime | PyTorch `2.8.0+cu128`; CUDA `12.8` | CUDA is supplied through the PyTorch wheel |
| Numerical stack | NumPy `1.26.4`, SciPy `1.17.1`, scikit-learn `1.9.1` (denoising models) | Later compatible solver builds may be selected |
| Conda environments | `model-score-denoise`, `model-straightPCF`, `pointcleannet`, `model-pinn-smcp` | One isolated environment per model or trainer |
| Model root | `~/smcp-models` | Contains `score-denoise`, `straightPCF`, `pointcleannet`, and the trainer `pinn-smcp` |
| Model registry | `~/smcp-models/models.json` | Editable defaults, schemas, environments, runners, and output suffixes |

This guide recreates the WSL and conda side of the SMCP AI module. The setup is
automated by [`scripts-wsl`](../scripts-wsl/README.md). Model source code,
checkpoints, and other large recovered artifacts are not downloaded by these
scripts; place them in the model root as described below.

## Quick start

### 1. Prepare WSL and the NVIDIA driver

From an elevated Windows PowerShell terminal:

```powershell
wsl --install -d Ubuntu-22.04
wsl --update
wsl --list --verbose
```

The Ubuntu entry must show WSL version `2`. These commands follow Microsoft's
[official WSL installation instructions](https://learn.microsoft.com/windows/wsl/install).

For GPU inference, install or update the NVIDIA Windows driver. Do **not**
install a Linux NVIDIA display driver inside WSL: the Windows host driver is
exposed to WSL. See NVIDIA's
[CUDA on WSL guidance](https://docs.nvidia.com/cuda/cuda-quick-start-guide/).

### 2. Run the automated setup

Open Ubuntu 22.04, change to the Windows checkout, and run:

```bash
cd /mnt/c/path/to/SMCP-CPP
bash scripts-wsl/setup.sh
exec bash
```

Replace `/mnt/c/path/to/SMCP-CPP` with the actual repository path. The setup:

1. Installs Miniconda into `~/miniconda3` if it is absent.
2. Creates `~/smcp-models` and the five conda environments (three models, two trainers).
3. Installs the shared `smcp` launcher into `~/.local/bin` and creates
   `~/smcp-models/models.json` if it does not exist.
4. Checks Python, PyTorch, CUDA visibility, and the GPU.

The script is idempotent. It preserves existing environments by default.

### 3. Restore the model artifacts

Place each recovered, prepared model directory at exactly these paths:

```text
~/smcp-models/
├── models.json
├── score-denoise/
│   ├── run_score_denoise.py
│   └── model/pretrained/ckpt.pt
├── straightPCF/
│   ├── run_straightpcf.py
│   ├── model/pretrained_straightpcf/ckpt_straightpcf.pt
│   └── model/pretrained_cvm/ckpt_cvm.pt
└── pointcleannet/
    ├── run_pointcleannet.py
    └── model/models/
        ├── PointCleanNet_params.pth
        └── PointCleanNet_model.pth
```

Each directory must also contain its complete recovered source tree and support
files. The launcher contract for every model runner is:

```bash
python MODEL_RUNNER.py --input INPUT.xyz --output OUTPUT.xyz
```

Once the artifacts are in place, perform the strict check:

```bash
bash scripts-wsl/verify.sh --strict
```

### 4. Run a model

Inspect the installed and validated models with:

```bash
smcp --validate-registry --json
smcp --list-models --json
smcp --describe-model score-denoise --json
```

To override inference parameters, create a JSON object containing any subset of
the registered configuration. Unknown keys, wrong types, and out-of-range
values are rejected before Conda starts:

```json
{
  "device": "auto",
  "ld_num_steps": 50,
  "cluster_size": 8000,
  "denoise_knn": 8
}
```

Then pass it to the launcher:

```bash
smcp --model score-denoise \
  --input ~/example_piece.xyz \
  --output ~/output \
  --config-json ~/score-settings.json
```

The `--output` argument may be either an output directory or a filename hint.
The launcher always applies the model suffix required by SMCP:

```bash
mkdir -p ~/output

smcp --model score-denoise \
  --input ~/example_piece.xyz \
  --output ~/output

smcp --model straightPCF \
  --input ~/example_piece.xyz \
  --output ~/output

smcp --model pointcleannet \
  --input ~/example_piece.xyz \
  --output ~/output
```

For `example_piece.xyz`, the results are:

| Model argument | Output filename |
| --- | --- |
| `score-denoise` | `example_piece_score_denoise.xyz` |
| `straightPCF` | `example_piece_straightPCF.xyz` |
| `pointcleannet` | `example_piece_pointcleannet.xyz` |

Open the result directory from WSL with:

```bash
explorer.exe "$(wslpath -w ~/output)"
```

### Run models from SMCP

When the Point Cloud Editor opens, SMCP lists the models returned by
`smcp --list-models --json` in its **PointCloud > AI Models** submenu, which stays disabled if
none is available. The chosen model runs on every selected cloud, one after another, while a
*Thinking...* window shows the elapsed time; see the [User guide](USER_GUIDE.md#run-an-ai-model).

SMCP passes `--job-id` to every inference. With a job identifier the launcher runs the model
under a supervisor in its own Linux session:

- `smcp --cancel-job JOB_ID` stops the whole process tree (`conda run` and the model); and
- if the launcher disappears, for example because SMCP was closed from Task Manager or
  `wsl.exe` was terminated, the supervisor notices and stops the model within about ten
  seconds instead of leaving it on the GPU.

Running jobs are registered in `/tmp/smcp-jobs`. The directory is empty when no inference is
running.

## Train the SMCP compensation (PINN)

Besides the point-cloud models, the launcher runs **trainers**. The `pinn-smcp` trainer fits a
physics-informed correction of the SMCP reconstruction from sphere measurements:

1. In SMCP, measure a campaign of the reference artifact with **Measure Spheres** and click
   **Export Training Data** (see the [User guide](USER_GUIDE.md#measure-reference-spheres)). Use raw
   reconstructions in the camera frame, captured in many positions that cover the volume.
2. In Ubuntu, train on that folder. Windows paths are available under `/mnt/c/...`:

   ```bash
   smcp --list-trainers --json
   smcp --train pinn-smcp --dataset /mnt/c/path/to/training-data         --output /mnt/c/path/to/compensation.json         --calibration /mnt/c/path/to/calibration.yml
   ```

   `--calibration` is optional: the trainer stores its SHA-256 so the correction can later be
   refused for scans reconstructed with a different calibration. Override the defaults
   (`folds`, `epochs`, `hidden`, `points_per_sphere`, `learning_rate`, `physics_learning_rate`,
   `regularization`, `smoothness`, `output_bound_mm`, `seed`, `device`) with `--config-json FILE`.

The model is `p' = p * (1 + s) + r`: a six-coefficient physical block `s` that moves each point
along its camera ray (global scale, its variation with depth, residual radial distortion, and
their coupling) plus a small network `r` bounded to ±`output_bound_mm` per axis, regularized and
smoothed, and limited to the volume covered by the training spheres. The loss compares the
diameters and center distances of the spheres refitted on the corrected points with the CMM
values. The global scale starts at the fixed scale correction.

The trainer prints, per fold, the error of each held-out scan under three corrections: the fixed
scale, the physical block alone, and the full PINN. Only these cross-validated numbers estimate
the error on new scans; the in-sample numbers stored in the output are optimistic. The output
JSON holds the coefficients, the network weights, the covered volume, the fixed scale, and the
validation summary, plus test points that SMCP uses to verify it reproduces the model exactly.
Apply it in SMCP to reconstructions, editor layers, or a measured campaign; see
[Compensate reconstructions](USER_GUIDE.md#compensate-reconstructions).

A scan does not need to show every sphere of the artifact. A long artifact measured in parts
can be trained as one: describe all its spheres and known distances in the manifest, and each
scan contributes the diameters and distances of the spheres it has.

The default is 400 epochs. With the ten scans available when it was chosen, longer training
fitted the training scans better (80 um with 1500 epochs) but the scans left out of it worse
(169 um, the same as the fixed scale, against 148 um with 400 epochs). Check the cross-validation
line of the log again when more scans are available.

To check the installation, run the synthetic self-test, which injects a known distortion and
verifies that the global scale is recovered and that the PINN beats the fixed scale on unseen
scans (about two minutes on the CPU):

```bash
~/miniconda3/envs/model-pinn-smcp/bin/python ~/smcp-models/pinn-smcp/train_pinn_smcp.py --self-test --epochs 600
```

## Script reference

| Script | Purpose |
| --- | --- |
| `scripts-wsl/setup.sh` | Runs the complete setup in the correct order |
| `scripts-wsl/install-conda.sh` | Installs Miniconda and initializes bash |
| `scripts-wsl/create-model-envs.sh` | Creates or deliberately recreates all model and trainer environments |
| `scripts-wsl/install-smcp-cli.sh` | Installs the shared launcher in `~/.local/bin` and the trainers in `~/smcp-models` |
| `scripts-wsl/verify.sh` | Checks WSL, conda, Python dependencies, CUDA, GPU, CLI, and model directories |

To discard and fully rebuild only the named model and trainer environments:

```bash
bash scripts-wsl/create-model-envs.sh --recreate
```

`--recreate` removes `model-score-denoise`, `model-straightPCF`, `pointcleannet`, and
`model-pinn-smcp`; it does not remove Miniconda, model files, inputs, or outputs.

Re-running `install-smcp-cli.sh` updates the CLI but deliberately preserves an
existing `models.json`, including user-tuned defaults (a registry without a `trainers` array
uses the trainers shipped with the launcher). Trainer scripts are always updated. Compare it with
`scripts-wsl/models.json` manually when upgrading the registry schema.
Re-run it after every SMCP update so `~/.local/bin/smcp` matches the application; for
example, job supervision and automatic cleanup require the launcher from version 3.0.2:

```bash
bash scripts-wsl/install-smcp-cli.sh
```

Environment creation uses `conda-forge` plus `nodefaults`, then installs the
CUDA 12.8 PyTorch wheels from PyTorch and the matching `torch-cluster` wheel
from PyG. StraightPCF additionally receives `torch-geometric`. The `model-pinn-smcp` trainer
environment only needs NumPy and the CPU build of PyTorch.

## Manual environment inspection

Use `conda run` when you want to test an environment without activating it:

```bash
~/miniconda3/bin/conda env list

~/miniconda3/bin/conda run -n model-score-denoise \
  python -c "import torch; print(torch.__version__, torch.cuda.is_available())"

~/miniconda3/bin/conda run -n model-straightPCF \
  python -c "import torch_geometric; print(torch_geometric.__version__)"
```

## Troubleshooting

### `CondaToSNonInteractiveError` while creating an environment

Recent conda versions refuse to create environments while the Terms of Service of the Anaconda
default channels (`repo.anaconda.com/pkgs/main` and `pkgs/r`) have not been accepted. The
`model-pinn-smcp` trainer environment avoids those channels: `create-model-envs.sh`
creates it with `--override-channels` from conda-forge only (PyTorch for the PINN trainer is
installed with pip). For the other model
environments, either review and accept those terms yourself with the `conda tos accept` commands
that the error prints, or remove the default channels from your conda configuration
(`conda config --remove channels defaults`) so only conda-forge is used.

### `smcp: command not found`

Open a new Ubuntu shell or run:

```bash
export PATH="$HOME/.local/bin:$PATH"
```

Then confirm the installation with `command -v smcp`.

### CUDA reports `False`

Run `wsl --update` from Windows, update the Windows NVIDIA driver, restart WSL
with `wsl --shutdown`, and run `bash scripts-wsl/verify.sh` again. Do not install
an NVIDIA Linux display driver inside WSL.

### A model entrypoint is missing

The environment setup does not include recovered model code or weights. Confirm
that the selected model directory contains the runner and checkpoints listed in
the artifact tree above.

### A model keeps running after SMCP closed

Reinstall the launcher with `bash scripts-wsl/install-smcp-cli.sh`; older launchers did not
supervise their jobs. To inspect and stop leftover processes:

```bash
pgrep -fa "supervise-job|run_score_denoise|run_straightpcf|run_pointcleannet"
smcp --cancel-job JOB_ID
```

The job identifiers are the names of the `.pid` files in `/tmp/smcp-jobs`, without the extension.

### `explorer.exe` reports `Exec format error`

Windows interoperability is disabled or unhealthy. In Windows PowerShell run
`wsl --shutdown`, reopen Ubuntu, and try the `explorer.exe` command again. Also
check that `/proc/sys/fs/binfmt_misc/WSLInterop` exists inside WSL.
