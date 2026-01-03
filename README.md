# SEECAM v1.0

## Project Overview

SEECAM v1.0 is a prototype assistive technology project designed for visually impaired individuals.
The goal is to provide a comprehensive, wide-angle, and high-clarity visual aid using a headset (
HMD) setup. By combining the high-quality main camera of a smartphone with a high-speed external
global shutter camera, the system aims to present a merged, lag-free field of view (FOV) directly to
the user's eyes, optimizing visual perception and spatial awareness.

## Hardware Setup

### Devices

* **Smartphone:** Google Pixel 7 Pro (Root Access Enabled). Used for its high-resolution wide lens
  and processing power.
* **External Camera:** Arducam 100fps Global Shutter Color USB Camera (1MP OV9782).
* **Lens:** Low Distortion M12 Lens.
* **SKU:** B0385.


* **Connectivity:** USB 3.0 PD HUB (Type-C).

### Physical Configuration

* **Housing:** Custom cardboard enclosure.
* **Alignment:**
* **Top:** Pixel 7 Pro (Back Camera).
* **Bottom:** Arducam USB Camera.
* **Orientation:** Vertically aligned to simulate a cohesive field of view.
* **Tilt:** The Arducam is mounted with an approximate **45-degree downward tilt** to capture the
  immediate lower peripheral view (e.g., walking path), complementing the forward view of the Pixel
  7 Pro.

## Technology Stack & Requirements

* **OS:** Android 13+ (Root Required).
* **Languages:**
* C++ (C++17 Standard)
* Kotlin (Android SDK)


* **Libraries & APIs:**
* **OpenCV (Android SDK):** For image matrix manipulation and color conversion.
* **V4L2 (Video for Linux 2):** For direct, low-latency communication with the USB camera driver.
* **Android NDK (Camera2 API):** For high-performance access to the internal back camera.
* **JNI (Java Native Interface):** For communication between the Kotlin UI layer and the C++
  processing layer.

---

## Software Architecture

### 1. Main Application Logic (`MainActivity.kt`)

The entry point of the application handles the UI composition, permissions, and geometric
transformations required to align the two camera feeds visually.

* **View Composition:**
* Uses a `FrameLayout` with a circular `ViewOutlineProvider` to mask the camera feeds into a
  circular overlay (simulating a biological eye field or lens).
* Layers two `TextureView` components: one for the Back Camera (background) and one for the External
  Camera (foreground).


* **Matrix Transformations (`applyTransform`):**
* Calculates and applies Affine Transformations (Scale, Rotate, Translate) to the video feeds.
* **FOV Matching:** Applies specific zoom multipliers (`BACK_WIDTH_SCALE`, `EXT_WIDTH_SCALE`) to
  digitally align the optical scale of the wide-angle Arducam with the Pixel 7 Pro's main lens.


* **Lifecycle Management:**
* Manages USB device attachment broadcasts to trigger the native C++ connection logic.
* Executes Root shell commands (`su`, `chmod`) to grant direct access to `/dev/video*` nodes for
  V4L2.

### 2. Native Camera Handling (`cpp`)

#### Back Camera (`back_camera.cpp`)

Handles the Pixel 7 Pro's internal camera using the NDK Camera2 API to ensure maximum throughput and
minimum latency.

* **Latency Optimization:** Forces `TEMPLATE_PREVIEW` and disables software post-processing blocks (
  Stabilization, Noise Reduction) to achieve a raw feed.
* **FPS Locking:** Analyzes the camera characteristics to force a tight 60 FPS range (`[60, 60]`)
  where supported, minimizing frame jitter.
* **Buffer Geometry:** Explicitly sets the native window buffer to `WINDOW_FORMAT_RGBA_8888` to
  ensure color space compatibility with the overlay system.

#### External UVC Camera (`uvc_camera.cpp`)

Manages the Arducam via V4L2 system calls, bypassing the standard Android USB Camera stack for
direct control.

* **Direct Memory Access:** Uses `mmap` (Memory Map) to access video buffers directly from the
  kernel space, reducing memory copy overhead.
* **Manual Exposure (AE):** Implements a custom software-based Auto-Exposure
  algorithm (`autoExposureMaybeAdjust`) that calculates average luma and adjusts `V4L2_CID_GAIN`
  and `V4L2_CID_EXPOSURE_ABSOLUTE` on the hardware level.

---

## Medical Image Processing & Computer Vision

This project applies specific Image Processing techniques to merge two distinct optical sources into
a coherent visual stream. The processing pipeline is handled natively in C++ using OpenCV.

### 1. Color Space Conversion

Raw data from the sensors comes in compressed or packed formats.

* **Method:** `cv::cvtColor`
* **Process:**
* **UVC:** Converts `YUYV` (YUV422) or `MJPEG` streams directly into `RGBA` (Red-Green-Blue-Alpha).
* **Back:** Configured to output direct RGBA via hardware, removing the need for software
  conversion.


* **Objective:** To standardize the pixel format for the alpha blending stage.

### 2. Region of Interest (ROI) & Cropping

To correct the aspect ratio and focus on the relevant path data (the ground in front of the user).

* **Macro:** `UVC_CROP_HEIGHT_RATIO 0.30f`
* **Process:**
* The system crops only the top 30% of the external camera's sensor data.
* OpenCV `cv::Rect` is used to slice the `cv::Mat` matrix efficiently without deep copying data.

### 3. Alpha Gradient Blending (Soft Seam)

To prevent a "hard cut" line where the external camera image overlays the back camera image, a
linear alpha gradient is applied programmatically to the pixel data.

* **File:** `uvc_camera.cpp` -> `decLoop`
* **Technique:** Per-pixel Alpha Channel Manipulation.
* **Algorithm:**
  Iterates through the top `N` rows (defined as `fadeHeight = 50`) of the external camera frame. The
  Alpha (Transparency) channel of the RGBA pixel is modified based on its vertical position ().

* **Result:** The top row is 0% opaque (fully transparent), gradually becoming 100% opaque at row
  50. This allows the Back Camera's image to "shine through" the top edge of the External Camera's
  feed, creating a seamless visual merge.

### 4. Latency Reduction Pipeline

Image processing usually incurs latency. This project minimizes it by:

* **Disabling ISP Filters:** In `back_camera.cpp`, standard filters are explicitly turned off:
* `ACAMERA_NOISE_REDUCTION_MODE_FAST`
* `ACAMERA_EDGE_MODE_FAST`
* `ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_OFF`


* **Reasoning:** Post-processing algorithms like stabilization introduce frame buffering delays (
  60-120ms). Disabling them allows for "Zero-Shutter-Lag" behavior required for real-time walking
  assistance.

---

## Author

**Ünal Zeray**
BFH Medizininformatik
HS 25 / 26 Medical Image Processing
