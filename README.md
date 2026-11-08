# PlaneRadar · ESP32-C3 圆屏航电+气象桌面

一个把「航班雷达・天气・降雨云图・时钟」做到一块 240×240 圆屏里的固件。插电即用，无触摸，只用板载一个 BOOT 键切页，所有数据联网自动更新。代码按功能拆成 11 个 `.inc` 分块，由 `src/main.cpp` 拼成单一编译单元；编译全部跑在 GitHub Actions 上，构建后自动把 `firmware/` 产物回推到仓库。

> 本项目由作者独立设计并实现：固件代码、界面布局、参数与实现方案均为本人自行编写，不依赖其他同类项目的源码。当前固件版本 `1.3.1-c3`。

## 一、硬件

| 器件 | 型号 | 说明 |
|---|---|---|
| 主控 | ESP32-C3（`esp32-c3-devkitm-1`） | 单核 RISC-V，2.4G WiFi，本次实测可用堆约 140~146KB |
| 屏幕 | GC9A01 240×240 圆屏 SPI | 无触摸，无背光控制脚，圆形可视区半径 120px |
| 按键 | 板载 BOOT 键 | GPIO9，`INPUT_PULLUP`，低电平有效 |

接线（固件里写死，要改请改 `platformio.ini` 的 `build_flags`）：

| 屏幕引脚 | ESP32-C3 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| RST | GPIO0 |
| CS | GPIO1 |
| DC | GPIO10 |
| SDA / MOSI | GPIO3 |
| SCL / SCLK | GPIO4 |

> 不要用 GPIO0 复用做按键：它是屏幕复位脚，实测会与 RST 电平互相干扰。

## 二、五个页面

短按 BOOT：循环切页。长按 750ms：循环切换雷达量程（10 / 25 / 50 / 100 / 200 / 300 / 400 km）。

1. **时钟页**：黑底圆表盘、1~12 刻度、大黄数字时间、绿色日期、左红城市名 / 右青星期，两行中文天气 + 当前温度；四角内置日出、日落、月相（本地天文算法，不耗网络）；模拟指针带秒针。
2. **航班雷达页**：粗红外环 + 绿色十字与 3 圈量程、右侧绿色量程数字、淡绿扫描扇形；飞机按航向画黄/红三角，标白呼号、黄机型、青距离；中心白点。
3. **天气页**：顶部天气图标与中文天气、绿色当前温度 / 黄色今日最高温；三列「明天 / 后天 / 大后天」图标 + 最低/最高温 + 中文天气；底部城市名、当前温度 / 雨况 / 湿度、蒲福风力等级、红色预警条（无预警时显示 `UPD HH:MM`）。
4. **降雨云图页**：整圆底图（中文地名、海洋、道路）+ 雷达回波叠加，底部 `OK R x%` 覆盖率与帧号；3 帧 500ms 循环播放。
5. **自检 / 关于页**：版本号、MAC、IP、RSSI、剩余堆与最大连续块、连续运行时长、上次刷新耗时。

## 三、数据来源（全部为公开接口，无需 Key）

| 用途 | 接口 | 缓存周期 |
|---|---|---|
| 飞机 | `https://opendata.adsb.fi/api/v3/lat/<lat>/lon/<lon>/dist/<nm>`（半径 250nm，TLS `setInsecure`） | 60s |
| 天气实况 / 预报 | `http://api.open-meteo.com/v1/forecast`（`current` 含温湿度、天气代码、风速风向阵风、降水；`daily` 含 4 天天气代码、最高最低温、降雨概率、降水量、阵风最大值） | 6min |
| 降雨雷达 | 元数据 `http://api.rainviewer.com/public/weather-maps.json`，瓦片 `http://tilecache.rainviewer.com{path}/256/7/{x}/{y}/2/1_1.png`（RGBA，强度在 alpha） | 6min |
| 底图 | `http://webrd01.is.autonavi.com/appmaptile?...&x={x}&y={y}&z=7` | 首次拉取后存 SPIFFS |
| 可选定位 | `http://ip-api.com/json/`（配网里选「固定位置」时不调用） | — |

## 四、配网与 OTA

- 首次上电无 WiFi 配置时，自建热点 `PlaneRadar-Setup`，手机连上后打开 `http://192.168.4.1` 填写。
- 已联网后，同一局域网访问设备 IP（例如 `http://192.168.31.94`）即可看到配网页。
- 配网页字段：`ssid`、`pass`、`city`（中文地名）、`lat`、`lon`、`manual`（固定位置，跳过 IP 定位）、`rsrc`（雨图源，0 = 雷达回波，1 = 红外卫星云图）。
- 同一页面下方是 OTA：选一个 `app.bin` 上传，`POST /update` 完成后自动重启，NVS 配置和 SPIFFS 缓存都不受影响。

## 五、自己编译

`platformio.ini` 已锁版本，避免新版 core 破坏 API：

```ini
[env:esp32c3]
platform = espressif32@6.12.0        ; Arduino core 2.0.17
board = esp32-c3-devkitm-1
framework = arduino
board_build.partitions = huge_app.csv
board_build.flash_mode = dio
board_build.merge_bin = true
lib_deps =
  bblanchon/ArduinoJson@7.1.0
  moononournation/GFX Library for Arduino@1.4.9
  bitbank2/PNGdec@1.0.1
  olikraus/U8g2@^2.35.0
```

