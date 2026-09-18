#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG_NAME="leetop-a603-imx296lq-camera"
PKG_VER="1.1.2"
ARCH="arm64"
KVER="5.15.148-tegra"
OUT="${PROJECT_DIR}/jetpack6.2_${PKG_NAME}_${PKG_VER}_${ARCH}.deb"
DTBO="${PROJECT_DIR}/build/tegra234-p3767-camera-603-imx296lq-overlay.dtbo"
KO="${PROJECT_DIR}/build/nv_imx296.ko"
OVERLAY_NAME="Camera IMX296LQ A603"

for required in "${DTBO}" "${KO}" \
	"${PROJECT_DIR}/tools/camview" \
	"${PROJECT_DIR}/tools/view-imx296" \
	"${PROJECT_DIR}/tools/stream-imx296" \
	"${PROJECT_DIR}/tools/blacklist-mainline-imx296.conf" \
	"${PROJECT_DIR}/tools/nv_imx296.conf" \
	"${PROJECT_DIR}/isp/imx296-isp.json" \
	"${PROJECT_DIR}/isp/imx296_16mm.json"; do
	if [[ ! -e "${required}" ]]; then
		echo "Missing build artifact: ${required}" >&2
		exit 1
	fi
done

STAGE_TMP=$(mktemp -d /tmp/imx296-deb-stage.XXXXXX)
chmod 755 "${STAGE_TMP}"
mkdir -p \
	"${STAGE_TMP}/DEBIAN" \
	"${STAGE_TMP}/boot" \
	"${STAGE_TMP}/usr/lib/imx296-a603" \
	"${STAGE_TMP}/usr/local/bin" \
	"${STAGE_TMP}/etc/modprobe.d" \
	"${STAGE_TMP}/etc/modules-load.d" \
	"${STAGE_TMP}/usr/share/imx296-a603" \
	"${STAGE_TMP}/etc/imx296-a603" \
	"${STAGE_TMP}/usr/share/doc/leetop-a603-imx296lq-camera"
find "${STAGE_TMP}" -type d -exec chmod 755 {} +

install -m 0644 "${DTBO}" "${STAGE_TMP}/boot/tegra234-p3767-camera-603-imx296lq-overlay.dtbo"
install -m 0644 "${KO}" "${STAGE_TMP}/usr/lib/imx296-a603/nv_imx296.ko"
install -m 0755 "${PROJECT_DIR}/tools/camview" "${STAGE_TMP}/usr/local/bin/camview"
install -m 0755 "${PROJECT_DIR}/tools/view-imx296" "${STAGE_TMP}/usr/local/bin/view-imx296"
install -m 0755 "${PROJECT_DIR}/tools/stream-imx296" "${STAGE_TMP}/usr/local/bin/stream-imx296"
install -m 0644 "${PROJECT_DIR}/tools/blacklist-mainline-imx296.conf" "${STAGE_TMP}/etc/modprobe.d/blacklist-mainline-imx296.conf"
install -m 0644 "${PROJECT_DIR}/tools/nv_imx296.conf" "${STAGE_TMP}/etc/modules-load.d/nv_imx296.conf"
install -m 0644 "${PROJECT_DIR}/isp/imx296-isp.json" "${STAGE_TMP}/usr/share/imx296-a603/imx296-isp.json"
install -m 0644 "${PROJECT_DIR}/isp/imx296_16mm.json" "${STAGE_TMP}/usr/share/imx296-a603/imx296_16mm.json"
install -m 0644 "${PROJECT_DIR}/isp/README.md" "${STAGE_TMP}/usr/share/imx296-a603/README.md"
install -m 0644 "${PROJECT_DIR}/tegra234-p3767-camera-603-imx296lq-overlay.dts" "${STAGE_TMP}/usr/share/imx296-a603/tegra234-p3767-camera-603-imx296lq-overlay.dts"
install -m 0644 "${PROJECT_DIR}/tools/camview.c" "${STAGE_TMP}/usr/share/imx296-a603/camview.c"
install -m 0644 "${PROJECT_DIR}/driver/tegracam/nv_imx296.c" "${STAGE_TMP}/usr/share/imx296-a603/nv_imx296.c"
install -m 0644 "${PROJECT_DIR}/driver/tegracam/imx296_mode_tbls.h" "${STAGE_TMP}/usr/share/imx296-a603/imx296_mode_tbls.h"
install -m 0644 "${PROJECT_DIR}/driver/tegracam/Makefile" "${STAGE_TMP}/usr/share/imx296-a603/nv_imx296.Makefile"
install -m 0755 "${PROJECT_DIR}/isp/export-runtime.py" "${STAGE_TMP}/usr/share/imx296-a603/export-runtime.py"
printf "%s\n" "Copy imx296-isp.json here to override /usr/share/imx296-a603/imx296-isp.json" > "${STAGE_TMP}/etc/imx296-a603/README"
install -m 0644 "${PROJECT_DIR}/README.md" "${STAGE_TMP}/usr/share/doc/leetop-a603-imx296lq-camera/README.md"
install -m 0644 "${PROJECT_DIR}/docs/PORTING.md" "${STAGE_TMP}/usr/share/doc/leetop-a603-imx296lq-camera/PORTING.md"
install -m 0644 "${PROJECT_DIR}/tools/imx296-preview.txt" "${STAGE_TMP}/usr/share/doc/leetop-a603-imx296lq-camera/imx296-preview.txt"

