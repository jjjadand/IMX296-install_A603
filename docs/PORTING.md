# IMX296 载板二次适配指南（JetPack 6.2 / L4T R36.4.3）

这份文档把 A603 + IMX296LQ（彩色全局快门）在 JetPack 6.2 上从探测到可预览、可推流的完整经验写下来，方便其他载板复用同一套 tegracam 驱动、CPU ISP 和推流工具。

参考实现：

- 彩色单摄 15-pin：`other_carrier_board/leetop_a603_imx296_jp62`（本文主角）
- 黑白双摄 24-pin：`other_carrier_board/recomputer_orin_imx296_jp62`（J401 / IMX296LL）

目标内核：`5.15.148-tegra`。Argus / `nvarguscamerasrc` **不要用**，没有 NITO，JP6.2 的 ISP 表按 camera badge 内置，IMX296 对不上。整条链路走 V4L2。

---

## 0. 先分清四件事

二次适配时，四块资产要分开想，不要混在一个脚本里：

| 资产 | 谁负责 | 换载板时通常要改什么 |
| --- | --- | --- |
| 设备树 overlay | CSI 拓扑、I2C、复位 GPIO、lane 极性、Bayer 相位 | **几乎总要改** |
| tegracam 驱动 `nv_imx296.ko` | 上电时序、寄存器、曝光/增益/帧率 | 一般 **复用**，只按目标内核重编 |
| CPU ISP 表 | 黑电平、AWB、CCM、gamma | 彩色模组可复用 RPi `imx296_16mm.json`；黑白模组关掉 ISP |
| 预览 / 推流 | `camview` → RGB8 → GStreamer | **不要绕开 camview**；换载板只改设备节点/默认曝光 |

A603 上验证过的成品包：`jetpack6.2_leetop-a603-imx296lq-camera_1.1.2_arm64.deb`。板上最终布局：

```
/boot/tegra234-p3767-camera-603-imx296lq-overlay.dtbo
/usr/lib/imx296-a603/nv_imx296.ko
/usr/share/imx296-a603/imx296-isp.json          # 运行时表，预览和推流共用
/usr/share/imx296-a603/imx296_16mm.json         # libcamera 源 JSON
/usr/share/imx296-a603/nv_imx296.c              # 驱动源码
/usr/local/bin/camview
/usr/local/bin/view-imx296
/usr/local/bin/stream-imx296
/etc/imx296-a603/                               # 本地覆盖 ISP 的目录
```

预览和推流都必须走 `camview`。ISP 不要写死在某一个工具里。

---

## 1. 硬件：先把 15-pin / 24-pin 对清楚

IMX296 模组常见 Raspberry Pi CSI 排线。A603 上实测（彩色 LQ，单 lane）：

| 模组脚 | 信号 | A603 / Orin |
| --- | --- | --- |
| 1 | GND | GND |
| 2 | MDN0 | CSI A lane0 − |
| 3 | MDP0 | CSI A lane0 + |
| 4 | GND | GND |
| 5 / 6 | MDN1 / MDP1 | **空**（传感器只有 1 lane） |
| 7 | GND | GND |
| 8 | MCN | CSI A clock − |
| 9 | MCP | CSI A clock + |
| 10 | GND | GND |
| 11 | PWDN / XCLR | **PH.06**（和这块板自带 IMX219 复位同一脚） |
| 12 | NC | **没有 host MCLK，没有 LED**；传感器自带 54 MHz 晶振 |
| 13 | SCL | `cam_i2c` / `i2c@3180000` |
| 14 | SDA | 同上 |
| 15 | +3.3V | +3.3V |

换载板时按这个清单核对，不要抄 A603 的 GPIO / CSI brick：

