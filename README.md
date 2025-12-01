# 基于 i.MX6ULL 与 STM32 的 AIoT 远程视频监控小车系统

## 项目简介

本项目是一个集成了嵌入式 Linux 上位机与 STM32 实时控制系统的完整 AIoT 项目。项目旨在实现远程视频监控与设备控制，涵盖了从硬件 PCB 设计到嵌入式 Linux 应用开发、RTOS 移植及网络通信的全栈技术。

系统主要分为两部分：

1. **上位机 (Host)**：基于 i.MX6ULL 运行 Linux，运行 LVGL 图形界面，集成了摄像头视频流采集与分发（本地显示/UDP传输/RTMP推流），下位机通信。实现了多进程协同工作与守护进程监控。
2. **下位机 (Client)**：基于 STM32F103运行 FreeRTOS，负责运动控制、传感器数据采集及 OLED 本地菜单交互，通过 ESP32 联网实现远程控制。

两者通过 MQTT 协议进行远程通信，实现控制指令下发与状态数据上报。

<!-- 建议在此处放置一张项目整体实物图（小车+上位机屏幕） -->

<img title="" src="images/lvgl.jpg" alt="项目整体架构图" width="839">

<img title="" src="images/car.jpg" alt="" width="841" data-align="inline">

## 系统架构与技术栈

### 嵌入式 Linux 上位机 (i.MX6ULL)

上位机软件采用多进程架构设计，各模块解耦，通过 IPC 机制协同工作，所有模块均使用 CMake 构建。

* **核心技术**：V4L2, LVGL, MQTT, 信号量、共享内存
* **开发环境**：Ubuntu, ffmpeg,mosquitto,nginx,CMake,交叉 gcc
* **功能模块**：
  * **camera**：通过 V4L2 驱动采集摄像头数据，写入共享内存。
  * **lvglTest**：基于 LVGL 库开发 GUI，支持多点触控。从共享内存中获取摄像头数据，通过下拉框选择视频流向（本地 LCD 显示 / UDP 传输 / RTMP 推流）。
  * **mqttClient**：处理网络通信，解析 JSON 格式指令并写入共享内存。
  * **monitorDaemon**：守护进程，利用 `fork` 和 `exec` 监控上述业务进程，异常退出时自动重启，保证系统稳定性。

### 下位机控制系统 (STM32)

下位机基于 FreeRTOS 实时操作系统，保证了任务调度的实时性。

* **核心技术**：FreeRTOS, HAL 库, I2C/UART 通信，编码器接口。
* **网络模块**：ESP32 (AT 指令模式)，负责 Wi-Fi 连接与 MQTT 封包透传。
* **交互界面**：0.96寸 OLED 屏幕 + 旋转编码器，实现了多级菜单系统，可脱离上位机独立配置参数。
* **传感器与控制**：
  * HC-SR04 超声波测距
  * 光电对射传感器测速
  * 直流电机驱动

### 硬件设计

项目包含自主设计的原理图与 PCB。

* **EDA 工具**：嘉立创EDA(专业版)
* **主要板载资源**：STM32 主控, 电源管理模块, 电机驱动模块, 传感器接口，2.4g通信模块等。

---

## 核心功能展示

### 视频流传输模式

上位机 GUI 支持动态切换三种视频数据链路，体现了对网络协议和多媒体流的处理能力：

1. **本地显示**：视频帧通过共享内存直接渲染至 LVGL 控件。
2. **UDP 局域网透传**：通过ffmpeg将视频帧点对点发送至 PC 端上位机软件。
3. **RTMP 远程推流**：调用 ffmpeg将视频推流至 Nginx 服务器，PC 端使用 VLC 拉流观看。

<!-- 在此处放置 i.MX6ULL 屏幕截图，展示 LVGL 界面和视频画面 -->

<img title="" src="images/camera.jpg" alt="LVGL界面与视频流" width="874">

### 小车控制与交互

* **OLED 多级菜单**：设计了通用的菜单框架，通过旋转编码器实现了参数设置、模式切换等功能。
* **运动控制**：实现了全向移动逻辑，支持上位机和本地菜单双重调速。

<!-- 在此处放置小车 OLED 菜单特写图或动图 -->

---

## 硬件设计文件

本项目的硬件电路（原理图及 PCB）均为自主设计与绘制。

### 原理图

<!-- 放置原理图截图 -->

<img src="images/SCH_Schematic1.png" title="" alt="原理图" width="858">

### PCB 布局

<!-- 放置 PCB 2D 或 3D 渲染图 -->

<img title="" src="images/PCB.png" alt="PCB渲染图" width="685">

---

### 代码结构（仓库目录）

```text
/ (repo root)
├─ imx6ull_camera/          # 摄像头采集与帧写入（共享内存）
├─ imx6ull_lvgl/            # LVGL 界面程序（多点触控 + 摄像头下拉选择）
├─ imx6ull_mqtt/            # MQTT 客户端（命令/状态交换）
├─ imx6ull_daemon/          # 守护进程（监控 LVGL 和 MQTT 等）
├─ stm32_car/               # STM32 端代码（FreeRTOS + HAL）
├─ images/                  #图片资源
└─ README.md
```

## 快速开始 | Quick Start

### 上位机编译 (i.MX6ULL)

1. 配置交叉编译环境 (Source SDK)。

2. 进入各模块目录进行编译：
   
   ```bash
   # 以 LVGL 模块为例
   cd imx6ull_lvgl/build/
   cmake ..
   make
   ```

3. 运行守护进程启动系统：
   
   ```bash
   ./imx6ull_daemon
   ```

### 下位机烧录 (STM32)

1. 使用 Keil/STM32CubeIDE 打开工程。
2. 配置为ST-link
3. 编译并下载至 STM32。
