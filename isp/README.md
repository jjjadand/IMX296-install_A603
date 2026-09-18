# IMX296LQ CPU ISP tables

NVIDIA Argus has no NITO for this sensor, so colour science lives here
instead of in a closed ISP binary.

- `imx296_16mm.json` — Raspberry Pi / libcamera tuning (source of truth)
- `imx296-isp.json` — compact runtime tables used by `camview`
  (black level, AWB ct curve, 4560 K CCM, gamma LUT, highlight / zipper knobs)

On the Jetson these are installed to:

- `/usr/share/imx296-a603/imx296-isp.json`  (package default)
- `/etc/imx296-a603/imx296-isp.json`        (local override, wins if present)

`view-imx296` and `stream-imx296` both run `camview`, so they share this
file. Override with `CAMVIEW_ISP_FILE=/path/to.json` or disable AWB/CCM
with `CAMVIEW_ISP=0`.

Regenerate the runtime file from the libcamera JSON:

```bash
python3 isp/export-runtime.py
```

`export-runtime.py` copies the highlight / zipper knobs below; they are not
in the Raspberry Pi file.

## Pipeline (`camview` 1.1.2)

Per pixel, after RGGB bilinear demosaic (10-bit, black subtracted):

1. Grey-world AWB, projected onto the RPi colour-temperature curve.
2. 4560 K CCM.
3. False-colour / zipper: if the pixel is bright **and** channel-imbalanced,
   fade RGB toward luma (not toward white).
4. If CCM drove a channel negative or to ~0 on a bright pixel, snap to luma.
5. Highlight recovery: if the **pixel itself** is clipped, fade toward paper
   white so the tube core is white, not purple.
6. RPi gamma LUT to 8-bit.

## Highlight recovery

`hl_start` / `hl_full` (10-bit linear, default **780..940**) fade clipped
lamps toward white. After AWB a clipped `{1023,1023,1023}` is no longer
neutral, and the 4560 K CCM then drives G negative → purple tubes.

Use these only for the overexposed core. Lowering `hl_start` to cover the
Bayer zipper paints a **white halo** around the tube.

## Bayer zipper / false colour

Clipped lamps saturate green first. Demosaic then CCM invent a cyan/orange
(and sometimes magenta) ring a few pixels wide, including pixels whose own
peak is only ~200–500/1023 — below `hl_start`.

| Key | Default | Meaning |
| --- | --- | --- |
| `fc_start` / `fc_full` | 220 / 500 | Brightness ramp (10-bit) |
| `fc_ratio_lo` / `fc_ratio_hi` | 250 / 650 | Channel imbalance `(max-min)*1024/max` |

Brightness is the **3x3 demosaic peak**, not the pixel itself, so the darker
zipper ring still desaturates. Scene-coloured objects in the A603 office
shot stay below ~200/1023 and are left alone.

Tune zipper with `fc_*`. Do not expand `hl_*` or neighborhood-average
clipped samples into the interpolation — both grow a white fog.