1. **CSI brick**：`serial_a` / `serial_c` / …，对应 `port-index` 和 `bus-width`。
2. **I2C**：直连 `i2c@3180000`，还是 `cam_i2cmux/i2c@0`。地址几乎都是 `0x1a`。
3. **复位脚**：必须用示波器或 `gpioset` + `i2cdetect` 验证。A603 上 PH.06 拉高才出现 `0x1a`，拉低就是 NACK。
4. **不要碰错 hog**。A603 的 `cam0-rst` hog 在 **PH.03**，那不是 15-pin 的 PWDN。乱改会把别的外设拉死。
5. **MCLK**：这颗模组不需要 host 时钟。DT 里 `mclk_khz = "54000"` 只是告诉驱动内部时钟，不要再去使能一个不存在的 CAM_MCLK。
6. **同座已有摄像头**：A603 底板 DTB 已经焊死 2-lane IMX219。overlay 必须 `status = "disabled"` 掉 `rbpcv2_imx219_a@10`，并把 CSI A 改成 1 lane。

J401 双摄对照（方便抄拓扑，不要抄到 A603 上）：

| 摄像头 | 座子 | CSI | port-index | I2C | PWDN |
| --- | --- | --- | --- | --- | --- |
| CAM0 | J12 | `serial_a` 1 lane | 0 | mux `i2c@0` @ 0x1a | PH.06 |
| CAM1 | J9 | `serial_c` 1 lane | 2 | mux `i2c@1` @ 0x1a | PAC.00 |

---

## 2. 设备树 overlay：只改拓扑，不要改传感器 mode 表

A603 的工作 overlay：`tegra234-p3767-camera-603-imx296lq-overlay.dts`。

### 2.1 Jetson-IO 能认到

```dts
overlay-name = "Camera IMX296LQ A603";
jetson-header-name = "Jetson 24pin CSI Connector";
compatible = JETSON_COMPATIBLE_P3768;
```

`jetson-header-name` 必须和这块载板 Jetson-IO 里的 header 2 名字一致，否则 `config-by-hardware.py -n 2="..."` 选不中。装包后：

```bash
sudo python3 /opt/nvidia/jetson-io/config-by-hardware.py -n 2="Camera IMX296LQ A603"
sudo reboot
```

### 2.2 三处 bus-width / port-index 必须一致

- `tegra-capture-vi` 的 endpoint
- `nvcsi@15a00000` channel@0 的 endpoint
- 传感器 `port@0` endpoint 的 `remote-endpoint`

A603：`port-index = <0>`，`bus-width = <1>`，远端复用底板已有的 `&rbpcv2_imx219_csi_in0`。标签要能在底板 DTB 的 `__symbols__` 里解析。编 overlay 后用 `fdtoverlay` 合到 **这块板的** base DTB 上检查，不要合 NVIDIA 开发套件的 DTB。

### 2.3 mode0 里真正影响出图的字段

```dts
mclk_khz = "54000";
num_lanes = "1";
tegra_sinterface = "serial_a";
phy_mode = "DPHY";
discontinuous_clk = "yes";
lane_polarity = "6";          /* 见下一节 */
csi_pixel_bit_depth = "10";
mode_type = "bayer";          /* 黑白改 "y" / pixel_phase = "y" */
pixel_phase = "rggb";         /* 彩色；用户态还要以实测为准 */
active_w = "1456";
active_h = "1088";
line_length = "1760";
pix_clk_hz = "118800000";
min_exp_time = "15";
max_exp_time = "15699";
default_exp_time = "10000";
min_gain_val = "0";
max_gain_val = "480";         /* 0.1 dB 步进，最大 48 dB */
max_framerate = "60000000";   /* tegracam 因子 1e6，即 60 fps */
default_framerate = "60000000";
embedded_metadata_height = "2";
```

彩色 vs 黑白 **只改 DT**，不要改 `nv_imx296.c`：

- 彩色 LQ：`mode_type = "bayer"`，`pixel_phase = "rggb"`
- 黑白 LL：`mode_type = "y"`，`pixel_phase = "y"`

`tegra-camera-platform` 的 `badge` 可以写成 `imx296lq_front_IMX296LQ`。这只给 Argus 用；反正 Argus 走不通，badge 对不对不影响 V4L2。

### 2.4 `lane_polarity`：抄同座已验证摄像头，不要猜

A603 上 IMX219 已经在 `serial_a` 上工作，极性是 **6**（lane0 翻转）。IMX296 直接抄 6，第一次出图就对齐。

如果新载板没有可抄的参考：