本地：

```bash
pio run            # 编译
pio run -t upload  # 直接烧
```

也可完全不装工具链，直接用仓库里的 GitHub Actions：推一次代码就自动编译，并把 `firmware/app.bin`、`firmware/firmware.bin`、`firmware/build.log` 回推到 `firmware/` 目录，下载即可烧。构建只依赖仓库内的源码，没有任何外部覆盖或后处理步骤。

源码结构（单编译单元，`static` 全局量跨文件共享，不用 `extern`）：

```
src/main.cpp            按顺序 #include 下面 11 个分块
src/inc/inc01_defs.inc  引脚、颜色、常量、全局状态、NVS 配置
src/inc/inc01b_ver.inc  版本号覆盖（改版本只动这里）
src/inc/inc02_util.inc  文字/绘图工具、Web 配网、OTA、WiFi 连接
src/inc/inc03_meteo.inc 气象数据抓取、蒲福风级换算、预警判定
src/inc/inc04_icon.inc  天气图标（几何绘制，不用位图）
src/inc/inc05_plane.inc ADS-B 解析
src/inc/inc06_radar.inc 雷达盘绘制、飞机投影、扫描扇形
src/inc/inc07_clock.inc 表盘、指针、日出日落、月相
src/inc/inc08_forecast.inc 天气页排版
src/inc/inc09_rain.inc  雨图：底图/瓦片下载、SPIFFS 缓存、帧平移复用、动画
src/inc/inc10_core.inc  页面调度、按键、定时任务、自检页、setup/loop
include/ChinaMap.h      时钟页背景中国地图轮廓（离线生成的坐标点，PROGMEM）
tools/patch_png.py      PNGdec 解码探针（调试期用的构建钩子，当前未接入构建）
```

## 六、烧录

分区表 `huge_app.csv`：`nvs@0x9000(0x5000)`、`otadata@0xe000(0x2000)`、`app0@0x10000(0x300000)`、`spiffs@0x310000(0xe0000)`、`coredump@0x3f0000(0x10000)`。

只升级程序（保留配网参数与雨图缓存）：

```bash
python -m esptool --chip esp32c3 --port COM3 --baud 460800 \
  write-flash 0x10000 firmware/app.bin
python -m esptool --chip esp32c3 --port COM3 --baud 460800 \
  verify-flash 0x10000 firmware/app.bin
```

从零全量烧写（一次性写入合并镜像，偏移已对齐 bootloader / 分区表 / boot_app0 / app）：

```bash
python -m esptool --chip esp32c3 --port COM3 --baud 460800 erase-flash
python -m esptool --chip esp32c3 --port COM3 --baud 460800 \
  write-flash 0x0 firmware/firmware.bin
```

手写偏移版本：`0x0 bootloader.bin`、`0x8000 partitions.bin`、`0xe000 boot_app0.bin`、`0x10000 app.bin`。

## 七、实现上几个值得说明的点

- **中文显示**：正文用 `u8g2_font_unifont_t_chinese4`（16px 点阵），数字用 Helvetica Bold 子集。Helvetica 子集没有 `°` 字形，所以「度」是用 `fillCircle` 画的小圆点 + `C` 拼出来的，不是字体字符。
- **雨图缓存**：底图和 3 帧雨图都落在 SPIFFS（实测已用约 98KB / 836KB）。帧缓存的签名 = 帧路径列表 + 瓦片坐标列表，换城市自动失效，避免串图。重进页面时按瓦片坐标把旧帧 rename 到新槽位复用，只补拉真正缺的那一帧。串口实测：首次冷拉 16.6s（最新帧先上屏），复用后 0.9~1.0s。
- **内存**：PNG 解码缓冲按需分配，最小 16KB、上限 64KB，解码后立即释放；实测空闲堆 140~146KB、最大连续块 118~122KB。
- **刷新节奏**：天气与雨图 6 分钟、飞机 60s、时钟 1s；失败 30s 后重试。
- **预警不是官方信号**：公共气象接口不提供预警发布数据，页面上的红字预警是按本地阈值算出来的提示（雷电 / 强降雨 / 大风 / 大雾 / 高温 / 低温 / 降雨提醒），要官方预警得接气象部门的付费数据服务。
- **降雨页是雷达拼图**：用的是公开的雷达回波瓦片（10 分钟一帧），不是气象卫星云图。配网里 `rsrc=1` 可切红外卫星图源，但该源当前常返回 0 帧，切过去大概率拿不到画面。

## 八、版本记录

| 版本 | 内容 |
|---|---|
| 1.3.1-c3 | 天气页加蒲福风力等级（风向 + 级数）与阈值预警条；雨图帧平移复用（重进页面 0.9~1.0s）；构建产物回推修复 |
| 1.3.0-c3 | 自检/关于页、OTA 升级入口、配网页扩展（城市/经纬度/雨图源）、日出日落月相、雨图逐帧动画、航班页卡顿优化、帧清单 flash 缓存 |
| 1.2.9-c3 | 四页 UI 重做、中文字体与度数圆点、航班页去闪烁、雨图底图叠加 |

## 九、许可

MIT，见 [LICENSE](LICENSE)。欢迎复刻、改造成自己的桌面。
