// back_camera.cpp

#include "back_camera.h"
#include "../common/logging.h"

#include <android/native_window_jni.h>
#include <android/native_window.h>

// Camera2 NDK
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraMetadata.h>

// Media NDK (ImageReader için)
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

// OpenCV
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <cstring>
#include <vector>
#include <thread>
#include <condition_variable>
#include <algorithm>
#include <cmath>
#include <dlfcn.h>

// --- UVC'den kopyalanan ayarlar (Eşit görünüm için) ---
#define UVC_CROP_HEIGHT_RATIO 1.00f
#ifndef BACK_SEAM_PX
#define BACK_SEAM_PX 12
#endif
#ifndef BACK_EDGE_PX
#define BACK_EDGE_PX 24
#endif

// Processing parametreleri
static constexpr double UVC_SEAM_SIGMA_X = 2.0;
static constexpr double UVC_SEAM_SIGMA_Y = 0.8;
static constexpr double UVC_SHARP_SIGMA = 1.0;
static constexpr double UVC_SHARP_AMOUNT = 0.60;

namespace backcam {

    // --- Global Değişkenler ve Threading ---
    static std::mutex gLock;

    static ACameraManager *gMgr = nullptr;
    static ACameraDevice *gDevice = nullptr;
    static ACameraCaptureSession *gSession = nullptr;
    static ACaptureRequest *gPreviewRequest = nullptr;

    // AImageReader
    static AImageReader *gImgReader = nullptr;
    static ANativeWindow *gImgReaderWindow = nullptr;

    // Ekrana çizim yapılacak pencere (JNI'dan gelen Surface)
    static ANativeWindow *gJavaWindow = nullptr;

    static ACameraOutputTarget *gTarget = nullptr;
    static ACaptureSessionOutputContainer *gOutputs = nullptr;
    static ACaptureSessionOutput *gSessionOutput = nullptr;

    static std::atomic<bool> gRunning{false};
    static std::atomic<long long> gLastSensorTsNs{0};
    static std::atomic<int> gFpsX100{0};
    static std::atomic<int> gChosenFps{0};
    static std::string gChosenCamId;
    static int gSensorOrientationDeg = 0;
    static std::string gLastError;

    static std::mutex gFrameLock;
    static std::condition_variable gFrameCv;
    static std::vector<uint8_t> gFrameYuv;
    static std::atomic<bool> gFrameReady{false};
    static int gFrameW = 0;
    static int gFrameH = 0;

    static std::thread gThDec;

    // ÖNEMLİ: Eğer rotate yapacaksak buffer boyutlarını değiştirmek gerekebilir.
    // Şimdilik 1280x720 istiyoruz.
    static constexpr int kPreviewW = 1280;
    static constexpr int kPreviewH = 720;

    static void setLastErrorLocked(const std::string &msg) { gLastError = msg; }

    static void clearLastErrorLocked() { gLastError.clear(); }

    // --- OpenCV Processing Helperları ---

    static inline void setAlphaRect(cv::Mat &rgba, const cv::Rect &r, uint8_t a) {
        if (rgba.empty()) return;
        cv::Rect rr = r & cv::Rect(0, 0, rgba.cols, rgba.rows);
        if (rr.width <= 0 || rr.height <= 0) return;

        for (int y = rr.y; y < rr.y + rr.height; ++y) {
            uint8_t *row = rgba.ptr<uint8_t>(y);
            for (int x = rr.x; x < rr.x + rr.width; ++x) {
                row[x * 4 + 3] = a;
            }
        }
    }

    static inline void applyTopSeamFeather(cv::Mat &rgba, int seamPx) {
        if (rgba.empty()) return;
        seamPx = std::clamp(seamPx, 1, rgba.rows);

        setAlphaRect(rgba, cv::Rect(0, 0, rgba.cols, rgba.rows), 255);

        cv::Rect seamR(0, 0, rgba.cols, seamPx);
        cv::Mat seam = rgba(seamR);

        cv::GaussianBlur(seam, seam, cv::Size(0, 0), UVC_SEAM_SIGMA_X, UVC_SEAM_SIGMA_Y);

        for (int y = 0; y < seamPx; ++y) {
            uint8_t a = (seamPx == 1) ? 255 : (uint8_t) std::lround(
                    255.0 * (double) y / (double) (seamPx - 1));
            uint8_t *row = rgba.ptr<uint8_t>(y);
            for (int x = 0; x < rgba.cols; ++x) {
                row[x * 4 + 3] = std::min<uint8_t>(row[x * 4 + 3], a);
            }
        }

        if (seamPx < rgba.rows) {
            setAlphaRect(rgba, cv::Rect(0, seamPx, rgba.cols, rgba.rows - seamPx), 255);
        }
    }