1. 先保证 I2C probe 成功、`/dev/video0` 出现。
2. 只改 `lane_polarity`，依次试 `0` / `2` / `4` / `6`。
3. 症状是花屏、整帧错位、全绿，而不是偏色。偏色是 Bayer 相位 / ISP，不要在这里扭极性。

编 DTBO：

```bash
./build-overlay.sh /path/to/Linux_for_Tegra
```

脚本会 `cpp` → `dtc -@` → `fdtoverlay` 合到 A603 base DTB。换载板时改 `BASE_DTB` 路径。

---

## 3. 驱动：复用 tegracam，禁止让主线 imx296 先探

### 3.1 为什么必须是 `nv_imx296`

主线 `imx296.ko` 和 NVIDIA tegracam 都匹配 `compatible = "sony,imx296"`。主线先探、要 sensor clock、失败，然后把 tegracam 挡住。板上必须：

```
# /etc/modprobe.d/blacklist-mainline-imx296.conf
blacklist imx296
install imx296 /sbin/modprobe --ignore-install nv_imx296
```

以及 `/etc/modules-load.d/nv_imx296.conf` 里写一行 `nv_imx296`。

驱动认三个 compatible：`sony,imx296` / `sony,imx296lq` / `sony,imx296ll`。overlay 用 `sony,imx296` 即可。

### 3.2 复位时序（A603 实测）

MB1 把 PH.06 留在 **输出低**。驱动 `imx296_power_on()`：

1. GPIO 拉低（复位）
2. 再拉高（释放）
3. Sony 传感器复位后要等足够长的启动时间，再读 ID

DT 里 `reset-gpios = <&gpio PH.06 GPIO_ACTIVE_HIGH>`。驱动自己按 **低有效复位** 来拉脚，不要再套一层 hog 去抢这根脚。

Probe 失败（I2C NACK）时先做：

```bash
# 确认 overlay 生效、节点在
ls /proc/device-tree/bus@0/i2c@3180000/imx296_a@1a
i2cdetect -y -r <bus>     # 0x1a 应在复位释放后出现
dmesg | grep -E 'imx296|nv_imx296|tegra-cam'
```

### 3.3 交叉编译：vermagic 和 CRC 必须对板上模块

`build-driver.sh` 有两处不能省：

1. `LOCALVERSION=-tegra`，vermagic 必须是 `5.15.148-tegra`。对不上 `modprobe` 直接拒。
2. **不要用** `nvidia-oot/Module.symvers` 里的 CRC。同一 L4T 版本，OOT 编出来的 CRC 和板上 `tegra-camera.ko` 仍可能不同。脚本用 `nm` 从 **实际刷进去的** `tegra-camera.ko` 抽 `__crc_*`，重写一份 `build/tegra-camera.symvers`，再当 `KBUILD_EXTRA_SYMBOLS`。

换载板 / 换镜像时，把 `TEGRA_CAMERA_KO` 指到 **这块板或这份 rootfs 里的** `tegra-camera.ko`。

驱动源码在 `driver/tegracam/`（A603 目录是指向 J401 的 symlink）。mode 表、曝光公式来自 libcamera `cam_helper_imx296.cpp`，增益上限 480。

用户态要手动曝光时必须：

```bash
v4l2-ctl -d /dev/video0 -c override_enable=1,bypass_mode=0,gain=80,exposure=10000,frame_rate=60000000
```

不设 `override_enable=1`，后面的 gain/exposure 会被忽略。

---

## 4. 出图诊断：先 RAW，再相位，再 ISP

顺序错了会浪费几天。A603 上真实走过的弯路：

### 4.1 GStreamer 不能直接吃 RG10

JetPack 6.2 的 GStreamer 1.20 **不能**协商 V4L2 `RG10`。这条必挂 `not-negotiated`：

```bash
# 不要用
gst-launch-1.0 v4l2src device=/dev/video0 ! video/x-bayer,format=rggb,width=1456,height=1088 ! bayer2rgb ! ...
```

正确路径：`camview` mmap 10-bit Bayer，做 demosaic + ISP，stdout 吐 RGB8，再交给 GStreamer `fdsrc ! rawvideoparse format=rgb`。

格式确认：

