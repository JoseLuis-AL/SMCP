#!/usr/bin/env bash

set -euo pipefail

smcp_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
smcp_user_home="${HOME:?HOME is not set}"
smcp_bin_dir="${smcp_user_home}/.local/bin"
smcp_model_dir="${smcp_user_home}/smcp-models"

mkdir -p "${smcp_bin_dir}" "${smcp_model_dir}"
install -m 0755 "${smcp_script_dir}/smcp" "${smcp_bin_dir}/smcp"
if [[ ! -f "${smcp_model_dir}/models.json" ]]; then
    install -m 0644 "${smcp_script_dir}/models.json" "${smcp_model_dir}/models.json"
    printf 'Installed the editable model registry at %s/models.json\n' "${smcp_model_dir}"
else
    printf 'Keeping the existing model registry at %s/models.json\n' "${smcp_model_dir}"
fi

printf 'Installed smcp at %s/smcp\n' "${smcp_bin_dir}"
case ":${PATH}:" in
    *":${smcp_bin_dir}:"*) ;;
    *) printf 'Open a new Ubuntu shell so ~/.local/bin is added to PATH.\n' ;;
esac
