# EdgeDetect-P4

在 **ESP32-P4** 上实现 MIPI 摄像头采集与显示。

> **状态**：已实现摄像头采集与 GUI 显示。
> **规划**：集成检测模型（见 [路线图](#路线图)）。

| 硬件/环境 | 规格 |
| :--- | :--- |
| **SoC** | ESP32-P4 |
| **显示** | 800×480 (MIPI-DSI) |
| **摄像头** | OV5647 (MIPI-CSI) |
| **IDF 版本** | v5.4 |
| **GUI 库** | LVGL 9.4 |

## 功能描述

- **采集驱动**：使用 ESP-Video (V4L2 + ISP) 驱动摄像头。
- **图像处理**：利用 **PPA** 硬件完成图像裁剪、缩放和旋转。
- **显示逻辑**：视频流直接写入 LCD 帧缓冲，不经过 LVGL 渲染层。
- **运行策略**：GUI 渲染一次后停止刷新，仅由硬件 PPA 更新视频区域。

## 目录结构

```text
EdgeDetect-P4/
├── main/
│   ├── main.c              # 程序入口与 UI 静态构建
│   ├── camera_display.c    # PPA 处理与帧缓冲写入
│   ├── app_video.c         # V4L2 与 ISP 初始化
├── components/
│   └── bsp_p4_touch_lcd/   # 板级显示驱动
├── python/
│   └── gui.py              # UI 布局参考
```

## 编译与烧录

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## 核心原理

1.  **UI 初始化**：使用 LVGL 绘制静态界面元素。
2.  **FB 冻结**：将 GUI 图像同步至 LCD 的所有帧缓冲并关闭 LVGL 刷新。
3.  **硬件旁路**：摄像头产生图像后，PPA 硬件直接处理并将像素块搬运至 LCD 帧缓冲的指定位置。

## 配置项

- **位置参数**：在 `main/camera_display.h` 中修改 `CAM_LOGICAL_X/Y`。
- **图像方向**：在 `camera_display_frame_cb` 中修改 `rotation_angle`。
- **底层参数**：使用 `idf.py menuconfig` 修改摄像头驱动设置。

## 路线图

- [x] 硬件驱动与视频通路
- [ ] 集成物体检测模型
- [ ] 结果数据展示
- [ ] 绘制检测框
- [ ] SD 卡存储

## 引用

- ESP-IDF / ESP-Video / PPA Driver
- LVGL