```bash
v4l2-ctl -d /dev/video0 --all | sed -n '1,80p'
# 期望：1456x1088, RG10 / RGGB, stride=2912
```

`1456*2 = 2912`，已经 32-byte 对齐。如果换分辨率后 stride 不是 64 的倍数，Orin VI 会在行尾拆数据；那时才需要 `preferred_stride`。mode0 全分辨率在 A603 上 **不需要**。

### 4.2 花屏 / 全绿 vs 品红

| 现象 | 原因 | 动作 |
| --- | --- | --- |
| 花屏、错位、全绿 | CSI 极性 / lane / 复位没好 | 改 `lane_polarity`，查 GPIO |
| 能认出办公室，但整幅品红/偏红 | Bayer 相位错，或没做 AWB/CCM | 改 `camview` 的 CFA，打开 ISP |
| 前几帧全黑，后面正常 | 曝光还没锁 | `CAMVIEW_SKIP=3`（默认已跳 3 帧） |
| 只有十几 fps | 用户态把 `frame_rate` 锁死了 | 不要在 `view-imx296` 里写 18 fps |

A603 上 DT `pixel_phase = "rggb"`，用户态默认也要 **rggb**。早期 `view-imx296` / `stream-imx296` 默认 `gbrg`，办公室场景均值大约 210/169/213（品红）。改成 rggb + ISP 后大约 55/50/46，颜色正常。

换载板时用同一场景扫四个相位：

```bash
for p in rggb gbrg grbg bggr; do
  camview /dev/video0 $p 2 > /tmp/$p.rgb
done
```

看哪张皮肤/白墙不品红。DT 的 `pixel_phase` 尽量和用户态一致，免得下一个人又抄错。

### 4.3 帧率

传感器 mode0 就是 60 fps。A603 / Orin NX 上 `camview` 1456×1088：

- 1.1.0 只有 AWB/CCM/gamma 时约 **59.5 fps**，瓶颈在 bilinear demosaic。
- 1.1.2 为了压灯管拉链，假彩色抑制看 **3x3 demosaic 峰值**，OpenMP 每像素多读邻域，stdout 抓帧大约 **43 fps**。桌面 `gtksink` 预览还会再低一点。

传感器和 CSI 仍是 60 fps；慢的是 CPU ISP。推流走 `nvv4l2h264enc` 时编码才是后面的瓶颈。桌面预览卡，先分清是 `videoconvert ! gtksink` 还是 1.1.2 的邻域去饱和。

曝光范围 15–15700 µs，增益 0–480（0.1 dB）。室内暗场可用 `EXPOSURE=15699 GAIN=160`。

---

## 5. ISP：放到板上共享文件，预览和推流共用

NVIDIA 没有这颗传感器的 NITO。颜色科学来自 Raspberry Pi / libcamera 的 `imx296_16mm.json`，**不要**试图塞进 Argus。

### 5.1 两份文件

| 文件 | 作用 |
| --- | --- |
| `isp/imx296_16mm.json` | 上游调校原文（黑电平、CT 曲线、多组 CCM、gamma） |
| `isp/imx296-isp.json` | `camview` 读的运行时表（Q10 整数、1024 项 gamma LUT） |

从原文重生成运行时表：

```bash
python3 isp/export-runtime.py
```

运行时表内容（A603 实测可用，1.1.2）：

- `cfa`: `rggb`
- `black_10`: 60（libcamera 16-bit 黑电平 3840 / 64）
- `ccm_q10`: 4560 K 那组 CCM
- `ct_*`: AWB 色温曲线，灰世界结果投影到最近的 CT
- `gamma_lut`: RPi `rpi.contrast.gamma_curve` 重采样到 10-bit
- `hl_start` / `hl_full`: 裁切灯芯往白拉，默认 **780..940**
- `fc_start` / `fc_full`、`fc_ratio_lo` / `fc_ratio_hi`: 亮且通道失衡的 Bayer 拉链先往 **luma** 去饱和，默认 **220..500**、**250..650**（Q10）。亮度看 3x3 峰值，避免漏掉较暗的拉链圈

`camview` 加载顺序：

