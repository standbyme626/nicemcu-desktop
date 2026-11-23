<div align="center">

# nicemcu-desktop

**把一块 2.8 寸触摸屏变成桌面仪表盘 + 媒体遥控器**

本地仪表盘 · 天气 · 番茄钟 · 媒体封面/进度 · 队列切歌 · 中文界面

![ESP32-S3](https://img.shields.io/badge/ESP32--S3-N16R8-e7352c)
![LVGL](https://img.shields.io/badge/LVGL-9.6-4f8cff)
![Display](https://img.shields.io/badge/Display-320%C3%97240-3ddc84)
![License](https://img.shields.io/badge/License-MIT-green)

[项目概览](#1-项目概览) · [核心特性](#2-核心特性) · [界面](#3-界面) · [系统架构](#4-系统架构) · [快速开始](#5-快速开始) · [媒体 API](#6-媒体页与-api) · [构建](#7-从源码构建) · [已知限制](#8-已知限制) · [目录结构](#9-目录结构) · [安全须知](#10-安全须知) · [Roadmap](#11-roadmap)

</div>

> **项目定位：第二块桌面屏 + 遥控器，不是播放器。**
> 设备不下载视频、不出声、不保存 B 站 cookie；只显示播放状态、发送控制指令。
> 音频/视频仍在 PC 的受控浏览器里播放。

**ESP32-S3** · **LVGL 9.6** · **横屏 320×240** · **中文字库自带** · **按需轮询（不拖慢 UI）**

---

## 目录

- [1. 项目概览](#1-项目概览)
- [2. 核心特性](#2-核心特性)
- [3. 界面](#3-界面)
- [4. 系统架构](#4-系统架构)
- [5. 快速开始](#5-快速开始)
- [6. 媒体页与 API](#6-媒体页与-api)
- [7. 从源码构建](#7-从源码构建)
- [8. 已知限制](#8-已知限制)
- [9. 目录结构](#9-目录结构)
- [10. 安全须知](#10-安全须知)
- [11. Roadmap](#11-roadmap)

---

## 1. 项目概览

### 1.1 要解决的问题

书桌上已经有一台在放 B 站音乐的电脑，但：

- 想看时间/天气/番茄钟，不想再点亮显示器；
- 想切歌/调音量，不想切窗口、找网页、掏手机；
- 手机遥控 App 有，但每次解锁手机太麻烦。

### 1.2 方案

用 NiceMCU-S3-2.8IPS（ESP32-S3 + 2.8" IPS 触摸屏）做一块常亮的桌面小屏：

- **本地**：大时钟、日期、天气、设备状态、番茄钟，开机即用；
- **遥控**：作为 [media-control](https://github.com/standbyme626/media-control) 的瘦客户端，显示封面/标题/进度，控制播放/音量/进度/定时/队列；
- **不越界**：设备只发 HTTP 请求，不解析 B 站视频、不解码音频、不保存登录态。

### 1.3 硬件

| 项 | 值 |
|---|---|
| 板子 | NiceMCU-S3-2.8IPS（XH-S3E，ESP32-S3-WROOM-1-N16R8） |
| SoC | ESP32-S3 双核 240MHz，16MB Flash，8MB PSRAM（当前未启用） |
| 屏幕 | ST7789P3 2.8" IPS 240×320，SPI 40MHz，固件内横屏 320×240 |
| 触摸 | T6336G（CST816T/D 协议，I2C 0x15） |
| 其它 | MicroSD（4-bit SDIO 38/40/39/41/48/47）、电池充电 + ADC(IO1)、红外(IO13/14)、MIC + NS4168 功放（本固件未使用） |

**引脚**

| 功能 | GPIO |
|---|---|
| LCD SCK / MOSI / CS / DC / BL | 12 / 11 / 10 / 9 / 45 |
| 触摸 SDA / SCL / INT | 17 / 18 / 8 |
| BOOT 键 | 0 |

---

## 2. 核心特性

| 能力 | 实现 | 说明 |
|---|---|---|
| 仪表盘 | LVGL 9.6 + 2×2 卡片 | 时钟、日期、天气、设备状态、番茄钟，横屏 320×240 |
| 天气 | open-meteo（HTTP）+ SNTP | 默认广州坐标，可改；15 分钟刷新，失败 30s 重试 |
| 网络状态 | WiFi STA + esp_wifi | SSID/信号/IP/内存/运行时长，断线 5s 自动重连 |
| 番茄钟 | 状态机 + 1s 定时器 | 25 分钟工作 / 5 分钟休息自动切换；点按开始/暂停，长按重置 |
| 设置弹层 | LVGL 自定义面板 | 亮度滑条、刷新天气、息屏、关于、关闭 |
| 媒体封面 | `/api/cover` → 80×80 RGB565 | 服务端裁剪，设备零解码；封面变化才刷新 |
| 媒体状态 | `/api/status` 轮询 | 标题/进度/播放状态/队列位置；**只在媒体页打开时轮询（2s）** |
| 播放控制 | HTTP Basic POST | 播放/暂停、上一首/下一首、静音、系统音量、拖动进度、定时暂停 |
| 队列 | `/api/queue/status` + `/api/queue/jump` | 最多 60 条，每条两行标题，当前项高亮，点按切歌 |
| 中文字体 | Noto Sans SC 子集 + GB2312 全量 | 静态 UI 用子集（省 Flash），动态标题用 7445 字全量；缺字显示 `?` |
| 触摸 | CST816T 轮询 | 横屏 `swap_xy + mirror_x` 映射，双缓冲 DMA 刷屏 |
| 性能 | 双缓冲 + DMA 回调 | 40MHz SPI，脏区局部刷新；静态页面几乎不占 CPU |

---

## 3. 界面

```
┌──────────────────────────── 320×240 ────────────────────────────┐
│ ┌────────────────┐ ┌──────────────┐                             │
│ │ 15:42          │ │ 天气          │                             │
│ │ 11月23日 周日   │ │ 22°C  阴      │                             │
│ │      媒体  设置 │ │ 湿度 86%      │                             │
│ └────────────────┘ └──────────────┘                             │
│ ┌────────────────┐ ┌──────────────┐                             │
│ │ 设备状态        │ │ 番茄钟        │                             │
│ │ WiFi -30dBm    │ │ 25:00        │                             │
│ │ IP 192.168.x.x │ │ 待命          │                             │
│ └────────────────┘ └──────────────┘                             │
└─────────────────────────────────────────────────────────────────┘
```

```
媒体页：封面（80×80） + 4 行标题               [×]
        播放中                        [队列]
        ─────────────●──────────────────────
        00:12                          38:14
        [⏮] [▶/⏸] [⏭] [🔇] [定时]
        音量  ━━━━━━●━━━━━━
```

- 首页默认显示仪表盘；「媒体」「设置」在时钟卡右下角
- 息屏后触摸唤醒；亮度通过 LEDC PWM 调节
- 队列页从媒体页进入，切歌后自动返回媒体页

---

## 4. 系统架构

```
┌────────────── ESP32 (this firmware) ──────────────┐
│  LVGL 9.6 (320×240, 双缓冲 DMA)                    │
│   ├─ 仪表盘 / 番茄钟 / 设置                         │
│   └─ 媒体页 / 队列页                               │
│  主循环：lv_timer_handler + 1s UI 定时器           │
│  任务：weather_task（15min）、media_task（按需 2s）│
└───────────────┬───────────────────────────────────┘
                │ HTTP Basic（局域网）
                ▼
        media-web :8097（媒体控制服务）
                │ 常驻 SSH 隧道 / CDP
                ▼
        Windows bridge :8765
                │ Chrome DevTools Protocol
                ▼
        受控 Edge → B 站播放器（声音在 PC）
```

| 模块 | 位置 | 职责 |
|---|---|---|
| 显示/触摸 | `main/main.c` | ST7789 SPI、CST816T I2C、横屏映射、LVGL flush |
| 网络 | `main/main.c` | WiFi STA、SNTP、open-meteo 天气 |
| 媒体客户端 | `main/main.c` | 状态轮询、命令队列、封面、队列、JSON 解析 |
| UI | `main/main.c` | 仪表盘、番茄钟、设置弹层、媒体页、队列页 |
| 字体 | `main/fonts/` | Noto Sans SC 子集（16/24/48）+ GB2312 全量（16） |

---

## 5. 快速开始

**设备侧**

```bash
git clone https://github.com/standbyme626/nicemcu-desktop.git
cd nicemcu-desktop
git clone --depth 1 --branch v9.6.0 https://github.com/lvgl/lvgl.git components/lvgl

idf.py set-target esp32s3
idf.py menuconfig   # Desktop -> WiFi SSID/password、media-web host/user/password
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

**服务端**

- 需要 [media-control](https://github.com/standbyme626/media-control) **3.6.0+**，并已按它的 README 配好 Windows 桥 + 受控 Edge；
- 设备只访问 `http://<media-web>:8097` 的 `/api/*`，Basic 认证；
- 服务端不需要额外配置，`/api/cover`、`/api/queue/jump` 已在 3.6.0 内置。

**首次使用**

1. 上电后自动连 WiFi、SNTP 对时、拉天气；
2. 点时钟卡「媒体」→ 显示当前 PC 上的 B 站播放内容；
3. 点「队列」→ 选一条切歌；
4. 「设置」→ 亮度/刷新天气/息屏。

---

## 6. 媒体页与 API

设备只依赖以下 media-control 端点（全部 Basic 认证）：

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/api/status` | 播放状态（title/pos/dur/volume/queue/bv/pc_online） |
| GET | `/api/cover?bv=` | 80×80 RGB565 封面原始字节 |
| GET | `/api/queue/status` | 队列列表 |
| GET | `/api/queue/jump?i=` | 切到队列第 i 条并打开 |
| POST | `/api/play` `/api/pause` `/api/next` `/api/prev` `/api/mute` | 播放控制 |
| POST | `/api/volume` (`level=0..100`) | 系统音量 |
| POST | `/api/seek` (`pos=秒`) | 播放进度 |
| POST | `/api/sleep` (`min=0/30/60/90`) | 定时暂停 |

> media-control 对 Basic 认证的 POST 跳过 CSRF（浏览器 Cookie 流程仍强制 CSRF），设备不需要页面 token。

---

## 7. 从源码构建

### 7.1 环境

- **ESP-IDF v6.0**（工具链 esp32s3）
- **LVGL v9.6.0** 放在 `components/lvgl`（`.gitignore` 已忽略，不提交）
- `sdkconfig` 不提交；首次 `menuconfig` 填 WiFi / media-web 凭据

### 7.2 分区与 Flash

- `partitions.csv`：NVS 24KB + app 4MB + 16MB Flash
- 当前未启用 PSRAM；封面/字体在内部 RAM/Flash
- 字体文件较大（全量 16px 约 6MB 源码），app 约 2.3MB，分区余量约 41%

### 7.3 中文字库

- 静态 UI：`font_zh16/24/48`（Noto Sans SC 子集，含 FontAwesome 符号）
- 动态标题：`font_zhfull`（GB2312 全部 7445 字 + 全角标点）
- 字库由 `lv_font_conv` 生成；如需增删字符，重新生成后替换 `main/fonts/`

### 7.4 触摸/屏幕适配

- 屏幕横屏使用：`esp_lcd_panel_swap_xy(true)` + `mirror(true, false)`
- 触摸坐标映射：`x = raw_y`，`y = 239 - raw_x`
- 换板/换屏需要重新确认映射和背光 PWM

---

## 8. 已知限制

- **媒体页只是遥控 + 状态**：设备不播音频/视频，声音在 PC；
- 设备不登录 B 站，完全依赖 media-control 的登录态；
- 40MHz SPI，整屏重绘约 30ms，大面积动画上限约 30fps；
- 动态标题限 GB2312，emoji/生僻字显示 `?`；
- 队列标题每条最多两行，超长省略；
- 触摸映射针对本板横屏校准；换板可能镜像/偏移；
- 未启用 PSRAM、无 OTA、无 SD 卡音乐播放（硬件支持，固件未做）。

---

## 9. 目录结构

```
nicemcu-desktop/
├── CMakeLists.txt
├── partitions.csv              # 16MB Flash / 4MB app
├── sdkconfig.defaults          # 已脱敏：WiFi / media 凭据留空
├── main/
│   ├── main.c                  # 显示/触摸/网络/媒体/UI 全部逻辑
│   ├── Kconfig.projbuild       # WiFi / media-web / 字体配置
│   └── fonts/                  # 生成的位图字体（Noto Sans SC + FA 符号）
├── components/lvgl -> ...      # 本地 LVGL 9.6（gitignore，不入库）
└── README.md
```

---

## 10. 安全须知

- 仓库**不含** WiFi 密码、media-web 账号、内网 IP；请通过 `menuconfig` 本地填写；
- 设备仅访问局域网 media-web，不直连 B 站、不保存 cookie；
- media-control 的 Basic 凭据会出现在设备 sdkconfig 中（本地文件），不要把 `sdkconfig` 提交/分享；
- 若要公网访问 media-web，请务必走 HTTPS + 强密码（详见 media-control 安全模型）；
- 本项目不提供、不托管任何视频内容，仅用于个人设备的状态显示与遥控。

---

## 11. Roadmap

- [x] 横屏 2×2 仪表盘 + 番茄钟 + 设置
- [x] media-control 薄客户端（封面/状态/控制/队列）
- [x] GB2312 全量字库 + 标点支持
- [ ] 队列懒加载（>60 条）
- [ ] SD 卡本地音乐/白噪音
- [ ] 大肥鱼桌宠页（MIT 素材，JPEG 帧 + PSRAM）
- [ ] OTA / 配置网页
- [ ] PSRAM 帧缓冲（提升动画帧率）

---

## License

[MIT](LICENSE) © standbyme626

第三方字体/图标见 [NOTICE](NOTICE)：LVGL (MIT)、Noto Sans SC (OFL)、FontAwesome Free。
