# nicemcu-desktop

ESP32-S3 + 2.8" IPS 桌面仪表盘，LVGL 9.6，横屏 320×240。  
包含本地仪表盘（时钟/天气/设备状态/番茄钟）、设置弹层，以及作为
[media-control](https://github.com/standbyme626/media-control) 瘦客户端的媒体页
（封面/标题/进度/控制/队列）。

> 设备只负责显示和遥控；B 站播放仍在 PC 的受控浏览器里，声音也从 PC 出。

## 硬件

| 项 | 值 |
|---|---|
| 板子 | NiceMCU-S3-2.8IPS (XH-S3E, ESP32-S3-WROOM-1-N16R8) |
| SoC | ESP32-S3 双核 240MHz，16MB Flash，8MB PSRAM（当前未启用） |
| 屏幕 | ST7789P3 2.8" IPS 240×320，SPI 40MHz，横屏使用 |
| 触摸 | T6336G（CST816T/D 协议，I2C 0x15，SDA=17 SCL=18 INT=8） |
| 音频 | ZTS6672 I2S 麦克风 + NS4168 功放（本固件未用到） |
| 其它 | MicroSD（4-bit SDIO 38/40/39/41/48/47）、电池充电+ADC(IO1)、红外(IO13/14) |

### 引脚

| 功能 | GPIO |
|---|---|
| LCD SCK / MOSI / CS / DC / BL | 12 / 11 / 10 / 9 / 45 |
| 触摸 SDA / SCL | 17 / 18 |
| BOOT 键 | 0 |

## 功能

- **仪表盘**：大时钟、日期、天气（open-meteo）、WiFi/IP/内存/运行时长、番茄钟
- **番茄钟**：点卡片开始/暂停，长按重置；25 分钟工作 / 5 分钟休息自动切换
- **设置弹层**：亮度、刷新天气、息屏、关于
- **媒体页**（需 media-control 服务端）：
  - 封面（服务端裁剪为 80×80 RGB565）、标题（最多 4 行）、播放状态
  - 进度拖动、上一首/播放暂停/下一首、静音、系统音量、定时暂停
  - 队列页：最多 60 条、每条两行标题、点按切歌、当前位置高亮
- **网络**：WiFi STA、SNTP（ntp.aliyun.com）、天气 15 分钟刷新
- **中文**：静态 UI 用 Noto Sans SC 子集；动态标题用 GB2312 全量（7445 字，含标点）

## 架构

```
ESP32 (this firmware) --HTTP Basic--> media-web :8097 --SSH tunnel--> winpc bridge :8765 --CDP--> 受控 Edge / B 站
```

设备只调用 media-control 的 HTTP API，不直接访问 B 站，不保存 cookie。

### 依赖的 media-control API

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/api/status` | 播放状态（title/pos/dur/volume/queue/bv/pc_online） |
| GET | `/api/cover?bv=...` | 80×80 RGB565 封面原始字节 |
| GET | `/api/queue/status` | 队列列表 |
| GET | `/api/queue/jump?i=n` | 切到队列第 n 条并打开 |
| POST | `/api/play` `/api/pause` `/api/next` `/api/prev` `/api/mute` | 播放控制 |
| POST | `/api/volume` (`level=0..100`)、`/api/seek` (`pos=秒`)、`/api/sleep` (`min=`) | 音量/进度/定时 |

> `/api/cover` 和 `/api/queue/jump` 是 media-control 3.6.0 新增；
> 设备使用 HTTP Basic；服务端对 Basic 认证的 POST 跳过 CSRF（浏览器 Cookie 流程仍强制 CSRF）。

## 构建

要求：**ESP-IDF v6.0**、**LVGL v9.6.0**。

```bash
# 1) 取 LVGL 9.6（不要提交到本仓库）
git clone --depth 1 --branch v9.6.0 https://github.com/lvgl/lvgl.git components/lvgl

# 2) 配置（WiFi / media-web 地址与 Basic 凭据）
idf.py set-target esp32s3
idf.py menuconfig   # Desktop -> WiFi SSID/password, media-web host/user/password

# 3) 编译烧录
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`sdkconfig`、`build/`、`components/lvgl/` 已在 `.gitignore`，不会提交凭据/依赖。

分区表 `partitions.csv`：4MB app + 16MB flash；不含 OTA。

### Kconfig

- `DESKTOP_WIFI_SSID` / `DESKTOP_WIFI_PASSWORD`
- `DESKTOP_MEDIA_HOST` / `DESKTOP_MEDIA_USER` / `DESKTOP_MEDIA_PASSWORD`
- LVGL：96KB 内存池、Montserrat 14/32、FreeType 关闭、无 PSRAM

## 操作

- 仪表盘：默认页；「媒体」「设置」在时钟卡右下
- 媒体页：点「队列」打开队列；点条目切歌；长按播放键无定义（定时用「定时」按钮）
- 息屏：设置 → 息屏；触摸唤醒

## 已知限制

- 媒体页是**遥控 + 状态显示**，不在设备上播音频/视频
- SPI 40MHz，整屏重绘约 30ms；大面积动画上限约 30fps
- CJK 动态标题限定 GB2312；emoji/生僻字显示为 `?`
- 触摸坐标按 `swap_xy + mirror_x` 横屏映射；如换板需重新校准
- 未启用 PSRAM；封面/字体均在内部 RAM/Flash

## License

MIT（见 `LICENSE`）；第三方字体/图标见 `NOTICE`。
