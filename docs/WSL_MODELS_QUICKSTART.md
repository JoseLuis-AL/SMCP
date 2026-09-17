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
| Numerical stack | NumPy `1.26.4`, SciPy `1.17.1`, scikit-learn `1.9.1` | Later compatible solver builds may be selected |
| Conda environments | `model-score-denoise`, `model-straightPCF`, `pointcleannet` | One isolated environment per model |
| Model root | `~/smcp-models` | Contains `score-denoise`, `straightPCF`, and `pointcleannet` |
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
2. Creates `~/smcp-models` and the three conda environments.
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
`smcp --list-models --json` in its **AI Model** selector, which stays disabled if none is
available. The selected model runs on the first point cloud of the overlay while a
*Thinking...* window shows the elapsed time; see the [User guide](USER_GUIDE.md#run-an-ai-model).

SMCP passes `--job-id` to every inference. With a job identifier the launcher runs the model
under a supervisor in its own Linux session:

- `smcp --cancel-job JOB_ID` stops the whole process tree (`conda run` and the model); and
- if the launcher disappears, for example because SMCP was closed from Task Manager or
  `wsl.exe` was terminated, the supervisor notices and stops the model within about ten
  seconds instead of leaving it on the GPU.

Running jobs are registered in `/tmp/smcp-jobs`. The directory is empty when no inference is
running.

## Script reference

| Script | Purpose |
| --- | --- |
| `scripts-wsl/setup.sh` | Runs the complete setup in the correct order |
| `scripts-wsl/install-conda.sh` | Installs Miniconda and initializes bash |
| `scripts-wsl/create-model-envs.sh` | Creates or deliberately recreates all model environments |
| `scripts-wsl/install-smcp-cli.sh` | Installs the shared launcher in `~/.local/bin` |
| `scripts-wsl/verify.sh` | Checks WSL, conda, Python dependencies, CUDA, GPU, CLI, and model directories |

To discard and fully rebuild only the three named model environments:

```bash
bash scripts-wsl/create-model-envs.sh --recreate
```

`--recreate` removes `model-score-denoise`, `model-straightPCF`, and
`pointcleannet`; it does not remove Miniconda, model files, inputs, or outputs.

Re-running `install-smcp-cli.sh` updates the CLI but deliberately preserves an
existing `models.json`, including user-tuned defaults. Compare it with
`scripts-wsl/models.json` manually when upgrading the registry schema.
Re-run it after every SMCP update so `~/.local/bin/smcp` matches the application; for
example, job supervision and automatic cleanup require the launcher from version 3.0.2:

```bash
bash scripts-wsl/install-smcp-cli.sh
```

Environment creation uses `conda-forge` plus `nodefaults`, then installs the
CUDA 12.8 PyTorch wheels from PyTorch and the matching `torch-cluster` wheel
from PyG. StraightPCF additionally receives `torch-geometric`.

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