1. `CAMVIEW_ISP_FILE`（若设置）
2. `/etc/imx296-a603/imx296-isp.json`（本地覆盖）
3. `/usr/share/imx296-a603/imx296-isp.json`（包默认）
4. 都没有则用编译进二进制的内置表

`CAMVIEW_ISP=0` 只做 demosaic + gamma，方便对比“相位对不对”和“校色对不对”。

换载板时：

- **同一颗彩色 IMX296LQ**：直接复用这两份 JSON，只改安装路径前缀（不要写死 `imx296-a603` 也行，但要同时改 `camview.c` 里的默认路径）。
- **黑白 IMX296LL**：`view-imx296 mono`，不要走 Bayer ISP。
- **换镜头 / 要更准的颜色**：改 `imx296_16mm.json` 或只改 `ccm_q10`，再 `export-runtime.py`。不要为了调色去改驱动。
- **灯管发紫 / 边缘拉链**：改 `hl_*` / `fc_*`，这两项不在 RPi JSON 里，`export-runtime.py` 会原样写回默认值。现场覆盖用 `/etc/imx296-a603/imx296-isp.json`。

### 5.2 为什么 ISP 不能只埋在 camview 里

`view-imx296` 和 `stream-imx296` 都必须 `exec camview`。如果推流自己 `v4l2src` 或把 `camview` 调成 `bayer` 且关掉 ISP，网上看到的就是未校色的品红 Bayer。A603 1.1.2 已统一：

- 默认相位 `rggb`
- 默认 60 fps
- 读同一份 `/usr/share/imx296-a603/imx296-isp.json`
- 1.1.2：灯芯去紫（`hl_*`）和拉链去饱和（`fc_*`）也在这份 JSON 里，预览和推流一起生效

### 5.3 灯管发紫 ≠ 灯管边缘拉链

办公室日光灯会同时踩中两件不同的事，不要用同一个旋钮修：

| 现象 | 原因 | 修法 | 不要做 |
| --- | --- | --- | --- |
| 灯管**芯**品红/紫 | 裁切白点经 AWB 后不再中性，4560 K CCM 把 G 打成负值 | `hl_start`/`hl_full` 把**该像素自己**往纸白拉 | 把 CCM 改“更不饱和”来保灯管，办公室墙壁会一起灰掉 |
| 灯管**外圈**青/橙/品红拉链 | Bayer 绿通道先饱和，bilinear 插值 + CCM 在邻域造假彩色；这些像素自身峰值只有 ~200–500，低于 `hl_start` | `fc_*` 按“亮且通道失衡”往 **luma** 靠；亮度用 3x3 demosaic 峰值，才能带上较暗的拉链圈 | 降低 `hl_start`、对邻域均值去饱和、或 clip-aware demosaic 把裁切样点扩进插值 — 都会在灯管外画出一圈**白雾** |

A603 办公室这帧上验证过：场景里真正的彩色物体（纸箱、地板）线性峰值 < ~200/1023，`fc_start=220` 几乎不动它们。拉链像素 `pre_max` 中位在 300–900。

`camview` 1.1.2 的顺序是 **AWB → CCM → 假彩色往 luma → 负通道回 luma → 裁切往白 → gamma**。白恢复必须在假彩色之后、且只用像素自己的峰值，否则拉链圈会被涂白。

---

## 6. 预览和推流

### 6.1 本地预览

桌面会话里：

```bash
view-imx296
EXPOSURE=15699 GAIN=160 view-imx296    # 暗环境
CAMVIEW_ISP=0 view-imx296              # 只看 demosaic
view-imx296 gbrg                       # 扫相位
```

`view-imx296` 内部等价于：

```bash
v4l2-ctl -d /dev/video0 -c override_enable=1,bypass_mode=0,gain=80,exposure=10000,frame_rate=60000000
camview /dev/video0 rggb | gst-launch-1.0 fdsrc ! \
  rawvideoparse format=rgb width=1456 height=1088 framerate=60/1 ! \
  videoconvert ! gtksink sync=false
```

前 3 帧默认丢掉（`CAMVIEW_SKIP`），避免曝光未锁的黑场进预览/录像。

### 6.2 网络推流（要“完好”，必须走同一 ISP）

板上：

