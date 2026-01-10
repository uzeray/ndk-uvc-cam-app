CamCpp

CamCpp is a dual-camera preview pipeline intended for vertical FOV extension (stacking two views along the Y axis) and for reducing visible artifacts so the combined output can be used in VR / electronic vision-assist glasses. The app renders:

Back (phone) camera → backTv (top half, 2160×800 area)

External UVC camera (/dev/video)* → extTv (bottom half, 2160×800 area)

It does this using a Kotlin UI layer (TextureView + Matrix transforms) and two native pipelines:

Camera2 NDK + AImageReader + OpenCV for the phone back camera

V4L2 (UVC) + OpenCV for the external camera

Below is what each part does, which libraries are used, which formats flow through the pipeline, and which image-processing operations are applied (and why).

1) High-level architecture and libraries
Android / Kotlin side (UI & lifecycle)

Key classes

MainActivity: builds the layout, requests permission, owns lifecycle

BackAction: manages SurfaceTexture for back camera preview and applies a transform matrix

UvcAction: manages SurfaceTexture for UVC preview, prepares root access to /dev/video*, applies a transform matrix

Android APIs used

TextureView, SurfaceTexture, Surface, Matrix

CameraManager / CameraCharacteristics (only to read SENSOR_ORIENTATION and pick a back camera ID for orientation logic on the Java side)

BroadcastReceiver for USB attach/detach events

ActivityResultContracts.RequestPermission for CAMERA permission

Native side (NDK) libraries

Back camera pipeline

camera2ndk: ACameraManager, ACameraDevice, ACameraCaptureSession, ACaptureRequest

mediandk: AImageReader (YUV frames callback)

android/native_window: ANativeWindow_lock/unlockAndPost

OpenCV: core, imgproc (color conversion, rotation, blur)

UVC pipeline

Linux V4L2: ioctl, VIDIOC_*, mmap, poll, /dev/video*

OpenCV: imgproc, imgcodecs (YUYV→RGBA conversion or MJPEG decode)

Build / link

CMakeLists.txt links: camera2ndk, mediandk, log, android, dl, jnigraphics, OpenCV libs.

2) Manifest and permissions
Manifest

Uses android.permission.CAMERA

Declares android.hardware.camera.any required

Declares USB host feature optional (android.hardware.usb.host, required=false)

Activity locked to screenOrientation="landscape"

Permission gating behavior

Both BackAction.maybeStartBack() and UvcAction.maybeStartExt() are guarded by hasPermissionProvider().

That means UVC start is currently coupled to the CAMERA permission even though V4L2 access itself doesn’t need it.

3) UI layout and how frames are placed on screen
Fixed “panel” and two stacked views

In MainActivity you build a panel:

PANEL_W_PX = 2160, PANEL_H_PX = 1600

Positioned at (leftMargin=250, topMargin=15)

Contains a vertical LinearLayout named stack

stack contains two TextureViews:

backTv: 2160×800

extTv: 2160×800

So the intended combined output is top camera + bottom camera for a vertical FOV expansion.

Panel scaling to fit the real device screen

scalePanelToFitIfNeeded() scales the panel down (never up) if the device screen can’t fit 2160×1600 at that position. It sets:

pivotX/Y = 0

panel.scaleX = panel.scaleY = min(1, sx, sy)

Small Y translations

backTv.translationY = +14

extTv.translationY = -30

This is a crude seam/overlap alignment approach: shifting each preview slightly toward each other to reduce the visible boundary.

4) Back camera pipeline (Camera2 NDK + ImageReader + OpenCV)
Camera selection

Native code picks the “widest” back camera by:

enumerating back-facing cameras (ACAMERA_LENS_FACING_BACK)

reading ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS

choosing the smallest focal length (widest FOV)

Capture path and format flow

AImageReader_new(1280, 720, AIMAGE_FORMAT_YUV_420_888, 4, ...)

AImageReader_ImageListener.onImageAvailable pulls frames via AImageReader_acquireNextImage

The planes are copied into a contiguous buffer in NV21 layout (Y + interleaved VU) manually:

Plane0 → Y

Plane2 + Plane1 → interleaved VU (NV21)

Decode thread converts NV21 → RGBA using OpenCV:

cv::cvtColor(yuv, rgbaReuse, cv::COLOR_YUV2RGBA_NV21)

So the back camera’s processing format is:

Input: YUV_420_888 (Android camera)

Internal: repacked as NV21

Output: RGBA8888

Rotation strategy

In native decode:

cv::rotate(rgbaReuse, rgbaReuse, cv::ROTATE_90_CLOCKWISE);

