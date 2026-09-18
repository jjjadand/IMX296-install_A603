#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BSP_DIR="${1:-$(cd "${PROJECT_DIR}/../.." && pwd)}"
KERNEL_SOURCE="${BSP_DIR}/source/kernel/kernel-jammy-src"
MODULE_DIR="${PROJECT_DIR}/driver/tegracam"
OUTPUT_MODULE="${PROJECT_DIR}/build/nv_imx296.ko"
NVOOT="${NVOOT:-${BSP_DIR}/rootfs/usr/src/nvidia/nvidia-oot}"
HEADERS_SYMVERS="${BSP_DIR}/rootfs/usr/src/linux-headers-5.15.148-tegra-ubuntu22.04_aarch64/3rdparty/canonical/linux-jammy/kernel-source/Module.symvers"
TEGRA_CAMERA_KO="${TEGRA_CAMERA_KO:-${BSP_DIR}/rootfs/usr/lib/modules/5.15.148-tegra/updates/drivers/media/platform/tegra/camera/tegra-camera.ko}"
OOT_SYMVERS="${NVOOT}/Module.symvers"
BOARD_SYMVERS="${PROJECT_DIR}/build/tegra-camera.symvers"
TOOLCHAIN_BIN="${TOOLCHAIN_BIN:-/other_data/36.4.3-nvidia-devkit/aarch64--glibc--stable-2022.08-1/bin}"
CROSS_COMPILE="${CROSS_COMPILE:-${TOOLCHAIN_BIN}/aarch64-buildroot-linux-gnu-}"

for required in \
	"${KERNEL_SOURCE}/Makefile" \
	"${KERNEL_SOURCE}/.config" \
	"${HEADERS_SYMVERS}" \
	"${OOT_SYMVERS}" \
	"${TEGRA_CAMERA_KO}" \
	"${NVOOT}/include/media/tegracam_core.h" \
	"${MODULE_DIR}/nv_imx296.c" \
	"${MODULE_DIR}/imx296_mode_tbls.h" \
	"${MODULE_DIR}/Makefile" \
	"${MODULE_DIR}/nvidia/conftest.h"; do
	if [[ ! -e "${required}" ]]; then
		echo "Missing required path: ${required}" >&2
		exit 1
	fi
done

mkdir -p "${PROJECT_DIR}/build"

# nvidia-oot/Module.symvers CRCs do not match the flashed tegra-camera.ko
# (same L4T version, different OOT build). Rewrite CRCs from the actual
# module that ships in the BSP rootfs / on the board.
python3 - "${OOT_SYMVERS}" "${TEGRA_CAMERA_KO}" "${BOARD_SYMVERS}" <<'PY'
import re, subprocess, sys
from pathlib import Path

oot, ko, out = map(Path, sys.argv[1:4])
crcs = {}
for line in subprocess.check_output(["nm", "-a", str(ko)], text=True, errors="replace").splitlines():
    m = re.search(r"([0-9a-fA-F]+)\s+A\s+__crc_(\S+)", line)
    if m:
        crcs[m.group(2)] = int(m.group(1), 16)
if not crcs:
    raise SystemExit("no exported CRCs in %s" % ko)

lines = []
replaced = 0
seen = set()
for raw in oot.read_text().splitlines():
    parts = raw.split("\t")
    if len(parts) >= 2 and parts[1] in crcs:
        new = "0x%08x" % crcs[parts[1]]
        if parts[0] != new:
            replaced += 1
        parts[0] = new
        seen.add(parts[1])
    if len(parts) == 4:
        parts.append("")
    lines.append("\t".join(parts))
extra = 0
for name, crc in sorted(crcs.items()):
    if name not in seen:
        lines.append("0x%08x\t%s\ttegra-camera\tEXPORT_SYMBOL_GPL\t" % (crc, name))
        extra += 1
out.write_text("\n".join(lines) + "\n")
print("Wrote %s (replaced %d CRCs, extra %d) from %s" % (out, replaced, extra, ko))
PY

# Match the flashed kernel vermagic: 5.15.148-tegra SMP preempt ...
if [[ ! -f "${KERNEL_SOURCE}/include/generated/utsrelease.h" ]] || \
   ! grep -q '5.15.148-tegra' "${KERNEL_SOURCE}/include/generated/utsrelease.h"; then
	echo "Preparing kernel tree (modules_prepare, LOCALVERSION=-tegra)"
	make -C "${KERNEL_SOURCE}" \
		ARCH=arm64 \
		CROSS_COMPILE="${CROSS_COMPILE}" \
		LOCALVERSION=-tegra \
		modules_prepare
fi

if [[ ! -f "${KERNEL_SOURCE}/Module.symvers" ]]; then
	cp "${HEADERS_SYMVERS}" "${KERNEL_SOURCE}/Module.symvers"
fi

make -C "${KERNEL_SOURCE}" \
	ARCH=arm64 \
	CROSS_COMPILE="${CROSS_COMPILE}" \
	LOCALVERSION=-tegra \
	CONFIG_MODULE_SIG_ALL= \
	M="${MODULE_DIR}" \
	NVOOT="${NVOOT}" \
	KBUILD_EXTRA_SYMBOLS="${BOARD_SYMVERS}" \
	modules

install -m 0644 "${MODULE_DIR}/nv_imx296.ko" "${OUTPUT_MODULE}"
echo "Built ${OUTPUT_MODULE}"
modinfo "${OUTPUT_MODULE}" | grep -E '^(filename|name|vermagic|depends|description):'