    static inline void unsharpRect(cv::Mat &rgba, const cv::Rect &r) {
        if (rgba.empty()) return;
        cv::Rect rr = r & cv::Rect(0, 0, rgba.cols, rgba.rows);
        if (rr.width <= 0 || rr.height <= 0) return;

        cv::Mat roi = rgba(rr);
        std::vector<cv::Mat> ch;
        cv::split(roi, ch);
        if (ch.size() != 4) return;

        for (int i = 0; i < 3; ++i) {
            cv::Mat blurred;
            cv::GaussianBlur(ch[i], blurred, cv::Size(0, 0), UVC_SHARP_SIGMA);
            cv::addWeighted(ch[i], 1.0 + UVC_SHARP_AMOUNT, blurred, -UVC_SHARP_AMOUNT, 0.0, ch[i]);
        }
        cv::merge(ch, roi);
    }

    static inline void applyBottomSeamBlur(cv::Mat &rgba) {
        if (rgba.empty()) return;

        // Alt 10px'i hesapla
        int seamH = std::min((int) BACK_SEAM_PX, rgba.rows);
        if (seamH <= 0) return;

        // ROI: Resmin en altındaki dikdörtgen
        cv::Rect bottomRect(0, rgba.rows - seamH, rgba.cols, seamH);

        // Sadece bu bölgeyi al
        cv::Mat bottomRoi = rgba(bottomRect);

        // Gaussian Blur uygula (Genişlikte sigma 2.0, Yükseklikte 2.0 yumuşak geçiş için)
        cv::GaussianBlur(bottomRoi, bottomRoi, cv::Size(0, 0), 2.0, 2.0);
    }

    static void renderRgbaToWindow(const uint8_t *rgba, int w, int h) {
        if (!gJavaWindow) return;
        ANativeWindow_Buffer out{};
        if (ANativeWindow_lock(gJavaWindow, &out, nullptr) != 0) return;

        uint8_t *dst = (uint8_t *) out.bits;
        int dstStride = out.stride * 4;
        int srcStride = w * 4;

        int copyH = std::min(h, out.height);
        int copyWBytes = std::min(w, out.width) * 4;

        for (int y = 0; y < copyH; y++) {
            std::memcpy(dst + y * dstStride, rgba + y * srcStride, copyWBytes);
            if (copyWBytes < dstStride) {
                std::memset(dst + y * dstStride + copyWBytes, 0, (size_t) (dstStride - copyWBytes));
            }
        }
        for (int y = copyH; y < out.height; y++) {
            std::memset(dst + y * dstStride, 0, (size_t) dstStride);
        }

        ANativeWindow_unlockAndPost(gJavaWindow);
    }

    static void onImageAvailable(void *ctx, AImageReader *reader) {
        AImage *image = nullptr;
        media_status_t status = AImageReader_acquireNextImage(reader, &image);
        if (status != AMEDIA_OK || !image) return;

        int64_t tsNs = 0;
        AImage_getTimestamp(image, &tsNs);
        gLastSensorTsNs.store((long long) tsNs, std::memory_order_relaxed);

        int32_t w = 0, h = 0;
        AImage_getWidth(image, &w);
        AImage_getHeight(image, &h);

        int32_t yLen = 0, uLen = 0, vLen = 0;
        uint8_t *yData = nullptr;
        uint8_t *uData = nullptr;
        uint8_t *vData = nullptr;
        int32_t yStride = 0, uStride = 0, vStride = 0;
        int32_t uvPixelStride = 0;

        AImage_getPlaneData(image, 0, &yData, &yLen);
        AImage_getPlaneRowStride(image, 0, &yStride);

        AImage_getPlaneData(image, 1, &uData, &uLen);
        AImage_getPlaneRowStride(image, 1, &uStride);
        AImage_getPlanePixelStride(image, 1, &uvPixelStride);

        AImage_getPlaneData(image, 2, &vData, &vLen);
        AImage_getPlaneRowStride(image, 2, &vStride);

        {
            std::lock_guard<std::mutex> lk(gFrameLock);
            size_t needed = (size_t) (w * h * 3 / 2);
            if (gFrameYuv.size() < needed) gFrameYuv.resize(needed);

            gFrameW = w;
            gFrameH = h;

            uint8_t *dst = gFrameYuv.data();
            for (int r = 0; r < h; ++r) {
                std::memcpy(dst + r * w, yData + r * yStride, w);
            }

            uint8_t *uvDst = dst + (w * h);
            int uvH = h / 2;
            int uvW = w / 2;

            if (uvPixelStride == 2) {
                for (int r = 0; r < uvH; ++r) {
                    const uint8_t *vRow = vData + r * vStride;
                    const uint8_t *uRow = uData + r * uStride;
                    uint8_t *dRow = uvDst + r * w;
                    for (int c = 0; c < uvW; ++c) {
                        dRow[c * 2 + 0] = vRow[c * uvPixelStride]; // V
                        dRow[c * 2 + 1] = uRow[c * uvPixelStride]; // U
                    }
                }
            } else {
                for (int r = 0; r < uvH; ++r) {
                    for (int c = 0; c < uvW; ++c) {
                        uvDst[r * w + c * 2 + 0] = vData[r * vStride + c * uvPixelStride];
                        uvDst[r * w + c * 2 + 1] = uData[r * uStride + c * uvPixelStride];
                    }
                }
            }
            gFrameReady.store(true, std::memory_order_relaxed);
        }

        gFrameCv.notify_one();
        AImage_delete(image);
    }