```bash
stream-imx296                          # TCP MPEG-TS :8554，硬件 H.264
stream-imx296 --protocol udp --host <PC-IP> --port 5000
stream-imx296 --preview                # 边推边本地看
```

PC：

```bash
vlc tcp://<jetson-ip>:8554
ffplay tcp://<jetson-ip>:8554
```

推流管道（有 `nvv4l2h264enc` 时）：

```
camview /dev/video0 rggb
  → fdsrc
  → rawvideoparse format=rgb width=1456 height=1088 framerate=60/1
  → videoconvert ! video/x-raw,format=I420
  → nvvidconv ! video/x-raw(memory:NVMM),format=NV12
  → nvv4l2h264enc bitrate=4000000 insert-sps-pps=true
  → h264parse ! mpegtsmux ! tcpserversink
```

没有硬件编码才回退 `x264enc`。不要把 RAW Bayer 直接塞给 `nvv4l2h264enc`。

换载板时推流脚本通常 **不用改逻辑**，只确认：

- `list_imx296_nodes` 仍按 sysfs `name` 含 `imx296` 找节点
- 默认 `--index 0` 是你的 CAM0
- `--fps` 默认 60，并写入 `frame_rate=$((FPS * 1000000))`
- `--phase` 默认 `rggb`（黑白板改 `mono`）

### 6.3 `/dev/video0` busy

`S_FMT: Device or resource busy` 就是已有 `camview` / `view-imx296` / `stream-imx296` 占着。先：

```bash
sudo fuser -v /dev/video0
sudo fuser -k /dev/video0
```

---

## 7. 换一块载板的推荐顺序

按这个顺序做，不要一上来改 ISP 或推流。

1. **抄硬件表**：座子、CSI brick、lane 数、I2C 总线、复位 GPIO、有没有同座 IMX219。用 GPIO 翻转 + `i2cdetect` 证明 `0x1a` 受复位控制。
2. **改 overlay**：disable 旧摄像头，改 `tegra_sinterface` / `port-index` / `reset-gpios` / `lane_polarity`。`fdtoverlay` 合到 **该板** base DTB，确认标签解析、没有重复 status。
3. **复用驱动**：`./build-driver.sh`，CRC 来自该镜像的 `tegra-camera.ko`。deb 的 `postinst` 把 `.ko` 拷到 `/lib/modules/$(uname -r)/updates/drivers/media/i2c/` 并 `depmod`。黑名单主线 `imx296`。
4. **Jetson-IO + reboot**，确认 `v4l2-ctl --list-devices` 看到 imx296，格式 1456x1088 RG10。
5. **RAW 冒烟**：`camview /dev/video0 rggb 8 >/tmp/t.rgb`，stderr 应有 `isp file /usr/share/... hl=780..940 fc=220..500`。1.1.2 stdout 大约 43 fps（传感器仍 60）。花屏就回到步骤 2 改极性。
6. **扫 Bayer 相位**，把默认 CFA 写进 `view-imx296` / `stream-imx296` / ISP JSON。
7. **装 ISP 文件** 到 `/usr/share/<board>/imx296-isp.json`，保证两个工具都走 `camview`。
8. **推流**：`stream-imx296`，PC 上 VLC 打开 TCP。颜色应和本地 `view-imx296` 一致。对着日光灯看：灯芯白、外圈没有青橙拉链、也没有白雾。
9. **打包**：dtbo + ko + ISP JSON + `camview`/`view`/`stream` + overlay/驱动源码进同一个 deb，避免现场只更新了预览、推流还是旧的 GBRG。

---

## 8. 这次 A603 上踩过、换板几乎还会踩的坑

