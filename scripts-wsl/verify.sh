#!/usr/bin/env bash

set -euo pipefail

smcp_user_home="${HOME:?HOME is not set}"
smcp_conda_dir="${SMCP_CONDA_DIR:-${smcp_user_home}/miniconda3}"
smcp_conda_exe="${SMCP_CONDA_EXE:-${smcp_conda_dir}/bin/conda}"
smcp_strict=false
smcp_environments_only=false
smcp_status=0

usage() {
    printf 'Usage: %s [--strict] [--environments-only]\n' "$(basename -- "$0")"
}

for smcp_arg in "$@"; do
    case "${smcp_arg}" in
        --strict) smcp_strict=true ;;
        --environments-only) smcp_environments_only=true ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Unknown option: %s\n' "${smcp_arg}" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ ! -x "${smcp_conda_exe}" ]]; then
    printf '[FAIL] conda was not found at %s\n' "${smcp_conda_exe}" >&2
    exit 1
fi

printf 'WSL: '
if grep -qi microsoft /proc/version 2>/dev/null; then
    printf 'detected\n'
else
    printf 'not detected\n'
    smcp_status=1
fi

if command -v lsb_release >/dev/null 2>&1; then
    printf 'Distribution: %s\n' "$(lsb_release -ds)"
fi
printf 'Kernel: %s\n' "$(uname -r)"
printf 'Conda: %s\n' "$("${smcp_conda_exe}" --version)"

smcp_common_check='import numpy, scipy, sklearn, torch, tqdm, yaml; print(f"Python environment OK | torch={torch.__version__} | CUDA runtime={torch.version.cuda} | CUDA available={torch.cuda.is_available()}"); print(f"GPU={torch.cuda.get_device_name(0) if torch.cuda.is_available() else '\''none'\''}")'
smcp_score_check='import numpy, scipy, sklearn, torch, torch_cluster, tqdm, yaml; print(f"Python environment OK | torch={torch.__version__} | CUDA runtime={torch.version.cuda} | CUDA available={torch.cuda.is_available()}"); print(f"GPU={torch.cuda.get_device_name(0) if torch.cuda.is_available() else '\''none'\''}")'
smcp_straight_check='import numpy, scipy, sklearn, torch, torch_cluster, torch_geometric, tqdm, yaml; print(f"Python environment OK | torch={torch.__version__} | PyG={torch_geometric.__version__} | CUDA runtime={torch.version.cuda} | CUDA available={torch.cuda.is_available()}"); print(f"GPU={torch.cuda.get_device_name(0) if torch.cuda.is_available() else '\''none'\''}")'

for smcp_env_name in model-score-denoise model-straightPCF pointcleannet; do
    printf '\n[%s]\n' "${smcp_env_name}"
    smcp_check_code="${smcp_common_check}"
    case "${smcp_env_name}" in
        model-score-denoise) smcp_check_code="${smcp_score_check}" ;;
        model-straightPCF) smcp_check_code="${smcp_straight_check}" ;;
    esac

    if ! "${smcp_conda_exe}" run --name "${smcp_env_name}" \
        python -c "${smcp_check_code}"; then
        printf '[FAIL] Environment %s is incomplete.\n' "${smcp_env_name}" >&2
        smcp_status=1
    fi
done

if command -v nvidia-smi >/dev/null 2>&1; then
    printf '\nNVIDIA device:\n'
    nvidia-smi --query-gpu=name,driver_version,memory.total \
        --format=csv,noheader
else
    printf '\n[WARN] nvidia-smi is unavailable; GPU inference will not work.\n' >&2
    if [[ "${smcp_strict}" == true ]]; then
        smcp_status=1
    fi
fi

if [[ "${smcp_environments_only}" == false ]]; then
    smcp_model_root="${smcp_user_home}/smcp-models"
    smcp_required_files=(
        score-denoise/run_score_denoise.py
        score-denoise/model/pretrained/ckpt.pt
        straightPCF/run_straightpcf.py
        straightPCF/model/pretrained_straightpcf/ckpt_straightpcf.pt
        straightPCF/model/pretrained_cvm/ckpt_cvm.pt
        pointcleannet/run_pointcleannet.py
        pointcleannet/model/models/PointCleanNet_params.pth
        pointcleannet/model/models/PointCleanNet_model.pth
    )

    printf '\nModel artifacts:\n'
    for smcp_required_file in "${smcp_required_files[@]}"; do
        if [[ -f "${smcp_model_root}/${smcp_required_file}" ]]; then
            printf '[OK]   %s/%s\n' "${smcp_model_root}" "${smcp_required_file}"
        else
            printf '[WARN] Missing %s/%s\n' "${smcp_model_root}" "${smcp_required_file}" >&2
            if [[ "${smcp_strict}" == true ]]; then
                smcp_status=1
            fi
        fi
    done

    if [[ -x "${smcp_user_home}/.local/bin/smcp" ]]; then
        printf '[OK]   %s/.local/bin/smcp\n' "${smcp_user_home}"
        printf '\nModel registry:\n'
        if ! "${smcp_user_home}/.local/bin/smcp" --validate-registry --json; then
            printf '[FAIL] The registry or one or more enabled models are invalid.\n' >&2
            smcp_status=1
        fi
    else
        printf '[WARN] The smcp launcher is not installed.\n' >&2
        if [[ "${smcp_strict}" == true ]]; then
            smcp_status=1
        fi
    fi
fi

if [[ "${smcp_status}" -ne 0 ]]; then
    printf '\nVerification failed. Review the messages above.\n' >&2
    exit "${smcp_status}"
fi

printf '\nVerification completed successfully.\n'
