# Xilinx Virtual Cable (XVC) Server for ESP32

[![License: CC0 1.0](https://img.shields.io/badge/License-CC0_1.0-lightgrey)](LICENSE)

在 ESP32-S3 上实现 **Xilinx Virtual Cable (XVC)** 协议，通过 WiFi 或 USB 连接 Vivado 对 Xilinx FPGA 进行 JTAG 编程与调试。

上游仓库: [ciniml/xvc-esp32](https://github.com/ciniml/xvc-esp32)

---

## 概述

将 ESP32-S3 的 GPIO 连接到目标 FPGA 的 JTAG 引脚（TDI、TDO、TMS、TCK），即可通过 Vivado Hardware Manager 中的 **Add Virtual Cable** 功能，像使用普通 JTAG 适配器一样访问 FPGA。

本项目提供两种运行模式，**运行时自动检测，无需编译切换**：

| 模式 | 描述 | XVC 协议运行位置 |
|------|------|-----------------|
| **WiFi TCP（默认）** | ESP32 运行 XVC 服务器，Vivado 通过 TCP 连接 | ESP32 |
| **USB-JTAG 桥接** | PC 端运行 XVC 代理，ESP32 作为 UART 到 JTAG 的透传桥 | PC（`xvc-proxy.py` / `xvc-proxy.exe`） |

---

## 硬件连接

### ESP32-S3 引脚分配（默认）

| GPIO | JTAG 信号 |
|------|-----------|
| 8 | TDI |
| 9 | TDO |
| 10 | TCK |
| 11 | TMS |

> **仅支持 GPIO 0~31**，`jtag_xfer` 使用 `uint32_t` 位操作。

### 接线示意

将 ESP32-S3 的四个 GPIO 分别连接到目标 FPGA 的对应 JTAG 引脚，并确保 GND 共地。JTAG 信号为 3.3V 逻辑电平。

---

## 快速开始

### 环境要求

- [ESP-IDF](https://github.com/espressif/esp-idf) 已安装，`IDF_PATH` 环境变量已设置
- Windows 用户运行 ESP-IDF 安装目录下的 `export.ps1` 来设置环境

### 构建与烧录

```bash
# 设置目标芯片（仅首次）
idf.py set-target esp32s3

# 配置 WiFi 等参数（首次）
idf.py menuconfig

# 构建
idf.py build

# 烧录（自动检测串口）
idf.py -p PORT flash

# 串口监视器
idf.py -p PORT monitor
```

也可使用项目自带的 `Makefile` 快捷命令：

```bash
make              # 构建
make upload       # 构建 + 烧录（自动检测串口）
make monitor      # 串口监视器
make flash        # 构建 + 烧录 + 监视器
make menuconfig   # 配置菜单
make set-target   # 设置芯片目标
make clean        # 完全清理
make format       # 代码格式化（astyle）
```

> **端口自动检测**：`board_detect.py` 匹配 USB VID `1a86`（CH340）或 `10c4`（CP210x）——**不检测** Espressif 原生 USB（VID `303a`）。烧录请使用 CH340/CP210x 串口。

### WiFi 凭证配置

复制示例文件并编辑：

```bash
cp main/credentials.h.example main/credentials.h
```

编辑 `main/credentials.h`，填入你的 WiFi SSID 和密码：

```cpp
static const char* MY_SSID = "你的WiFi名称";
static const char* MY_PASSPHRASE = "你的WiFi密码";
```

> `credentials.h` 已加入 `.gitignore`，不会被 Git 跟踪。首次构建前必须创建此文件。

---

## 模式一：WiFi TCP 模式（默认）

ESP32 上电后：
1. 尝试连接 WiFi（优先使用 NVS 中保存的凭证，若不存在则使用 `credentials.h` 中的凭证）
2. 启动 `XvcServer`，监听 TCP **端口 2542**
3. Vivado 连接 `ESP32_IP:2542` 即可使用

### Vivado 操作步骤

1. 打开 Vivado → **Hardware Manager**
2. 点击 **Open target** → **Open New Target**
3. 选择 **Add Virtual Cable**
4. 输入 ESP32 的 IP 地址，端口 `2542`
5. 连接成功后即可看到 FPGA 设备

---

## 模式二：USB-JTAG 桥接模式

### 工作原理

ESP32 启动时会等待 3 秒，检测 USB Serial/JTAG（原生 USB，VID `303a`）上是否收到 `'I'`（0x49）字节：
- 收到 → 进入桥接模式
- 未收到 → 进入 WiFi 模式，但继续通过 `usb_watchdog_task` 监视 USB，后续收到 `'I'` 会自动切换

桥接模式下，WiFi 关闭以省电，ESP32 作为透传桥，将 PC 发来的 JTAG 移位命令直接转换为 GPIO 操作。

### 使用步骤

```bash
# 1. 构建并烧录固件（与 WiFi 模式使用同一固件）
make flash

# 2. 将 ESP32 通过原生 USB 端口（VID=303a）连接至 PC

# 3. 在 PC 上运行代理（需要 Python 3 + pyserial）
pip install pyserial
python tools/xvc-proxy.py                        # 交互式选择端口
python tools/xvc-proxy.py --port COM3             # 直接指定端口
python tools/xvc-proxy.py --debug                 # 启用详细日志
python tools/xvc-proxy.py --diagnose              # USB 数据路径诊断（无需 Vivado）

# 4. 在 Vivado 中：Hardware Manager → Add Virtual Cable → localhost:2542
```

也可直接使用 Windows 可执行文件（无需 Python，已预装 `pyserial`）：

```bash
tools/xvc-proxy.exe --port COM3
tools/xvc-proxy.exe --debug
tools/xvc-proxy.exe --diagnose
```

> **提示**：如果首次编程失败，先运行 `--diagnose` 验证 USB 数据路径。诊断测试会清空 ESP32 引导加载程序残留的 USB 数据，一次性解决同步问题。

### USB 桥接二进制协议

```
PC → ESP32: 'I' (0x49)                    → 初始化 JTAG（ESP32 回复 0x06 ACK）
PC → ESP32: 'S' (0x53) + n_bits(u16 LE)   → 移位命令
            + TMS[n/8 字节] + TDI[n/8 字节]
ESP32 → PC: len_prefix(u16 LE) + TDO[n/8 字节]
```

---

## 运行时 WiFi 命令

通过 **CH340 串口**（`make monitor` 所使用的端口，115200 baud）发送以下命令。
**在 WiFi 模式和桥接模式下均可用。**

| 命令 | 作用 |
|------|------|
| `wifi set <SSID> <PASS>` | 保存 WiFi 凭证到 NVS 并重新连接 |
| `wifi show` | 显示当前 WiFi SSID 和 NVS 状态 |
| `wifi reset` | 擦除 NVS 中保存的凭证，回退到 `credentials.h` |
| `wifi delay <1-1000>` | 设置 JTAG 延迟值（持久化到 NVS，默认 20） |
| `wifi speed` | 打印基于当前延迟的近似 TCK 频率 |

### 凭证优先级

1. **NVS 存储的凭证**（通过 `wifi set` 设置）— 优先级最高
2. **`credentials.h` 中的硬编码凭证** — 回退默认值

> NVS 中的凭证在重新烧录后仍然保留，而 `credentials.h` 的修改需要重新编译。

---

## JTAG 时序调整

默认 JTAG 延迟值为 `20`（TCK 约 3.5 MHz），可通过以下方式调整：

- 运行时：`wifi delay <1-1000>`（实时生效，持久化到 NVS）
- 编译时：修改 `main/main.cpp` 中的 `static unsigned int jtag_delay = 20;`

延迟值与 TCK 频率对照：

| `jtag_delay` | 约 TCK | 适用场景 |
|:---:|:---:|---|
| 20 | 3.5 MHz | 短接线、同一 PCB（默认） |
| 50 | 2.0 MHz | 杜邦线、普通环境 |
| 100 | 1.0 MHz | 线缆较长或不确定稳定性 |
| 200 | 0.6 MHz | 线缆很长或干扰较大 |

对于较长的线缆或不稳定的环境，适当增大延迟值可以提高稳定性。降低延迟值可以获得更高的 TCK 频率。

---

## 静态 IP 配置

取消 `main/main.cpp` 中 `#define USE_STATIC_IP` 的注释，并在 `wifi_init_sta()` 函数中设置 IP、网关和子网掩码。

---

## UART 串口桥接

固件包含一个双向 UART 桥接任务，将 `UART_NUM_0`（CH340 USB UART，115200 baud）与 `UART_NUM_1`（RX=33, TX=23, 115200 baud）连接，在 `APP_CPU_NUM` 核心上运行。这提供了 FPGA UART 的透传功能。

该任务也会从 CH340 串口数据流中检测 `wifi` 命令并在**两种模式**下处理。响应会同时输出到 CH340 和原生 USB 端口。

---

## 项目文件说明

| 文件 | 说明 |
|------|------|
| `main/main.cpp` | 固件源码（~990 行，C++17，ESP-IDF） |
| `main/credentials.h.example` | WiFi 配置模板（重命名为 `credentials.h` 并编辑） |
| `main/CMakeLists.txt` | 组件构建配置 |
| `firmware/xvc-esp32.bin` | 预编译固件（ESP32-S3） |
| `tools/xvc-proxy.py` | USB 桥接模式 PC 端代理（Python） |
| `tools/xvc-proxy.exe` | USB 桥接模式 PC 端代理（Windows 可执行文件） |
| `tools/xvc-proxy.spec` | PyInstaller 打包配置 |
| `--diagnose` 参数 | 运行 USB 数据路径回环测试，验证数据完整性并清空残留字节 |
| `tools/test_xvc_server.py` | XVC 服务器测试脚本 |
| `tools/fix11.py` | 辅助工具脚本 |
| `board_detect.py` | 串口自动检测 |
| `CMakeLists.txt` | 顶层 ESP-IDF 构建配置 |
| `Makefile` | 构建快捷命令 |
| `formatter.conf` | Artistic Style 格式化配置 |
| `.clangd` | LSP 配置 |

---

## 构建配置

- **构建系统**: ESP-IDF（`idf.py`）
- **语言标准**: C++17
- **依赖**: 仅依赖标准 ESP-IDF 库，无额外第三方库
- **编译器**: esp-clang（LSP 配置指向 IDF 安装目录下的 `esp-clang`）
- **格式化工具**: Artistic Style（`astyle`），配置见 `formatter.conf`，2 空格缩进

### LSP 配置

`.clangd` 文件位于项目根目录。首次构建后，`build/` 目录会生成 `compile_commands.json`，LSP 才能正常工作。

```bash
idf.py build   # 先生成编译数据库
```

---

## 常见问题

### Vivado 报错 "End of startup status: LOW"

**桥接模式**：ESP32 ROM 引导加载程序向 USB Serial/JTAG 输出的残留字节可能干扰桥接协议。运行 `--diagnose` 可自动清空残留数据并恢复正常通信。也可重启 ESP32 后重试。

**WiFi 模式**：检查 FPGA 电源的电压和电流是否满足要求。

### 烧录时检测不到端口

使用 `board_detect.py` 仅匹配 CH340（`1a86`）和 CP210x（`10c4`）VID。如果使用 ESP32-S3 的原生 USB 端口（`303a`）烧录，请手动指定端口：

```bash
idf.py -p COM3 flash
```

### 桥接模式下代理无法连接

确保使用了原生 USB 端口（VID `303a`），而不是 CH340/CP210x 串口。代理的端口检测逻辑与烧录脚本不同，除了 `1a86`/`10c4` 外也匹配 `303a`。

### 时序不稳定 / 编程失败

通过串口执行 `wifi delay 200` 增加 JTAG 延迟。如果稳定，可通过 `wifi set <SSID> <PASS>` 重新设置 WiFi 重新连接自动生效。

---

## 相关项目

- [xvcpi](https://github.com/kholia/xvcpi) — Raspberry Pi 版 XVC
- [xvc-pico](https://github.com/kholia/xvc-pico) — Raspberry Pi Pico 版 XVC
- [xvc-esp8266](https://github.com/kholia/xvc-esp8266) — ESP8266 版 XVC
- [Colorlight-5A-75B](https://github.com/kholia/Colorlight-5A-75B)
- [xc3sprog](https://sourceforge.net/projects/xc3sprog/) — 配合 FT2232H 可获得更高编程速度

---

## License

This project is licensed under [CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/) — Public Domain Dedication.

原作者: Kenta IDA ([@ciniml](https://github.com/ciniml))

本作品是 Derek Mulcahy 的 Raspberry Pi XVC 实现（[xvcpi](https://github.com/derekmulcahy/xvcpi)）向 ESP32 的移植。
