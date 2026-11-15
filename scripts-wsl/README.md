# WSL setup scripts

Run the complete setup from Ubuntu 22.04:

```bash
bash scripts-wsl/setup.sh
```

The scripts install Miniconda, create the isolated model and trainer environments,
install the `smcp` launcher, the trainers, and the editable `~/smcp-models/models.json` registry,
and verify the Python/CUDA stack. They are safe to
run again: existing installations and environments are preserved unless
`--recreate` is explicitly supplied.

See the [WSL models quick start](../docs/WSL_MODELS_QUICKSTART.md) for host
requirements, model-artifact placement, usage, and troubleshooting.

Run `smcp --list-models --json`, `smcp --describe-model MODEL --json`, or
`smcp --validate-registry --json` to inspect the same contract used by the
Windows application. Per-run parameter overrides are supplied with
`--config-json FILE`. `smcp --list-trainers --json` and
`smcp --train pinn-smcp --dataset DIR --output FILE.json` train the SMCP compensation; see the
quick start.

## Trainers

`smcp --list-trainers --json` reports every trainer with its configuration and schema. A trainer
entry of the registry may set `output_extension` (default `.json`; the trained model path passed
to `--output` gets it). Trainers shipped with SMCP that an existing `~/smcp-models/models.json`
does not list are added automatically; list one with `"enabled": false` to hide it.
