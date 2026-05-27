# EdgeDetect-P4

Real-time on-device object detection on the **ESP32-P4**, with a polished LVGL
landscape GUI overlaid on a live MIPI-CSI camera feed.

> Status: **GUI + camera pipeline complete**. Custom detection model
> integration is the next milestone — see [Roadmap](#roadmap).

| Target SoC | Display              | Camera             | Framework |
| ---------- | -------------------- | ------------------ | --------- |
| ESP32-P4   | 480×800 ST7701 (MIPI-DSI) used as **800×480 landscape** | OV5647 (MIPI-CSI, 800×1280 RAW8 @ 50 fps) | ESP-IDF v5.4+, LVGL 9.4 |

## What it does

- Brings up the OV5647 camera through ESP-Video / V4L2 + ISP.
- Builds an LVGL GUI in landscape 800×480 (mirrors `python/gui.py`):
  header (title + device tag), a 320×320 camera frame on the **left**, a
  white info card on the **right** showing the current **Class** and
  **Probability**, and a footer signature.
- Pipes each camera frame through the **PPA** (Pixel Processing Accelerator)
  for center-crop + scale + rotation, and blits it straight into the LCD
  framebuffer — bypassing LVGL for the camera path so it can run at full
  sensor rate without fighting the GUI refresh.
- Once the GUI is rendered, the framebuffer is snapshotted and LVGL is
  switched to `dummy_draw` mode, so the static GUI background lives in all
  three LCD framebuffers and only the 320×320 camera region updates per
  frame.

## Repository layout

```
EdgeDetect-P4/
├── main/
│   ├── main.c              # LVGL GUI build, app startup
│   ├── camera_display.{c,h}# PPA path: V4L2 frame → LCD framebuffer
│   ├── app_video.{c,h}     # OV5647 / V4L2 / ISP plumbing
│   └── idf_component.yml
├── components/
│   └── bsp_p4_touch_lcd/   # Board support for the 480×800 ST7701 panel
├── python/
│   └── gui.py              # PyQt5 mock-up the firmware GUI was ported from
├── CMakeLists.txt
├── partitions.csv
└── sdkconfig.defaults
```

## Build & flash

Requires **ESP-IDF release/v5.4** or newer and the ESP32-P4 toolchain.

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor   # adjust port for your machine
```

## Architecture at a glance

```
┌───────────────────────┐                   ┌────────────────────────┐
│ Startup (app_main)    │                   │ Per camera frame       │
│                       │                   │ (camera_display_       │
│  bsp_display_start_   │                   │  frame_cb)             │
│  with_config()        │                   │                        │
│  ROTATE_90  → 800×480 │                   │  V4L2 hands a buffer   │
│  logical surface      │                   │     │                  │
│                       │                   │     ▼                  │
│  build_gui()  ────────┐                   │  PPA: center-crop +    │
│  LVGL refreshes       │                   │  scale + rotate        │
│  the GUI into fb[0]   │                   │     │                  │
│           ▼           │                   │     ▼                  │
│  camera_display_init  │                   │  Write 320×320 block   │
│  → memcpy fb[0]→1,2   │                   │  into lcd_buffer[i] at │
│  → dummy_draw=true    │                   │  the GUI's left frame  │
│                       │                   │     │                  │
└───────────────────────┘                   │     ▼                  │
                                            │  esp_lv_adapter_       │
                                            │  dummy_draw_blit()     │
                                            │  pushes fb to LCD      │
                                            └────────────────────────┘
```

Key design choices:

- **LVGL `ROTATE_90` software rotation** so widgets are positioned in 800×480
  landscape coords (same coordinate system as `python/gui.py`); the LVGL
  adapter handles rotation into the native 480×800 framebuffer.
- **GUI rendered once, then frozen** via `dummy_draw`. Avoids LVGL contending
  with the camera path for the same framebuffers.
- **PPA does scale + rotation + format conversion in one transaction**
  (no CPU touch on pixel data).
- `CAM_PHYS_X / CAM_PHYS_Y` are *derived* from the logical position via the
  rotation transform, so changing the GUI layout doesn't desync the camera
  placement.

## Customizing

- **Where the camera sits in the GUI** — edit `CAM_LOGICAL_X / CAM_LOGICAL_Y`
  in `main/camera_display.h`. Physical offsets recompute automatically.
- **Camera orientation** — `rotation_angle` (and `mirror_x` / `mirror_y`) in
  `camera_display_frame_cb`. PPA angles count counter-clockwise.
- **Sensor / resolution** — `idf.py menuconfig` → *Espressif Camera Sensors*.

## Roadmap

- [x] BSP, camera, GUI, real-time blit path
- [ ] Integrate custom detection model (ESP-DL or hand-rolled)
- [ ] Wire inference output into the on-screen `Class` / `Probability`
      labels (needs a dynamic-text path that survives the `dummy_draw`
      snapshot — see `docs/` once added)
- [ ] Bounding-box overlay on the live camera region
- [ ] Save / replay clips to SD card

## Acknowledgements

- Espressif: `esp-idf`, `esp-video`, `esp_lcd`, `esp_lvgl_adapter`, PPA driver.
- LVGL contributors.

## License

TBD — pick one before publishing (MIT / Apache-2.0 are the usual choices for
embedded demos).
