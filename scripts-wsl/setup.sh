#!/usr/bin/env bash

set -euo pipefail

smcp_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

bash "${smcp_script_dir}/install-conda.sh"
bash "${smcp_script_dir}/create-model-envs.sh" "$@"
bash "${smcp_script_dir}/install-smcp-cli.sh"
bash "${smcp_script_dir}/verify.sh" --environments-only

printf '\nWSL model setup completed.\n'
printf 'Open a new Ubuntu shell (or run: exec bash) before using smcp.\n'
printf 'Then restore the model artifacts and run: bash %s/verify.sh --strict\n' "${smcp_script_dir}"