    // --- Decode & Render Loop (Opencv İşleme) ---

    static void decLoop() {
        std::vector<uint8_t> localYuv;
        cv::Mat rgbaReuse;
        int lw = 0, lh = 0;

        while (gRunning.load(std::memory_order_relaxed)) {
            {
                std::unique_lock<std::mutex> lk(gFrameLock);
                gFrameCv.wait(lk, [] {
                    return !gRunning.load(std::memory_order_relaxed) ||
                           gFrameReady.load(std::memory_order_relaxed);
                });
                if (!gRunning.load(std::memory_order_relaxed)) break;

                size_t need = (size_t) (gFrameW * gFrameH * 3 / 2);
                if (localYuv.size() < need) localYuv.resize(need);
                std::memcpy(localYuv.data(), gFrameYuv.data(), need);
                lw = gFrameW;
                lh = gFrameH;
                gFrameReady.store(false, std::memory_order_relaxed);
            }

            if (lw <= 0 || lh <= 0) continue;

            cv::Mat yuv(lh + lh / 2, lw, CV_8UC1, localYuv.data());

            if (rgbaReuse.empty()) {
                rgbaReuse = cv::Mat(lh, lw, CV_8UC4);
            }

            // 1. YUV -> RGBA
            cv::cvtColor(yuv, rgbaReuse, cv::COLOR_YUV2RGBA_NV21);

            // 2. ROTASYON (90 Derece)
            cv::rotate(rgbaReuse, rgbaReuse, cv::ROTATE_90_CLOCKWISE);

            // 3. EFEKTLER (BURASI DEĞİŞTİ)
            // UVC fonksiyonunu çağırma (o üstü siliyor). Biz elle yapıyoruz:

            // A) Keskinleştirme (Opsiyonel ama kalitenin düşmemesi için iyi)
            // Kenarlardan 24px içerisini keskinleştir (UVC ile tutarlı olsun)
            /* İstersen burayı açabilirsin, ama sadece blur istediğin için kapalı tutuyorum.
               Kaliteyi artırmak istersen unsharpRect kullanabilirsin. */

            // B) ALT KENAR BLUR (İstediğin 10px Gaussian)
            applyBottomSeamBlur(rgbaReuse);

            // 4. Ekrana bas
            renderRgbaToWindow(rgbaReuse.data, rgbaReuse.cols, rgbaReuse.rows);
        }
    }

