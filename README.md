# Seeed A603 + IMX296LQ (color) on JetPack 6.2

It is for the **color** IMX296LQ module on the A603 **15-pin CSI** connector.

NVIDIA Argus / `nvarguscamerasrc` is **not** supported (no NITO). Colour
science is a CPU ISP shared by `view-imx296` and `stream-imx296`.

Porting this stack to another carrier: see [docs/PORTING.md](docs/PORTING.md).
On the board after install: `/usr/share/doc/leetop-a603-imx296lq-camera/PORTING.md`.

## Package contents

| Asset | On the Jetson |
| --- | --- |
| Device-tree overlay | `/boot/tegra234-p3767-camera-603-imx296lq-overlay.dtbo` |
| Overlay source | `/usr/share/imx296-a603/tegra234-p3767-camera-603-imx296lq-overlay.dts` |
| tegracam driver | `/usr/lib/imx296-a603/nv_imx296.ko` → `/lib/modules/$(uname -r)/updates/...` |
| CPU ISP tables | `/usr/share/imx296-a603/imx296-isp.json` |
| libcamera source JSON | `/usr/share/imx296-a603/imx296_16mm.json` |
| Preview | `/usr/local/bin/view-imx296` (uses `camview`) |
| Stream | `/usr/local/bin/stream-imx296` (uses `camview`) |
| Capture helper | `/usr/local/bin/camview` |

Local ISP override (wins over the package file):

```bash
sudo mkdir -p /etc/imx296-a603
sudo cp /usr/share/imx296-a603/imx296-isp.json /etc/imx296-a603/
# edit /etc/imx296-a603/imx296-isp.json
```

## 15-pin mapping (confirmed from the camera module)

| Pin | Camera | A603 / Orin |
| --- | --- | --- |
| 1 | GND | GND |
| 2 | MDN0 | CSI A lane0 - |
| 3 | MDP0 | CSI A lane0 + |
| 4 | GND | GND |
| 5 | MDN1 | unused (sensor is 1-lane) |
| 6 | MDP1 | unused |
| 7 | GND | GND |
| 8 | MCN | CSI A clock - |
| 9 | MCP | CSI A clock + |
| 10 | GND | GND |
| 11 | PWDN / XCLR | Tegra main GPIO **PH.06** (same as A603 IMX219 reset) |
| 12 | NC | no host MCLK, no LED; sensor uses its own 54 MHz oscillator |
| 13 | SIO-C (SCL) | `cam_i2c` / `i2c@3180000` |
| 14 | SIO-D (SDA) | `cam_i2c` / `i2c@3180000` |
| 15 | +3.3V | +3.3V |

DT overlay: `serial_a`, 1 lane, I2C `0x1a`, `lane_polarity=6` (matches the
A603 baked-in IMX219 topology). The overlay **disables** `rbpcv2_imx219_a@10`
because A603 already ships a 2-lane IMX219 on this same connector.

## Build on the BSP host

```bash
cd leetop_a603_imx296_jp62
./build-overlay.sh /path/to/Linux_for_Tegra
# optional, only if nv_imx296.ko is missing:
./build-driver.sh
make -C tools
./build-deb.sh
```

The tegracam driver is reused from `../recomputer_orin_imx296_jp62`.
Color vs mono is selected by DT `pixel_phase = "rggb"` (this overlay) vs
`"y"` (J401 IMX296LL).

Rebuild CPU ISP tables from the Raspberry Pi JSON:

```bash
python3 isp/export-runtime.py
```

## Install on the A603 board (JetPack 6.2 / L4T R36.4.3)

```bash
sudo dpkg -i jetpack6.2_leetop-a603-imx296lq-camera_1.1.2_arm64.deb
sudo python3 /opt/nvidia/jetson-io/config-by-hardware.py -n 2="Camera IMX296LQ A603"
sudo reboot
```

After reboot:

```bash
# should list imx296, 1456x1088, RG10 / RGGB
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --all | sed -n '1,80p'

# local GTK preview (desktop session)
view-imx296

# darker indoor scene
EXPOSURE=15699 GAIN=160 view-imx296

# H.264 MPEG-TS over TCP :8554 (same ISP as the preview)
stream-imx296
# watch on a PC:  vlc tcp://<jetson-ip>:8554
```

Both tools call `camview`, which loads `/usr/share/imx296-a603/imx296-isp.json`
(or `/etc/imx296-a603/imx296-isp.json` if present). `CAMVIEW_ISP=0` skips
AWB/CCM and keeps demosaic + gamma only.

The runtime JSON also has highlight and zipper knobs (10-bit linear, after
black subtraction):

- `hl_start` / `hl_full` (default 780..940): fade clipped lamps toward white
  so AWB+CCM cannot turn fluorescent tubes purple.
- `fc_start` / `fc_full` plus `fc_ratio_lo` / `fc_ratio_hi` (default 220..500
  and 250..650 Q10): desaturate bright, channel-imbalanced Bayer zipper
  (cyan/orange lamp edges) toward luma. Brightness uses the 3x3 demosaic
  peak so the darker zipper ring is included. Do **not** lower `hl_start`
  to kill zipper — that paints a white halo around the tube.

## First-bring-up notes

- If probe fails with I2C NACK, check 15-pin seating and that pin 11 really
  toggles PH.06. The driver drives reset low then high.
- If probe succeeds but the image is garbled / all-green, try
  `lane_polarity` `0`, `2`, or `4` in the overlay and rebuild.
- Default live-view is 10000 us / gain 80 / 60 fps, RGGB + CPU ISP.
  Too dark/bright: `EXPOSURE=... GAIN=... view-imx296`
  (exposure 15-15700, gain 0-480).
- Fluorescent tubes look purple: raise / leave `hl_*`; the 4560 K CCM drives
  G negative on clipped whites after AWB.
- Cyan/orange zipper on lamp edges: tune `fc_*`, not `hl_*`.

## Changelog

### 1.1.2

- CPU ISP desaturates Bayer zipper toward luma (`fc_*`). Brightness uses
  the 3x3 demosaic peak so the darker cyan/orange/magenta ring around a
  clipped tube is included, without painting a white halo.
- `camview` stderr now prints `hl=... fc=... r=...` so a live capture
  shows which tables are loaded.
- OpenMP 3x3 peak drops `camview` stdout from ~60 fps to ~43 fps at
  1456x1088; the sensor/CSI stay at 60.

### 1.1.1

- CPU ISP fades clipped lamps toward white (`hl_start` / `hl_full`) so
  AWB+CCM cannot turn fluorescent tube cores purple.

### 1.1.0

- Unified preview and stream on `camview`, default CFA `rggb`, 60 fps,
  and `/usr/share/imx296-a603/imx296-isp.json`.
