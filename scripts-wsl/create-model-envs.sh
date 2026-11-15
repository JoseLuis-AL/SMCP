#!/usr/bin/env bash

set -euo pipefail

smcp_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
smcp_user_home="${HOME:?HOME is not set}"
smcp_conda_dir="${SMCP_CONDA_DIR:-${smcp_user_home}/miniconda3}"
smcp_conda_exe="${SMCP_CONDA_EXE:-${smcp_conda_dir}/bin/conda}"
smcp_recreate=false

usage() {
    printf 'Usage: %s [--recreate]\n' "$(basename -- "$0")"
    printf '  --recreate  Remove and rebuild only the SMCP model and trainer environments.\n'
}

for smcp_arg in "$@"; do
    case "${smcp_arg}" in
        --recreate) smcp_recreate=true ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Unknown option: %s\n' "${smcp_arg}" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ ! -x "${smcp_conda_exe}" ]]; then
    printf 'Error: conda was not found at %s. Run install-conda.sh first.\n' "${smcp_conda_exe}" >&2
    exit 1
fi

smcp_env_exists() {
    "${smcp_conda_exe}" env list | awk '{print $1}' | grep -Fxq -- "$1"
}

smcp_env_names=(model-score-denoise model-straightPCF pointcleannet model-pinn-smcp)

if [[ "${smcp_recreate}" == true ]]; then
    for smcp_env_name in "${smcp_env_names[@]}"; do
        if smcp_env_exists "${smcp_env_name}"; then
            printf 'Removing environment %s...\n' "${smcp_env_name}"
            "${smcp_conda_exe}" env remove --name "${smcp_env_name}" --yes
        fi
    done
fi

# The trainer needs only conda-forge and pip. Creating it with --override-channels keeps conda from
# consulting the Anaconda default channels, whose Terms of Service may not have been accepted.
smcp_create_pinn_env() {
    "${smcp_conda_exe}" create --yes --override-channels --channel conda-forge \
        --name model-pinn-smcp python=3.11 pip numpy=1.26
    "${smcp_conda_dir}/envs/model-pinn-smcp/bin/python" -m pip install \
        --extra-index-url https://download.pytorch.org/whl/cpu torch==2.8.0+cpu
}

for smcp_env_name in "${smcp_env_names[@]}"; do
    if ! smcp_env_exists "${smcp_env_name}"; then
        printf 'Creating %s...\n' "${smcp_env_name}"
        if [[ "${smcp_env_name}" == model-pinn-smcp ]]; then
            smcp_create_pinn_env
        else
            "${smcp_conda_exe}" env create \
                --file "${smcp_script_dir}/environments/${smcp_env_name}.yml"
        fi
    else
        printf 'Environment %s already exists; keeping it.\n' "${smcp_env_name}"
    fi
done

printf 'The model and trainer environments are ready.\n'