cat > "${STAGE_TMP}/DEBIAN/control" <<CTL
Package: ${PKG_NAME}
Version: ${PKG_VER}
Architecture: ${ARCH}
Maintainer: Seeed BSP <bsp@seeed.com>
Depends: v4l-utils, libgomp1
Section: kernel
Priority: optional
Description: IMX296LQ (color) global-shutter camera support for Leetop/Seeed A603
 Single IMX296LQ camera device tree overlay on the A603 15-pin CSI connector
 (serial_a, 1-lane, PWDN=PH.06, I2C cam_i2c @ 0x1a). Reuses the JetPack 6.2
 tegracam module nv_imx296 (${KVER}, L4T R36.4.3) plus V4L2 viewing utilities
 (camview / view-imx296 / stream-imx296) and CPU ISP tables.
 .
 After install, select the overlay with Jetson-IO:
   sudo python3 /opt/nvidia/jetson-io/config-by-hardware.py -n 2="${OVERLAY_NAME}"
   sudo reboot
 Then run view-imx296 locally, or stream-imx296 to push H.264 MPEG-TS over TCP.
 Colour science is /usr/share/imx296-a603/imx296-isp.json; override in /etc/imx296-a603/.
 Carrier-porting notes: /usr/share/doc/leetop-a603-imx296lq-camera/PORTING.md.
 Note: Argus/nvarguscamerasrc is NOT supported for this sensor (NITO
 unavailable); use the V4L2 path.
CTL

cat > "${STAGE_TMP}/DEBIAN/postinst" <<'POST'
#!/bin/bash
set -e
KVER=$(uname -r)
SRC=/usr/lib/imx296-a603/nv_imx296.ko
if [ "$KVER" = "5.15.148-tegra" ]; then
	DSTDIR=/lib/modules/$KVER/updates/drivers/media/i2c
	mkdir -p "$DSTDIR"
	cp "$SRC" "$DSTDIR/nv_imx296.ko"
	depmod -a
	echo "nv_imx296.ko installed for kernel $KVER"
	rmmod imx296 2>/dev/null || true
	modprobe nv_imx296 2>/dev/null || true
else
	echo "WARNING: this package ships nv_imx296.ko for 5.15.148-tegra, board is $KVER" >&2
	echo "WARNING: kernel module NOT installed; dtbo and tools are installed." >&2
	echo "WARNING: rebuild nv_imx296 against your kernel headers and copy it to" >&2
	echo "         /lib/modules/$KVER/updates/drivers/media/i2c/nv_imx296.ko" >&2
fi
echo
echo "1. apply overlay:  sudo python3 /opt/nvidia/jetson-io/config-by-hardware.py -n 2=\"Camera IMX296LQ A603\""
echo "   (or interactive: sudo /opt/nvidia/jetson-io/jetson-io.py)"
echo "2. reboot"
echo "3. local view (desktop terminal):  view-imx296"
echo "4. network stream:                  stream-imx296"
echo "   ISP tables:                      /usr/share/imx296-a603/imx296-isp.json"
echo "   local override:                  /etc/imx296-a603/imx296-isp.json"
echo "   watch on PC:                     vlc tcp://<jetson-ip>:8554"
POST

cat > "${STAGE_TMP}/DEBIAN/postrm" <<'POSTRM'
#!/bin/bash
# dpkg also runs postrm on upgrade. Only clean up on a real remove/purge.
if [ "$1" != "remove" ] && [ "$1" != "purge" ]; then
	exit 0
fi
KVER=$(uname -r)
rm -f "/lib/modules/$KVER/updates/drivers/media/i2c/nv_imx296.ko"
depmod -a 2>/dev/null || true
exit 0
POSTRM

chmod 755 "${STAGE_TMP}/DEBIAN/postinst" "${STAGE_TMP}/DEBIAN/postrm"

dpkg-deb --build --root-owner-group "${STAGE_TMP}" "${OUT}"
echo "Built ${OUT}"
dpkg-deb -I "${OUT}"
dpkg-deb -c "${OUT}"