So you rotate the back camera frame 90° clockwise in native before rendering.

Back camera image processing operations

Only one is actively applied in the current code:

(A) Bottom seam blur

applyBottomSeamBlur(rgbaReuse)

It takes the last BACK_SEAM_PX rows (default 12) and applies:

cv::GaussianBlur(bottomRoi, bottomRoi, Size(0,0), 2.0, 2.0)

Purpose: soften the bottom boundary of the top image so the seam is less harsh when stacked against the UVC image.

There are also helper functions for alpha feathering (applyTopSeamFeather) and sharpening (unsharpRect) in back_camera.cpp, but in the shown code they are not used in the active pipeline.

Rendering to Android Surface (why RGBA8888)

Rendering uses:

ANativeWindow_lock() → gets an ANativeWindow_Buffer

memcpy line-by-line into the window buffer

ANativeWindow_unlockAndPost()

The window buffers are configured as:

ANativeWindow_setBuffersGeometry(..., AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM)

Why this format:

Simple CPU copy to a known 4-byte pixel layout

Matches OpenCV’s RGBA output

Supports alpha (useful for seam feathering / blending strategies)

5) UVC pipeline (V4L2 + OpenCV)
Root access strategy (why SU is used)

UvcAction.prepareUvcAccess() runs:

su -c id (checks root)

su -c setenforce 0 (disables SELinux enforcing)

su -c chmod 666 /dev/video* /dev/v4l-subdev*

Purpose: allow direct V4L2 access to /dev/video* nodes without going through Android’s USB permission UI / framework camera stack.

Device discovery

Native enumerates /dev/video0..63:

checks capture capability: V4L2_CAP_VIDEO_CAPTURE + V4L2_CAP_STREAMING

prefers driver name starting with "uvcvideo" (true UVC)

otherwise keeps first capture node as fallback

Format/mode negotiation (what it tries to select)

The candidate builder tries two pixel formats:

V4L2_PIX_FMT_YUYV (uncompressed YUV 4:2:2)

V4L2_PIX_FMT_MJPEG (compressed JPEG stream)

It enumerates:

frame sizes via VIDIOC_ENUM_FRAMESIZES

frame intervals via VIDIOC_ENUM_FRAMEINTERVALS to estimate max FPS per mode

Selection priority (as coded):

Prefer YUYV over MJPEG

Prefer modes that meet desired FPS

Prefer higher pixel count (resolution)

Prefer higher max FPS

Streaming and buffers

Requests MMAP buffers via VIDIOC_REQBUFS (count=8)

Maps them via mmap

Queues them with VIDIOC_QBUF

Starts with VIDIOC_STREAMON

UVC image processing operations
A) Custom auto-exposure loop (only when using YUYV)

When using YUYV, the code switches camera exposure to manual and then performs a software AE:

Compute sampled average luma (fast subsampling):

avgLumaYuyvSample() reads Y from packed YUYV

Every UVC_AE_ADJUST_INTERVAL_MS (60 ms):

Compare avgLuma to target UVC_AE_TARGET_LUMA = 126 with tolerance ±10

Adjust exposure and gain using V4L2 controls (if available):

V4L2_CID_EXPOSURE_ABSOLUTE

V4L2_CID_GAIN

Exposure is capped based on FPS:

cap ≈ 65% of frame duration, bounded to [100us..16000us] and device min/max

Purpose: stabilize brightness and reduce flicker/instability under changing lighting while keeping exposure short enough to maintain FPS and reduce motion blur.

When using MJPEG, the code keeps camera auto exposure and autogain enabled (no custom AE).

B) Seam feathering via alpha on the TOP rows

In applyUvcSeamAndEdgeProcessing():

Set entire frame alpha to 255

For the top UVC_SEAM_PX rows (default 12):

Apply GaussianBlur over the seam region

Apply a vertical alpha ramp from transparent→opaque (row 0 alpha near 0, last seam row near 255)

Purpose: make the top edge of the UVC frame blend more gracefully with whatever is above it (the back camera frame) when compositing.

Important detail: right now you are stacking two TextureViews, not compositing them into one surface, so this alpha feather only helps if:

the UI is later changed to overlay and alpha-blend, or

a blending step produces a combined bitmap.

C) Format decode and conversion

Depending on chosen UVC mode:

YUYV path

cv::cvtColor(yuyv, rgbaReuse, cv::COLOR_YUV2RGBA_YUY2)

ROI cropping applied (currently UVC_CROP_HEIGHT_RATIO = 1.00, so no crop)

seam alpha feather applied

render RGBA to window

MJPEG path

cv::imdecode(buf, IMREAD_COLOR) → BGR