1. **Argus 没表就没有彩**。不要花时间找 `.nito` 或改 badge 幻想 `nvarguscamerasrc` 能出正确颜色。
2. **主线 `imx296` 会抢 compatible**。不黑名单就会“有节点、没 video、dmesg 报 clock”。
3. **复位脚不是板子丝印上随便一个 CAM_RST**。A603 的 PH.03 hog 和 15-pin PWDN 不是同一根。
4. **`lane_polarity` 抄同座已工作的传感器**。乱试四次极性比猜原理快。
5. **品红不是 CSI 坏了**。相位错或没 AWB/CCM 时，办公室白墙就是品红。
6. **DT `pixel_phase` 和用户态 CFA 都要改**。只改一个，另一个工具立刻回到品红。
7. **GStreamer 1.20 不认 RG10**。所有预览/推流必须经 `camview` 转 RGB8。
8. **ISP 必须是板上文件**。埋进 `camview` 二进制后，推流或别的工具绕开它就会各走各的颜色。
9. **不要把 fps 锁成 18**。那是早期调试残留。传感器 mode0 仍是 60；1.1.2 的 3x3 假彩色抑制会把 `camview` stdout 降到大约 43 fps，不要回头把 `frame_rate` 写成 18。
10. **OOT `Module.symvers` CRC ≠ 板上 `tegra-camera.ko`**。模块能编出来，`modprobe` 因 CRC 失败。
11. **前几帧黑是曝光**，用 skip，不要当 CSI 丢帧。
12. **`override_enable=1` 忘记设**，曝光增益怎么写都不动。
13. **deb 要带 `libgomp1`**。`camview` 链了 OpenMP。
14. **Jetson-IO 只记录 overlay 名**。如果载板还依赖“先叠 IMX219 dtbo 再叠 IMX296”（J401 就是），要在 `extlinux.conf` 里确认顺序；A603 是底板已焊 IMX219、overlay 里 disable，不需要再叠一份 IMX219 dtbo。
15. **灯管紫芯和灯管边缘拉链不是同一个 bug**。紫芯用 `hl_*` 往白拉；拉链用 `fc_*` 往 luma 拉。把 `hl_start` 降到 500 会修掉拉链，但灯管外圈变成白雾。
16. **假彩色亮度必须看邻域峰值，白恢复不能看邻域**。拉链圈比裁切点更宽、更暗；3x3 max 能罩住它。若 3x3 也参与 `hl_*`，又会扩出白边。

---

## 9. 验收清单（换板签字用）

- [ ] `i2cdetect` 在复位释放后看到 `0x1a`，拉复位后消失
- [ ] `modinfo nv_imx296` vermagic = `5.15.148-tegra`，`modprobe` 无 CRC 错
- [ ] `v4l2-ctl --list-devices` 有 imx296；`--all` 为 1456x1088 RG10
- [ ] `camview` stderr：`isp file /usr/share/.../imx296-isp.json (black=60 ct_n=5 hl=780..940 fc=220..500 r=250..650)`。1.1.2 stdout 抓帧大约 43 fps（传感器仍 60）
- [ ] 本地 `view-imx296` 办公室场景不品红、不全绿、前几帧过后稳定
- [ ] 日光灯灯芯是白的，不是紫的；灯管外圈没有青/橙拉链，也没有一圈白雾
- [ ] `stream-imx296` 与 `view-imx296` 颜色一致；PC 上 `vlc tcp://<ip>:8554` 能看
- [ ] `CAMVIEW_ISP=0` 能看出“未校色但相位对”的图，方便以后调 CCM
- [ ] 包内同时有 dtbo、ko、ISP、view、stream，现场只装一个 deb

---

## 10. 源码地图

```
leetop_a603_imx296_jp62/
  tegra234-p3767-camera-603-imx296lq-overlay.dts
  build-overlay.sh / build-driver.sh / build-deb.sh
  driver/tegracam/          # symlink → J401 nv_imx296.c
  isp/imx296_16mm.json      # libcamera 原文
  isp/imx296-isp.json       # camview 运行时表（含 hl_* / fc_*）
  isp/export-runtime.py     # 从 RPi JSON 重生表；hl/fc 是脚本里的默认值
  isp/README.md             # 高光 / 拉链旋钮说明
  tools/camview.c           # RG10 → RGB8 + CPU ISP（1.1.2 含 3x3 假彩色抑制）
  tools/view-imx296         # 本地 GTK
  tools/stream-imx296       # H.264 MPEG-TS
  docs/PORTING.md           # 本文件
```

驱动重编入口在 J401 目录的 `build-driver.sh`；A603 的 `build-driver.sh` 只是调用它再把 `.ko` 拷过来。换 L4T 版本时先改那一份。
