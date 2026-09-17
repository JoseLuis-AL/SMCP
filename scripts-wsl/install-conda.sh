#!/usr/bin/env bash

set -euo pipefail

smcp_user_home="${HOME:?HOME is not set}"
smcp_conda_dir="${SMCP_CONDA_DIR:-${smcp_user_home}/miniconda3}"
smcp_installer_url="https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh"

if [[ "$(uname -s)" != "Linux" ]] || ! grep -qi microsoft /proc/version 2>/dev/null; then
    printf 'Error: run this script inside WSL.\n' >&2
    exit 1
fi

if [[ "$(uname -m)" != "x86_64" ]]; then
    printf 'Error: this setup currently supports x86_64 WSL only.\n' >&2
    exit 1
fi

if [[ ! -r /etc/os-release ]]; then
    printf 'Error: /etc/os-release is unavailable; Ubuntu 22.04 cannot be verified.\n' >&2
    exit 1
fi

# shellcheck disable=SC1091
source /etc/os-release
if [[ "${ID:-}" != "ubuntu" ]] || [[ "${VERSION_ID:-}" != "22.04" ]]; then
    printf 'Error: this setup targets Ubuntu 22.04; detected %s %s.\n' \
        "${ID:-unknown}" "${VERSION_ID:-unknown}" >&2
    exit 1
fi

if [[ -x "${smcp_conda_dir}/bin/conda" ]]; then
    printf 'Miniconda already exists at %s; keeping it.\n' "${smcp_conda_dir}"
else
    if ! command -v curl >/dev/null 2>&1; then
        printf 'Installing curl and CA certificates...\n'
        sudo apt-get update
        sudo apt-get install -y curl ca-certificates
    fi

    smcp_tmp_dir="$(mktemp -d)"
    trap 'rm -rf -- "${smcp_tmp_dir}"' EXIT

    printf 'Downloading Miniconda...\n'
    curl --fail --location --retry 3 \
        --output "${smcp_tmp_dir}/miniconda.sh" \
        "${smcp_installer_url}"

    printf 'Installing Miniconda at %s...\n' "${smcp_conda_dir}"
    bash "${smcp_tmp_dir}/miniconda.sh" -b -p "${smcp_conda_dir}"
fi

"${smcp_conda_dir}/bin/conda" init bash >/dev/null
mkdir -p "${smcp_user_home}/smcp-models" "${smcp_user_home}/.local/bin"

printf 'Conda: %s\n' "$("${smcp_conda_dir}/bin/conda" --version)"
printf 'Model directory: %s\n' "${smcp_user_home}/smcp-models"
