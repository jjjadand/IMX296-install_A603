#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BSP_DIR="${1:-$(cd "${PROJECT_DIR}/../.." && pwd)}"
SOURCE_DTS="${PROJECT_DIR}/tegra234-p3767-camera-603-imx296lq-overlay.dts"
BUILD_DIR="${PROJECT_DIR}/build"
PREPROCESSED_DTS="${BUILD_DIR}/tegra234-p3767-camera-603-imx296lq-overlay.preprocessed.dts"
OUTPUT_DTBO="${BUILD_DIR}/tegra234-p3767-camera-603-imx296lq-overlay.dtbo"
WARNINGS_FILE="${BUILD_DIR}/dtc-warnings.txt"
BASE_DTB="${PROJECT_DIR}/../603_jp62/Linux_for_Tegra/kernel/dtb/tegra234-p3768-0000+p3767-0003-nv-super.dtb"
MERGED_DTB="${BUILD_DIR}/tegra234-p3768-0000+p3767-0003-nv-super+imx296lq.dtb"
MERGED_DTS="${BUILD_DIR}/merged-imx296lq.dts"

INCLUDE_KERNEL="${BSP_DIR}/source/hardware/nvidia/t23x/nv-public/include/kernel"
INCLUDE_PLATFORMS="${BSP_DIR}/source/hardware/nvidia/t23x/nv-public/include/platforms"

for required in "${SOURCE_DTS}" "${INCLUDE_KERNEL}" "${INCLUDE_PLATFORMS}" "${BASE_DTB}"; do
	if [[ ! -e "${required}" ]]; then
		echo "Missing required path: ${required}" >&2
		exit 1
	fi
done

mkdir -p "${BUILD_DIR}"

cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
	-I "${INCLUDE_KERNEL}" \
	-I "${INCLUDE_PLATFORMS}" \
	"${SOURCE_DTS}" > "${PREPROCESSED_DTS}"

dtc -@ -I dts -O dtb -o "${OUTPUT_DTBO}" "${PREPROCESSED_DTS}" \
	2> "${WARNINGS_FILE}"

echo "Built ${OUTPUT_DTBO}"
if [[ -s "${WARNINGS_FILE}" ]]; then
	echo "dtc warnings: ${WARNINGS_FILE}"
	cat "${WARNINGS_FILE}"
fi

fdtoverlay -i "${BASE_DTB}" -o "${MERGED_DTB}" "${OUTPUT_DTBO}"
dtc -I dtb -O dts -o "${MERGED_DTS}" "${MERGED_DTB}" >/dev/null
echo "Merged overlay onto A603 base DTB: ${MERGED_DTB}"