    static void closeAllLocked() {
        if (gSession) {
            ACameraCaptureSession_close(gSession);
            gSession = nullptr;
        }
        if (gPreviewRequest) {
            ACaptureRequest_free(gPreviewRequest);
            gPreviewRequest = nullptr;
        }
        if (gTarget) {
            ACameraOutputTarget_free(gTarget);
            gTarget = nullptr;
        }
        if (gSessionOutput) {
            ACaptureSessionOutput_free(gSessionOutput);
            gSessionOutput = nullptr;
        }
        if (gOutputs) {
            ACaptureSessionOutputContainer_free(gOutputs);
            gOutputs = nullptr;
        }
        if (gDevice) {
            ACameraDevice_close(gDevice);
            gDevice = nullptr;
        }
        if (gImgReaderWindow) {
            gImgReaderWindow = nullptr;
        }
        if (gImgReader) {
            AImageReader_delete(gImgReader);
            gImgReader = nullptr;
        }
        if (gJavaWindow) {
            ANativeWindow_release(gJavaWindow);
            gJavaWindow = nullptr;
        }
        if (gMgr) {
            ACameraManager_delete(gMgr);
            gMgr = nullptr;
        }

        gSensorOrientationDeg = 0;
        gChosenCamId.clear();
        gLastSensorTsNs.store(0, std::memory_order_relaxed);
        gFpsX100.store(0, std::memory_order_relaxed);
        gChosenFps.store(0, std::memory_order_relaxed);
        gLastError.clear();
    }