cv::cvtColor(bgr, rgbaReuse, cv::COLOR_BGR2RGBA)

seam alpha feather applied

render RGBA to window

So UVC format flow is:

Input: YUYV (raw) OR MJPEG (compressed)

Internal: decoded to RGBA8888

Output: rendered to RGBA window buffers

6) TextureView transforms (how the previews are scaled/cropped)
BackAction transform logic

BackAction.applyTransform() builds a Matrix and sets it via TextureView.setTransform(m).

Intended responsibilities:

account for camera buffer size (1280×720)

account for display rotation vs sensor orientation (backRotDeg)

scale to fit/fill the target view (2160×800)

optionally zoom (BACK_ZOOM_MUL)

optionally mirror (parameter exists, but not applied)

What actually happens in the shown code:

Rotation detection and display rotation mapping are effectively broken:

getDisplayRotationDeg() returns 0 for every case (all branches are ROTATION_0)

rot90 is computed as (rot == 0f || rot == 0f) which is always true and never checks 90/270

Non-uniform scaling is hardcoded:

sx = scale * 2

sy = scale / 2

Practical impact: the back preview is stretched/cropped in a way that is not purely aspect-ratio fit/fill. This can easily produce the “small central image with black borders” or unnatural scaling you see in your photo, depending on the effective buffer geometry and rotation.

UvcAction transform logic

applyExtTransform() does custom scaling/anchoring:

Uses extBufW/extBufH (updated from native chosen mode string)

Computes:

scaleX = (vw/srcW) * kx with kx≈0.495 minus a right trim

scaleY = (vh/srcH) * ky with ky=1.80

Computes translation tx/ty to anchor left and shift inward

Purpose: manual calibration to:

remove “strip” artifacts

force the image to fill the 2160×800 space in a controlled way

align optical center / seam visually for your headset setup

Tradeoff: these constants are device- and camera-specific. If the chosen UVC mode changes (e.g., 1280×720 vs 1920×1080) the same constants can produce very different framing.

7) Blending / stitching between the two cameras
What exists in code

In native-lib.cpp you have:

nativeBlendSeam(backStrip, extStrip, outBand, overlapPx)

It:

Builds an output band with height 2*overlapPx

For each row in that band, linearly blends:

back strip row (near bottom of back image)

ext strip row (near top of UVC image)
using cv::addWeighted

Applies a final GaussianBlur to the band to hide hard transitions

Purpose: produce a smooth stitched seam between the two vertical views.

What is missing in the shown Kotlin

In the provided Kotlin code, there is no call that:

extracts seam strips from the two TextureViews into Bitmaps, and/or

draws the blended band over the seam region, and/or

renders both images into a single composited surface.

So at the moment, the “stitching” is primarily:

visual alignment by shifting translationY

seam blur on the back frame bottom

seam alpha feather on the UVC frame top (only useful if compositing with alpha)

8) Why the output in your photo looks like it does (based on the code)

From the photo, the live content appears in a smaller central region with black margins. With the current code, the most likely reasons are:

The TextureView transform matrices apply strong non-uniform scaling and offsets (sx=scale*2, sy=scale/2, plus UVC kx/ky constants), which can shrink or crop the visible region unexpectedly.

The back pipeline rotates in native and then the Java side may also rotate depending on backRotDeg (computed from sensor orientation). If the assumptions mismatch, the effective width/height used for scaling can become wrong.

The UVC transform deliberately uses kx≈0.495, meaning “use about half the nominal width,” which inherently produces letterboxing unless the translation and scale are tuned to your exact view size.

9) Summary of all image-processing steps (active vs present)
Back camera (active)

YUV_420_888 → NV21 repack

NV21 → RGBA (cv::COLOR_YUV2RGBA_NV21)

Rotate 90° clockwise

Bottom seam Gaussian blur (last ~12 rows)

Back camera (present in code but not currently used)

alpha feather utilities (applyTopSeamFeather)

unsharp masking (unsharpRect)

UVC camera (active)

Mode selection (YUYV preferred, MJPEG fallback)

Exposure/brightness controls setup

If YUYV:

YUYV avg-luma sampling

custom AE loop (adjust exposure/gain toward target luma)

YUYV → RGBA (cv::COLOR_YUV2RGBA_YUY2)

If MJPEG:

JPEG decode (cv::imdecode)

BGR → RGBA

Top seam alpha feather + seam Gaussian blur (first ~12 rows)

Stitch/blend (implemented in native but not wired in Kotlin)

Overlap band blending (cv::addWeighted across a 2×overlap band)

Final Gaussian blur over the band
