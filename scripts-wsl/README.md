# WSL setup scripts

Run the complete setup from Ubuntu 22.04:

```bash
bash scripts-wsl/setup.sh
```

The scripts install Miniconda, create the three isolated model environments,
install the `smcp` launcher and editable `~/smcp-models/models.json` registry,
and verify the Python/CUDA stack. They are safe to
run again: existing installations and environments are preserved unless
`--recreate` is explicitly supplied.

See the [WSL models quick start](../docs/WSL_MODELS_QUICKSTART.md) for host
requirements, model-artifact placement, usage, and troubleshooting.

Run `smcp --list-models --json`, `smcp --describe-model MODEL --json`, or
`smcp --validate-registry --json` to inspect the same contract used by the
Windows application. Per-run parameter overrides are supplied with
`--config-json FILE`.

Re-run `bash scripts-wsl/install-smcp-cli.sh` after updating SMCP. The launcher tests run
with `python3 -m unittest discover -s scripts-wsl/tests -p "test_*.py"`.