    static int readSensorOrientationDeg(const char *cameraId) {
        if (!gMgr || !cameraId) return 0;
        ACameraMetadata *chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(gMgr, cameraId, &chars) != ACAMERA_OK ||
            !chars)
            return 0;
        ACameraMetadata_const_entry e{};
        int deg = 0;
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_ORIENTATION, &e) == ACAMERA_OK &&
            e.count > 0) {
            deg = e.data.i32[0];
        }
        ACameraMetadata_free(chars);
        return deg;
    }

    static bool isBackFacing(const char *cameraId) {
        if (!gMgr) return false;
        ACameraMetadata *chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(gMgr, cameraId, &chars) != ACAMERA_OK ||
            !chars)
            return false;
        ACameraMetadata_const_entry entry{};
        bool back = false;
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_FACING, &entry) == ACAMERA_OK &&
            entry.count > 0) {
            back = (entry.data.u8[0] == ACAMERA_LENS_FACING_BACK);
        }
        ACameraMetadata_free(chars);
        return back;
    }

    static float getMinFocalLength(const char *cameraId) {
        ACameraMetadata *chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(gMgr, cameraId, &chars) != ACAMERA_OK || !chars)
            return 1e9f;
        ACameraMetadata_const_entry e{};
        float minF = 1e9f;
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &e) ==
            ACAMERA_OK && e.count > 0) {
            for (uint32_t i = 0; i < e.count; i++) {
                minF = std::min(minF, e.data.f[i]);
            }
        }
        ACameraMetadata_free(chars);
        return minF;
    }

    static std::string pickWidestBackCameraId() {
        if (!gMgr) return "";
        ACameraIdList *list = nullptr;
        if (ACameraManager_getCameraIdList(gMgr, &list) != ACAMERA_OK || !list) return "";

        std::string bestId;
        float bestF = 1e9f;

        for (int i = 0; i < list->numCameras; i++) {
            const char *id = list->cameraIds[i];
            if (!isBackFacing(id)) continue;
            float f = getMinFocalLength(id);
            if (f < bestF) {
                bestF = f;
                bestId = id;
            }
        }
        ACameraManager_deleteCameraIdList(list);
        return bestId;
    }

    static void onDeviceDisconnected(void *, ACameraDevice *) {
        std::lock_guard<std::mutex> lk(gLock);
        setLastErrorLocked("camera disconnected");
        gRunning.store(false);
    }

    static void onDeviceError(void *, ACameraDevice *, int err) {
        std::lock_guard<std::mutex> lk(gLock);
        setLastErrorLocked("device error " + std::to_string(err));
        gRunning.store(false);
    }

    static ACameraDevice_stateCallbacks gDevCbs = {nullptr, onDeviceDisconnected, onDeviceError};

    static void onSessionClosed(void *, ACameraCaptureSession *) {}

    static void onSessionReady(void *, ACameraCaptureSession *) {}

    static void onSessionActive(void *, ACameraCaptureSession *) {}

    static ACameraCaptureSession_stateCallbacks gSessionCbs = {
            nullptr, onSessionClosed, onSessionReady, onSessionActive
    };

    static void trySetFrameRate(ANativeWindow *win, float fps) {
        if (!win) return;
        void *h = dlopen("libandroid.so", RTLD_NOW);
        if (!h) return;
        using Fn = int32_t (*)(ANativeWindow *, float, int32_t);
        auto fn = reinterpret_cast<Fn>(dlsym(h, "ANativeWindow_setFrameRate"));
        if (fn) fn(win, fps, 1);
        dlclose(h);
    }

    bool start(JNIEnv *env, jobject surface, int desiredFps) {
        std::lock_guard<std::mutex> lk(gLock);
        clearLastErrorLocked();
        closeAllLocked();

        gRunning.store(true);
        gThDec = std::thread(decLoop);

        gJavaWindow = ANativeWindow_fromSurface(env, surface);
        if (!gJavaWindow) {
            setLastErrorLocked("ANativeWindow_fromSurface failed");
            return false;
        }

        ANativeWindow_setBuffersGeometry(gJavaWindow, kPreviewH, kPreviewW,
                                         AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

        trySetFrameRate(gJavaWindow, (float) desiredFps);

        gMgr = ACameraManager_create();
        if (!gMgr) {
            setLastErrorLocked("ACameraManager create failed");
            return false;
        }

        std::string camId = pickWidestBackCameraId();
        if (camId.empty()) {
            setLastErrorLocked("No back camera found");
            return false;
        }
        gChosenCamId = camId;
        gSensorOrientationDeg = readSensorOrientationDeg(camId.c_str());

        media_status_t ms = AImageReader_new(kPreviewW, kPreviewH, AIMAGE_FORMAT_YUV_420_888, 4,
                                             &gImgReader);
        if (ms != AMEDIA_OK || !gImgReader) {
            setLastErrorLocked("AImageReader_new failed");
            return false;
        }

        AImageReader_ImageListener listener{
                .context = nullptr,
                .onImageAvailable = onImageAvailable
        };
        AImageReader_setImageListener(gImgReader, &listener);

        ms = AImageReader_getWindow(gImgReader, &gImgReaderWindow);
        if (ms != AMEDIA_OK || !gImgReaderWindow) {
            setLastErrorLocked("AImageReader_getWindow failed");
            return false;
        }

        camera_status_t cs = ACameraManager_openCamera(gMgr, camId.c_str(), &gDevCbs, &gDevice);
        if (cs != ACAMERA_OK || !gDevice) {
            setLastErrorLocked("openCamera failed");
            return false;
        }

        ACaptureSessionOutputContainer_create(&gOutputs);
        ACaptureSessionOutput_create(gImgReaderWindow, &gSessionOutput);
        ACaptureSessionOutputContainer_add(gOutputs, gSessionOutput);

        ACameraOutputTarget_create(gImgReaderWindow, &gTarget);
        ACameraDevice_createCaptureRequest(gDevice, TEMPLATE_PREVIEW, &gPreviewRequest);
        ACaptureRequest_addTarget(gPreviewRequest, gTarget);

        int32_t fpsRange[2] = {30, 30};
        ACaptureRequest_setEntry_i32(gPreviewRequest, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2,
                                     fpsRange);

        uint8_t af = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
        ACaptureRequest_setEntry_u8(gPreviewRequest, ACAMERA_CONTROL_AF_MODE, 1, &af);

        uint8_t vs = ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
        ACaptureRequest_setEntry_u8(gPreviewRequest, ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE, 1,
                                    &vs);
        uint8_t os = ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_OFF;
        ACaptureRequest_setEntry_u8(gPreviewRequest, ACAMERA_LENS_OPTICAL_STABILIZATION_MODE, 1,
                                    &os);

        cs = ACameraDevice_createCaptureSession(gDevice, gOutputs, &gSessionCbs, &gSession);
        if (cs != ACAMERA_OK) {
            setLastErrorLocked("createCaptureSession failed");
            return false;
        }

        ACameraCaptureSession_setRepeatingRequest(gSession, nullptr, 1, &gPreviewRequest, nullptr);

        ALOGI("BackCam started via ImageReader+OpenCV pipeline.");
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lk(gLock);
        if (gRunning.load()) {
            gRunning.store(false);
            gFrameCv.notify_all();
            if (gThDec.joinable()) gThDec.join();
        }
        closeAllLocked();
    }

    long long lastSensorTimestampNs() { return gLastSensorTsNs.load(std::memory_order_relaxed); }

    int estimatedFpsX100() { return gFpsX100.load(std::memory_order_relaxed); }

    int sensorOrientationDeg() { return gSensorOrientationDeg; }

    std::string chosenCameraId() { return gChosenCamId; }

    int chosenFps() { return gChosenFps.load(std::memory_order_relaxed); }

    std::string lastError() {
        std::lock_guard<std::mutex> lk(gLock);
        return gLastError;
    }

} // namespace backcam