<div align="center">

# nicemcu-desktop

**把一块 2.8 寸触摸屏变成桌面仪表盘 + 媒体遥控器**

本地仪表盘 · 天气 · 番茄钟 · 媒体封面/进度 · 队列切歌 · 中文界面 · OTA 双槽 · PSRAM · 截图 · 配网 · 队列分页 · OTA 进度

![ESP32-S3](https://img.shields.io/badge/ESP32--S3-N16R8-e7352c)
![LVGL](https://img.shields.io/badge/LVGL-9.6-4f8cff)
![Display](https://img.shields.io/badge/Display-320%C3%97240-3ddc84)
![OTA](https://img.shields.io/badge/OTA-A%2FB%20%2B%20rollback-3ddc84)
![PSRAM](https://img.shields.io/badge/PSRAM-8MB%4080MHz-8a2be2)
![HTTP](https://img.shields.io/badge/HTTP-API%20%2B%20screenshot-4f8cff)
![License](https://img.shields.io/badge/License-MIT-green)

[项目概览](#1-项目概览) · [外设利用](#14-板载外设利用情况) · [核心特性](#2-核心特性) · [界面](#3-界面) · [系统架构](#4-系统架构) · [快速开始](#5-快速开始) · [媒体 API](#6-媒体页与-api) · [设备 API](#62-设备-http-api) · [构建](#7-从源码构建) · [OTA](#8-ota-更新) · [已知限制](#9-已知限制) · [目录结构](#10-目录结构) · [安全须知](#11-安全须知) · [Roadmap](#12-roadmap) · [版本记录](#13-版本记录)

</div>

> **项目定位：第二块桌面屏 + 遥控器，不是播放器。**
> 设备不下载视频、不出声、不保存 B 站 cookie；只显示播放状态、发送控制指令。
> 音频/视频仍在 PC 的受控浏览器里播放。
> OTA 只走局域网 HTTP；首次迁移分区表必须 USB 刷入，不做公网升级。
> 本地 HTTP API 需要 token；配网 AP 只在配网模式短时开放。

**ESP32-S3** · **LVGL 9.6** · **PSRAM 8MB** · **截图/配网** · **OTA 双槽回滚** · **按需轮询（不拖慢 UI）**

---

## 目录

- [1. 项目概览](#1-项目概览)
- [1.4 板载外设利用情况](#14-板载外设利用情况)
- [2. 核心特性](#2-核心特性)
- [3. 界面](#3-界面)
- [4. 系统架构](#4-系统架构)
- [5. 快速开始](#5-快速开始)
- [6. 媒体页与 API](#6-媒体页与-api)
- [6.2 设备 HTTP API](#62-设备-http-api)
- [7. 从源码构建](#7-从源码构建)
- [8. OTA 更新](#8-ota-更新)
- [9. 已知限制](#9-已知限制)
- [10. 目录结构](#10-目录结构)
- [11. 安全须知](#11-安全须知)
- [12. Roadmap](#12-roadmap)
- [13. 版本记录](#13-版本记录)

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
| SoC | ESP32-S3 双核 240MHz，16MB Flash，8MB PSRAM（已启用，80MHz） |
| 屏幕 | ST7789P3 2.8" IPS 240×320，SPI 40MHz，固件内横屏 320×240 |
| 触摸 | T6336G（CST816T/D 协议，I2C 0x15） |
| 其它 | MicroSD（4-bit SDIO 38/40/39/41/48/47）、电池充电 + ADC(IO1)、红外 TX/RX(13/14)、MIC ZTS6672S(I2S 4/5/6/46)、NS4168 功放(I2S 16/15/7)、WS2812(42)、I2C 扩展(2/3)、IO21、UART、BLE 5 |

**引脚**

| 功能 | GPIO |
|---|---|
| LCD SCK / MOSI / CS / DC / BL | 12 / 11 / 10 / 9 / 45 |
| 触摸 SDA / SCL / INT | 17 / 18 / 8 |
| BOOT 键 | 0 |

### 1.4 板载外设利用情况

**当前固件已用**：LCD/背光、触摸、BOOT、WiFi、PSRAM/Flash、USB Serial/JTAG、UART0 console。

**板载已有但当前固件未用**（不需要外接模块）：

| 外设 | 官方引脚 | 官方 xiaozhi 示例 | 可以做到 |
|---|---|---|---|
| MIC ZTS6672S | I2S：WS4 / BCLK5 / DATA6 / SELECT46 | 用（左声道） | 录音、VAD、语音备忘录、语音助手/TTS |
| NS4168 功放 | I2S：LRCLK16 / BCLK15 / SDATA7 | 用（右声道） | 番茄钟提示音、白噪音、SD 音乐、TTS |
| WS2812 RGB | GPIO42 | 用 | WiFi/配网/OTA/录音状态灯、夜灯、番茄钟颜色 |
| microSD/TF | SDIO：CLK38 / CMD40 / D0 39 / D1 41 / D2 48 / D3 47 | 未用 | 本地音乐/白噪音、录音、截图/日志归档 |
| 红外 TX/RX | GPIO13 / GPIO14 | 未用（README 明确要自己加库） | 学习遥控器、媒体/空调控制 |
| 电池 ADC | GPIO1 | 未用 | 电量百分比、低电量提醒、UPS 模式 |
| I2C 扩展 | GPIO2 / GPIO3 | 未用 | AHT20/SHT30 温湿度、BH1750 光照、PCF8574 扩展 |
| IO21 | GPIO21 | 未用 | PIR 人体感应、DS18B20、继电器/按键 |
| BLE 5 | 芯片内 | 可选 BluFi，默认热点 | BLE 配网、BLE HID 媒体键 |
| UART 排针 | U0TXD / U0RXD | 只做 console | 外部 MCU/传感器/Modbus（日志切 USB） |

> 官方 xiaozhi 示例（`main/boards/nicemcu-s3-2.8ips`）只初始化了 LCD、MIC、功放、BOOT、WS2812、背光；**没有** touch/SD/IR/电池/I2C/IO21/UART 应用。触摸是当前固件的额外能力。
>
> 建议顺序：音频（MIC+NS4168，官方引脚可直接复用）→ RGB → SD/IR/电池 → I2C/IO21 → BLE/UART。

---

## 2. 核心特性

| 能力 | 实现 | 说明 |
|---|---|---|
| 仪表盘 | LVGL 9.6 + 2×2 卡片 | 时钟、日期、天气、设备状态、番茄钟，横屏 320×240 |
| 天气 | open-meteo（HTTP）+ SNTP | 默认深圳坐标（menuconfig 可改经纬度）；15 分钟刷新，失败 30s 重试 |
| 网络状态 | WiFi STA + NVS 凭据 | SSID/信号/IP/内存/运行时长；30s 重连、10 分钟失败重启 |
| 番茄钟 | 状态机 + 1s 定时器 | 25 分钟工作 / 5 分钟休息自动切换；点按开始/暂停，长按重置 |
| 设置弹层 | LVGL 自定义面板 | 亮度、刷新天气、息屏、检查更新、关于、关闭；长按关于进校准 |
| OTA 更新 | 双 4MB 槽 + manifest + 回滚 | 开机 15s 检查；进度/失败重试；下载后校验 project/version；联网确认后 mark_valid |
| 版本 | `version.txt` → `PROJECT_VER` | 关于页显示固件版本与当前槽位 |
| 本地 HTTP API | `:8080` + token | `/status` `/logs` `/sleep` `/wake` `/bright` `/reboot` `/ota/check` `/cfg` `/portal` |
| 截图通道 | PSRAM 影子缓冲 + BMP | `/screenshot.bmp` 320×240；`/ui` 远程切页 |
| WiFi 配网 | NVS + SoftAP 门户 + `/portal` | BOOT 长按或远程触发；扫描/保存/迷你 DNS；10 分钟超时 |
| 设置持久化 | NVS `desk` | 亮度/番茄钟/天气坐标/media host；`/cfg` 读写 |
| PSRAM | 8MB octal @80MHz | 截图/封面缓冲；LVGL draw buffer 仍在内部 RAM |
| 媒体封面 | `/api/cover` → 80×80 RGB565 | 服务端裁剪，设备零解码；封面变化才刷新 |
| 媒体状态 | `/api/status` 轮询 | 标题/进度/播放状态/队列位置；**只在媒体页打开时轮询（2s）** |
| 播放控制 | HTTP Basic POST | 播放/暂停、上一首/下一首、静音、系统音量、拖动进度、定时暂停 |
| 队列 | `/api/queue/page` + `/api/queue/jump` | 4 条/页 + 上下页；media-control 3.6.1+；当前项高亮 |
| 中文字体 | Noto Sans SC 子集 + GB2312 全量 | 静态 UI 用子集（省 Flash），动态标题用 7445 字全量；缺字显示 `?` |
| 触摸 | CST816T + 4 点校准 | 默认映射 `x=raw_y, y=239-raw_x`；仿射系数存 NVS |
| 性能 | 双缓冲 + DMA 回调 | 40MHz SPI，脏区局部刷新；PSRAM 放截图/封面，draw buffer 留内部 |

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
│  任务：weather（15min）、media（按需 2s）          │
│        ota_boot/ota_manual、httpd(:8080)、wifi_wd  │
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

```
OTA：设备 → GET manifest（局域网 HTTP） → 比较 version → 下载 app.bin
      → 写入另一 OTA 槽 → 校验 project/version → 重启 → 联网 15s 后 mark_valid

本地 API：/status /logs /sleep /wake /bright /reboot /ota/check /cfg /portal
截图：PSRAM 影子缓冲 → /screenshot.bmp（320×240 BMP）
配网：BOOT 长按或 /portal → AP NiceMCU-xxxx → /scan /save → 重启
```

| 模块 | 位置 | 职责 |
|---|---|---|
| 显示/触摸 | `main/main.c` | ST7789 SPI、CST816T I2C、横屏映射、LVGL flush |
| 网络 | `main/wifi_mgr.c` + `portal.c` | NVS 凭据、STA、30s/10min 看门狗、SoftAP 配网门户 |
| 本地 API | `main/net_server.c` | HTTP 8080、token、控制/诊断/截图/配网入口 |
| 屏幕 | `main/screen.c` | 背光/息屏/唤醒、PSRAM 影子缓冲、截图快照 |
| 设置 | `main/settings.c` | NVS 持久化、触摸仿射 |
| 诊断 | `main/diag.c` | 8KB 日志环、`/status` JSON |
| 媒体客户端 | `main/main.c` | 状态轮询、命令队列、封面、队列分页、JSON 解析 |
| OTA | `main/ota.c` | manifest、版本比较、进度/重试、校验、回滚标记 |
| UI | `main/main.c` | 仪表盘、番茄钟、设置弹层、媒体页、队列页、校准页 |
| 字体 | `main/fonts/` | Noto Sans SC 子集（16/24/48）+ GB2312 全量（16） |

---

## 5. 快速开始

**设备侧**

```bash
git clone https://github.com/standbyme626/nicemcu-desktop.git
cd nicemcu-desktop
git clone --depth 1 --branch v9.6.0 https://github.com/lvgl/lvgl.git components/lvgl

idf.py set-target esp32s3
idf.py menuconfig   # Desktop -> WiFi、media-web、天气经纬度、OTA URL、HTTP token
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

> 首次刷入必须 USB（分区表从 `factory` 改为 `ota_0` / `ota_1`）；之后可在「设置 → 检查更新」走 OTA。

**服务端**

- 需要 [media-control](https://github.com/standbyme626/media-control) **3.6.1+**（队列分页 `/api/queue/page`），并已按它的 README 配好 Windows 桥 + 受控 Edge；
- 设备只访问 `http://<media-web>:8097` 的 `/api/*`，Basic 认证；
- 服务端不需要额外配置，`/api/cover`、`/api/queue/jump`、`/api/queue/page` 已内置。

**首次使用**

1. 上电后优先用 NVS WiFi；无凭据/连接失败时开 `NiceMCU-xxxx` 配网 AP（也可 `GET /portal` 远程触发）；
2. 手机连 AP → 打开 `192.168.4.1`（SoftAP 示例地址）→ 选网/输密码 → 保存后自动重启连接；
3. 点时钟卡「媒体」→ 显示当前 PC 上的 B 站播放内容；
4. 点「队列」→ 4 条/页，上下页切歌；
5. 「设置」→ 亮度/刷新天气/息屏；长按「关于」进 4 点触摸校准；
6. 「设置 → 检查更新」→ 显示下载进度，完成后自动重启。

**本地 HTTP API**

```bash
# token 在 menuconfig 的 Desktop -> HTTP API token，未配置时 API 返回 503
curl -H "X-Token: <token>" http://<device>:8080/status
curl -H "X-Token: <token>" "http://<device>:8080/bright?p=30"
curl -H "X-Token: <token>" -o screen.bmp "http://<device>:8080/screenshot.bmp"
```

---

## 6. 媒体页与 API

### 6.1 媒体 API

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

### 6.2 设备 HTTP API

设备本地 `:8080`，全部需要 `X-Token` 头或 `?token=<token>`；token 为空时返回 503。只建议在局域网使用。

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/status` | 版本/槽位/reset/heap/PSRAM/IP/RSSI/OTA/队列/portal |
| GET | `/logs?n=` | 最近 N 行日志（8KB 环形缓冲） |
| GET | `/sleep` `/wake` | 息屏/唤醒（背光） |
| GET | `/bright?p=5..100` | 设置亮度并保存 |
| GET | `/reboot` | 重启 |
| GET | `/ota/check` | 手动触发 OTA 检查 |
| GET | `/screenshot.bmp` | 320×240 BMP 截图（PSRAM 影子缓冲） |
| GET | `/ui?page=dashboard|media|settings|queue|calib|about|ota` | 远程切页/打开弹窗 |
| GET | `/cfg` | 读取设置 JSON；带 `bright/pomo_work/pomo_break/lat/lon/media_host` 则保存 |
| GET | `/portal` | 触发 SoftAP 配网，返回 AP SSID |

---

## 7. 从源码构建

### 7.1 环境

- **ESP-IDF v6.0**（工具链 esp32s3）
- **LVGL v9.6.0** 放在 `components/lvgl`（`.gitignore` 已忽略，不提交）
- `sdkconfig` 不提交；首次 `menuconfig` 填 WiFi / media-web 凭据 / OTA manifest URL / HTTP API token

### 7.2 分区与 Flash

- `partitions.csv`：NVS 24KB + otadata 8KB + ota_0 4MB + ota_1 4MB + coredump 64KB（预留，未启用）+ 16MB Flash
- 首次从 `factory` 单槽迁移到双 OTA 槽必须 USB 刷一次；之后 OTA 不改分区表
- PSRAM 已启用（8MB octal @80MHz）；截图/封面缓冲在 PSRAM，LVGL draw buffer 仍在内部 RAM
- 字体文件较大（全量 16px 约 6MB 源码），app 约 2.53MB，每个 OTA 槽余量约 40%

### 7.3 中文字库

- 静态 UI：`font_zh16/24/48`（Noto Sans SC 子集，含 FontAwesome 符号）
- 动态标题：`font_zhfull`（GB2312 全部 7445 字 + 全角标点）
- 字库由 `lv_font_conv` 生成；如需增删字符，重新生成后替换 `main/fonts/`

### 7.4 触摸/屏幕适配

- 屏幕横屏使用：`esp_lcd_panel_swap_xy(true)` + `mirror(true, false)`
- 默认触摸映射：`x = raw_y`，`y = 239 - raw_x`
- 长按设置页「关于」进入 4 点校准；仿射系数存 NVS，重启保留
- 换板/换屏需要重新确认映射和背光 PWM

### 7.5 版本号

- 根目录 `version.txt` 是唯一版本源；CMake 在 `project()` 前注入 `PROJECT_VER`，并把它加入 `CMAKE_CONFIGURE_DEPENDS`，改版本会自动重新配置。
- OTA 版本比较只支持 `x.y.z` 数字段；发布前先改 `version.txt` 再 build。当前版本 `0.3.4`。

### 7.6 本地 API 与配网

- HTTP token 在 `Desktop → HTTP API token`；空则 API 503；只建议局域网。
- WiFi 优先 NVS；NVS 空用 Kconfig 种子；连接失败 / BOOT 长按 / `GET /portal` 进 SoftAP 门户。
- 配网 AP 开放，10 分钟超时；保存后重启连接。

---

## 8. OTA 更新

设备使用双 4MB OTA 槽（`ota_0` / `ota_1`）+ manifest + 回滚。OTA 只走局域网 HTTP，不做公网升级。

### 8.1 manifest 格式

```json
{
  "project": "nicemcu-desktop",
  "version": "0.3.4",
  "url": "http://<ota-host>:8000/nicemcu-desktop-0.3.4.bin"
}
```

- `project` 必须等于固件项目名（`nicemcu-desktop`），防止清单串项目；
- `version` 只支持 `x.y.z` 数字段，必须大于设备当前版本；
- `url` 是 app bin 的直链；HTTP 需要 `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y`（默认已开）。

### 8.2 发布新版本

```bash
# 1. 改版本号（唯一版本源）
echo "0.3.5" > version.txt

# 2. 构建
idf.py build

# 3. 把 bin 放到 OTA 静态目录，并更新 manifest
cp build/nicemcu-desktop.bin <ota-dir>/nicemcu-desktop-0.3.5.bin
# manifest 的 version/url 指向 0.3.5
```

- 改 `version.txt` 会自动触发 CMake 重新配置（`CMAKE_CONFIGURE_DEPENDS`）；
- 目标镜像写入后，设备会校验目标分区的 `project_name` 和 `version`，不一致会回退启动槽。

### 8.3 设备行为

- 联网成功 15s 后检查一次 manifest（`Desktop → OTA check delay` 可调）；
- 「设置 → 检查更新」可手动触发；下载中屏幕保持可用，完成后自动重启；
- 新镜像首次启动处于 `PENDING_VERIFY`；联网 15s 后先 `mark_valid` 再检查，避免被 `ESP_ERR_OTA_ROLLBACK_INVALID_STATE` 卡住；
- 下载进度通过 `/status` 的 `ota.pct` 和 OTA 弹窗显示；失败自动重试 3 次（5s 间隔）；
- 如果新镜像在 mark_valid 前崩溃/反复重启，bootloader 会回滚到上一槽；
- 关于页显示当前版本和槽位（`ota_0` / `ota_1`）。

### 8.4 首次迁移与限制

- 从旧的 `factory` 单槽布局迁移到双槽，必须 USB 刷一次（分区表不能 OTA 改）；
- 刷机前建议全片备份；NVS 偏移保持不变，WiFi/media 配置不受影响；
- OTA 目前是局域网 HTTP，无签名/加密；不要把 OTA 服务暴露到公网；
- 共享 manifest 会串项目：每个项目/板子用独立清单，清单带 `project` 字段。

### 8.5 实测

- `0.3.0` → `0.3.1` 首次切槽；`0.3.1` → `0.3.2` 进度弹窗；
- `0.3.3` → 坏 `0.3.4`（启动即重启）→ bootloader 自动回滚到 `0.3.3`；
- `0.3.3` → 好 `0.3.4`（2.5MB）约 30 秒，进度 1→100%，切到 `ota_1` 并 `mark_valid`；
- 版本比较有主机侧回归测试：`tests/test_ota_version.c`（`gcc -I main tests/test_ota_version.c`）。

---

## 9. 已知限制

- **媒体页只是遥控 + 状态**：设备不播音频/视频，声音在 PC；
- 设备不登录 B 站，完全依赖 media-control 的登录态；
- 40MHz SPI，整屏重绘约 30ms，大面积动画上限约 30fps；
- 动态标题限 GB2312，emoji/生僻字显示 `?`；
- 队列标题每条最多两行，超长省略；
- 触摸映射针对本板横屏校准；换板可能镜像/偏移；
- PSRAM 已启用；LVGL draw buffer 仍在内部 RAM，截图/封面在 PSRAM；
- 无 SD 卡音乐播放（硬件支持，固件未做）；
- OTA 只支持局域网 HTTP，无签名；分区表变更仍需 USB；
- coredump 分区已预留但未启用；
- 配网 AP 开放且 10 分钟超时；`/portal` 需要 token；
- 截图 BMP 约 230KB；`/ui` 只切页，不模拟触摸；
- 队列页 4 条/页，需要 media-control 3.6.1+；
- 触摸校准需手动点 4 个十字；默认映射已可用。

---

## 10. 目录结构

```
nicemcu-desktop/
├── CMakeLists.txt              # 注入 version.txt 到 PROJECT_VER
├── version.txt                 # 固件版本（OTA 比较源）
├── partitions.csv              # 16MB Flash / ota_0 4MB / ota_1 4MB / coredump 预留
├── sdkconfig.defaults          # 已脱敏：WiFi / media / OTA URL / HTTP token 留空
├── main/
│   ├── main.c                  # 显示/触摸/媒体/UI/校准
│   ├── ota.c / ota.h           # manifest/进度/重试/校验/回滚标记
│   ├── ota_version.h           # 版本比较（可主机测试）
│   ├── settings.c / .h         # NVS 设置 + 触摸仿射
│   ├── screen.c / .h           # 背光/息屏/PSRAM 影子缓冲
│   ├── diag.c / .h             # 日志环 + /status
│   ├── net_server.c / .h       # HTTP 8080 API
│   ├── wifi_mgr.c / .h         # STA/NVS/看门狗
│   ├── portal.c / .h           # SoftAP 配网门户
│   ├── app_ui.h                # /ui 页面请求
│   ├── Kconfig.projbuild       # WiFi / media / 天气 / OTA / token
│   └── fonts/                  # 生成的位图字体（Noto Sans SC + FA 符号）
├── tests/test_ota_version.c    # 版本比较回归测试
├── components/lvgl -> ...      # 本地 LVGL 9.6（gitignore，不入库）
└── README.md
```

---

## 11. 安全须知

- 仓库**不含** WiFi 密码、media-web 账号、内网 IP、OTA 清单地址；请通过 `menuconfig` 本地填写；
- 设备仅访问局域网 media-web，不直连 B 站、不保存 cookie；
- media-control 的 Basic 凭据会出现在设备 sdkconfig 中（本地文件），不要把 `sdkconfig` 提交/分享；
- 自己编译的 bin 也会带 WiFi/media/OTA 地址；不要公开发布带真实配置的固件；
- OTA 只走局域网 HTTP，无签名；不要把 OTA 静态服务或 media-web 暴露到公网；
- OTA manifest 的 `project` 字段用于防串项目；不要复用其他项目的清单；
- 本地 HTTP API 用 `CONFIG_DESKTOP_HTTP_TOKEN`；未配置时返回 503；只绑局域网，不做端口映射；
- 配网 AP 开放且 10 分钟超时；`/portal` 需要 token，不要在公共场所触发；
- `/screenshot.bmp` 会暴露屏幕内容；token 保护，别公开；
- 若要公网访问 media-web，请务必走 HTTPS + 强密码（详见 media-control 安全模型）；
- 本项目不提供、不托管任何视频内容，仅用于个人设备的状态显示与遥控。

---

## 12. Roadmap

- [x] 横屏 2×2 仪表盘 + 番茄钟 + 设置
- [x] media-control 薄客户端（封面/状态/控制/队列）
- [x] GB2312 全量字库 + 标点支持
- [x] OTA 双槽 + manifest + 进度/重试 + 回滚
- [x] WiFi 配网（NVS + SoftAP + `/portal`）+ 30s/10min 自愈
- [x] 队列分页（media-control 3.6.1+，4 条/页）
- [x] 设置持久化 + 4 点触摸校准
- [x] PSRAM 8MB + 截图通道（`/screenshot.bmp`）
- [ ] 音频：MIC 录音/回放、番茄钟提示音、白噪音
- [ ] WS2812 RGB 状态灯/夜灯
- [ ] 红外学习/发射
- [ ] 电池电量/低电量提醒
- [ ] I2C 传感器（AHT20/BH1750）
- [ ] IO21 PIR/DS18B20
- [ ] BLE 配网/HID
- [ ] UART 外部设备
- [ ] SD 卡本地音乐/白噪音
- [ ] 大肥鱼桌宠页（MIT 素材，JPEG 帧 + PSRAM）
- [ ] PSRAM 帧缓冲 / 动画性能
- [ ] OTA 签名 / HTTPS

---

## 13. 版本记录

| 版本 | 内容 |
|---|---|
| **v0.3.4** | HTTP/截图/PSRAM/配网/设置/校准/队列分页/OTA 进度+重试；坏镜像回滚实测 |
| v0.3.3 | OTA 下载节流修正（2.5MB 约 30s）；配网门户控制端口修复 |
| v0.3.0 | 首个 OTA 固件；USB 迁移分区表（ota_0/ota_1/coredump） |
| v0.2 | 仪表盘/天气/番茄钟/设置/媒体页/队列 |

---

## License

[MIT](LICENSE) © standbyme626

第三方字体/图标见 [NOTICE](NOTICE)：LVGL (MIT)、Noto Sans SC (OFL)、FontAwesome Free。
