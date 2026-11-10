# WirelessDAP P4 + C6

ESP32-P4＋ESP32-C6 无线调试器，包含 PCB 原始工程、Windows C++ 上位机和固件源码。整理日期为 2026-11-10，上位机基线为已交付的 2.2。

## 目录

| 路径 | 内容 |
| --- | --- |
| hardware/调试器.eprj2 | 嘉立创 EDA 专业版原始工程 |
| hardware/fabrication/ | PCB、Gerber、原理图 PDF、BOM、贴片坐标和装配说明 |
| native/src/ | Windows C++ 上位机、CMSIS-DAP、ST-LINK 兼容桥、SSH |
| native/tests/ | 软件与硬件测试程序 |
| native/build/payload/ | 构建所需 OpenOCD、SSH、Keil 插件资源 |
| firmware/main/ | ESP32-P4 应用源码 |
| firmware/components/ | CMSIS-DAP 源码 |
| firmware/esp_hosted/slave/ | ESP-Hosted 2.13.0 的 C6 从机项目 |
| firmware/main/c6_image.bin | 已交付固件配套的 C6 镜像 |
| third_party_sources/ | OpenOCD、libusb 对应源码及构建说明 |
| docs/ | 使用说明、接口协议、引脚表、验收记录与第三方许可 |

## 功能

USB、蓝牙 BLE、Wi-Fi 通信，STM32 SWD 烧录，CMSIS-DAP、Keil 与 ST-LINK Server 兼容桥，UART1/2、I²C、SPI、CAN、RS485 多接口，以及电脑端 SSH 内嵌终端。

## 上位机构建

安装 Visual Studio C++ 桌面开发工具与 Windows SDK。打开 x64 Native Tools Command Prompt，进入 native 目录，执行以下命令。

```bat
build.cmd release
release\WirelessDAP.exe --self-test
```

编译使用 C++20 和静态运行库。native/build/payload 是打包输入，构建时自动生成嵌入资源。测试程序包含本机硬件端口和路径参数，执行硬件测试前请核对目标板与备份目录。

## P4 固件构建

使用 ESP-IDF 6.1.0，目标 esp32p4，依赖版本由 firmware/dependencies.lock 固定。在 ESP-IDF 环境进入 firmware，执行以下命令。

```text
idf.py menuconfig
idf.py build
idf.py -p PORT flash monitor
```

在 WirelessDAP configuration 中设置 AP 密码和应用访问令牌，并同步 native/src/board_config.h 后重编译上位机。上传副本采用 CHANGE-ME 占位值。既有板上固件继续使用原配置。

C6 从机源码位于 firmware/esp_hosted/slave，构建配置见该目录 README 和 sdkconfig.ci.sdio。当前保留的 c6_image.bin 是既有交付镜像，C6 源码重编译与该镜像的逐字节一致性未验证。

## 已有验证

USB、BLE 的 H743 烧录、读回、原 Flash 恢复，以及六接口并发已有记录。UART1 使用实际双向通信，CAN 使用内部回环，I²C 使用空总线探测。Wi-Fi 实板及外部总线设备互通仍需验证。详细范围见 docs/验收记录.txt。

整理后的上位机源码已重新编译，通过 19 项离线自检。修改口令占位值后的副本未重新执行实板烧录。

## 第三方许可

第三方许可保留在 docs/licenses、native/third_party、firmware/components 和 firmware/esp_hosted。OpenOCD 对应源码位于 third_party_sources。项目自有代码及硬件的对外授权方式由仓库所有者决定。

SHA256SUMS.txt 记录此次整理后的文件内容校验值。
