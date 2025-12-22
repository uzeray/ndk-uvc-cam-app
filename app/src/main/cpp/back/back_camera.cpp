// back_camera.cpp

#include "back_camera.h"
#include "../common/logging.h"

#include <android/native_window_jni.h>
#include <android/native_window.h>

#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraMetadata.h>

#include <atomic>
#include <mutex>
#include <string>
#include <cstring>
#include <dlfcn.h>
#include <vector>
#include <algorithm>

namespace backcam {

    static std::mutex gLock;

    static ACameraManager *gMgr = nullptr;
    static ACameraDevice *gDevice = nullptr;
    static ACameraCaptureSession *gSession = nullptr;

    static ACaptureRequest *gPreviewRequest = nullptr;
    static ACameraOutputTarget *gTarget = nullptr;

    static ACaptureSessionOutputContainer *gOutputs = nullptr;
    static ACaptureSessionOutput *gSessionOutput = nullptr;

    static ANativeWindow *gWindow = nullptr;

    static std::atomic<long long> gLastSensorTsNs{0};
    static std::atomic<long long> gPrevTsNs{0};
    static std::atomic<int> gFpsX100{0};
    static std::atomic<int> gChosenFps{0};
    static std::atomic<bool> gRepeatingStarted{false};

    static std::string gLastError;

    static constexpr int kMinFps = 240;
    static constexpr int kPreviewW = 1920;
    static constexpr int kPreviewH = 1080;

    static void setLastErrorLocked(const std::string &msg) { gLastError = msg; }

    static void clearLastErrorLocked() { gLastError.clear(); }

    static void closeAllLocked();

    static bool chooseBestPreviewSize16by9(const char *camId, int32_t *outW, int32_t *outH) {
        if (!gMgr || !camId || !outW || !outH) return false;

        // SurfaceTexture/ANativeWindow output for camera preview is typically ImageFormat.PRIVATE (34)
        static constexpr int32_t kFmtPrivate = 120;

        // “yüksek ama optimum” default hedef: 1920x1080 (60 fps için genelde daha güvenli)
        static constexpr int32_t kTargetW = 1920;
        static constexpr int32_t kTargetH = 1080;

        ACameraMetadata *chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(gMgr, camId, &chars) != ACAMERA_OK ||
            !chars)
            return false;

        ACameraMetadata_const_entry e{};
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                                          &e) != ACAMERA_OK ||
            e.count < 4) {
            ACameraMetadata_free(chars);
            return false;
        }

        bool foundTarget = false;
        int32_t bestW = 0, bestH = 0;

        // 1) Direkt 1920x1080 varsa onu seç
        for (uint32_t i = 0; i + 3 < e.count; i += 4) {
            const int32_t fmt = e.data.i32[i + 0];
            const int32_t w = e.data.i32[i + 1];
            const int32_t h = e.data.i32[i + 2];
            const int32_t in = e.data.i32[i + 3];
            if (in != 0) continue;
            if (fmt != kFmtPrivate) continue;
            if (w == kTargetW && h == kTargetH) {
                bestW = w;
                bestH = h;
                foundTarget = true;
                break;
            }
        }

        ACameraMetadata_free(chars);

        if (bestW > 0 && bestH > 0) {
            *outW = bestW;
            *outH = bestH;
            return true;
        }
        return false;
    }


    static bool isBackFacing(const char *cameraId) {
        if (!gMgr) return false;

        ACameraMetadata *chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(gMgr, cameraId, &chars) != ACAMERA_OK || !chars)
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

        auto parseNullSeparatedIds = [](const uint8_t *data,
                                        uint32_t count) -> std::vector<std::string> {
            std::vector<std::string> out;
            if (!data || count == 0) return out;

            std::string cur;
            cur.reserve(8);
            for (uint32_t i = 0; i < count; i++) {
                char c = static_cast<char>(data[i]);
                if (c == '\0') {
                    if (!cur.empty()) {
                        out.push_back(cur);
                        cur.clear();
                    }
                } else {
                    cur.push_back(c);
                }
            }
            if (!cur.empty()) out.push_back(cur);
            return out;
        };

        auto getPhysicalIds = [&](const char *logicalId) -> std::vector<std::string> {
            std::vector<std::string> ids;
#if defined(ACAMERA_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS)
            ACameraMetadata* chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(gMgr, logicalId, &chars) != ACAMERA_OK || !chars) {
            return ids;
        }

        ACameraMetadata_const_entry e{};
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS, &e) == ACAMERA_OK &&
            e.count > 0) {
            ids = parseNullSeparatedIds(reinterpret_cast<const uint8_t*>(e.data.u8), e.count);
        }

        ACameraMetadata_free(chars);
#else
            (void) logicalId;
#endif
            return ids;
        };

        std::string bestId;
        float bestF = 1e9f;

        for (int i = 0; i < list->numCameras; i++) {
            const char *id = list->cameraIds[i];
            if (!isBackFacing(id)) continue;

            // Candidate: this id
            float candF = getMinFocalLength(id);
            std::string candId = id;

            // If this is a logical multi-cam, check its physical IDs and pick the smallest focal length among them.
            const auto phys = getPhysicalIds(id);
            for (const auto &pid: phys) {
                if (!isBackFacing(pid.c_str())) continue;
                float pf = getMinFocalLength(pid.c_str());
                if (pf < candF) {
                    candF = pf;
                    candId = pid; // prefer physical ultra-wide if it is widest
                }
            }

            if (candF < bestF) {
                bestF = candF;
                bestId = candId;
            }
        }

        ACameraManager_deleteCameraIdList(list);
        return bestId;
    }

    static void trySetFrameRate(ANativeWindow *win, float fps, int32_t compatibility) {
        if (!win) return;

        void *h = dlopen("libandroid.so", RTLD_NOW);
        if (!h) return;

        using Fn = int32_t (*)(ANativeWindow *, float, int32_t);
        auto fn = reinterpret_cast<Fn>(dlsym(h, "ANativeWindow_setFrameRate"));
        if (fn) {
            int32_t r = fn(win, fps, compatibility);
            ALOGI("ANativeWindow_setFrameRate(%.2f) -> %d", fps, r);
        }
        dlclose(h);
    }

#if defined(ACAMERA_CONTROL_ZOOM_RATIO) && defined(ACAMERA_CONTROL_ZOOM_RATIO_RANGE)
    static void applyZoomRatioLocked(const char* camId, float wantZoom) {
    if (!gMgr || !gPreviewRequest || !camId) return;

    ACameraMetadata* chars = nullptr;
    if (ACameraManager_getCameraCharacteristics(gMgr, camId, &chars) != ACAMERA_OK || !chars) return;

    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_CONTROL_ZOOM_RATIO_RANGE, &e) == ACAMERA_OK && e.count >= 2) {
        const float minZ = e.data.f[0];
        const float maxZ = e.data.f[1];
        const float z = std::min(std::max(wantZoom, minZ), maxZ);

        (void)ACaptureRequest_setEntry_f(gPreviewRequest, ACAMERA_CONTROL_ZOOM_RATIO, 0.50, &z);
        ALOGI("BackCam zoomRatio applied=%.2f (range %.2f..%.2f)", z, minZ, maxZ);
    } else {
        ALOGI("BackCam zoomRatio range not available");
    }

    ACameraMetadata_free(chars);
}
#endif

    static void onDeviceDisconnected(void *, ACameraDevice *) {
        std::lock_guard<std::mutex> lk(gLock);
        setLastErrorLocked("camera disconnected");
        closeAllLocked();
    }

    static void onDeviceError(void *, ACameraDevice *, int err) {
        std::lock_guard<std::mutex> lk(gLock);
        setLastErrorLocked(std::string("camera device error=") + std::to_string(err));
        closeAllLocked();
    }

    static ACameraDevice_stateCallbacks gDevCbs = {
            nullptr,
            onDeviceDisconnected,
            onDeviceError
    };

    static void
    onCaptureStarted(void *, ACameraCaptureSession *, const ACaptureRequest *, int64_t timestamp) {
        gLastSensorTsNs.store((long long) timestamp, std::memory_order_relaxed);

        long long prev = gPrevTsNs.exchange((long long) timestamp, std::memory_order_relaxed);
        if (prev != 0 && timestamp > prev) {
            double fps = 1e9 / (double) (timestamp - prev);
            if (fps > 0.0 && fps < 10000.0) {
                gFpsX100.store((int) (fps * 100.0), std::memory_order_relaxed);
            }
        }
    }

    static void onCaptureCompleted(void *, ACameraCaptureSession *, ACaptureRequest *,
                                   const ACameraMetadata *result) {
        if (!result) return;
        ACameraMetadata_const_entry e{};
        if (ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_TIMESTAMP, &e) == ACAMERA_OK &&
            e.count > 0) {
            gLastSensorTsNs.store((long long) e.data.i64[0], std::memory_order_relaxed);
        }
    }

    static ACameraCaptureSession_captureCallbacks gCapCbs = {
            nullptr,
            onCaptureStarted,
            nullptr,
            onCaptureCompleted,
            nullptr,
            nullptr,
            nullptr,
            nullptr
    };

    static void startRepeatingLocked() {
        if (!gSession || !gPreviewRequest) return;
        if (gRepeatingStarted.exchange(true)) return;

        int seqId = 0;
        camera_status_t st = ACameraCaptureSession_setRepeatingRequest(
                gSession, &gCapCbs, 1, &gPreviewRequest, &seqId
        );
        if (st != ACAMERA_OK) {
            setLastErrorLocked(std::string("setRepeatingRequest failed=") + std::to_string(st));
            gRepeatingStarted.store(false);
        } else {
            clearLastErrorLocked();
        }
    }

    static void onSessionClosed(void *, ACameraCaptureSession *) {
        std::lock_guard<std::mutex> lk(gLock);
        gSession = nullptr;
        gRepeatingStarted.store(false);
    }

    static void onSessionReady(void *, ACameraCaptureSession *session) {
        std::lock_guard<std::mutex> lk(gLock);
        gSession = session;
        startRepeatingLocked();
    }

    static void onSessionActive(void *, ACameraCaptureSession *session) {
        std::lock_guard<std::mutex> lk(gLock);
        gSession = session;
        startRepeatingLocked();
    }

    static ACameraCaptureSession_stateCallbacks gSessionCbs = {
            nullptr,
            onSessionClosed,
            onSessionReady,
            onSessionActive
    };

    static void closeAllLocked() {
        if (gSession) {
            ACameraCaptureSession_close(gSession);
            gSession = nullptr;
        }
        gRepeatingStarted.store(false);

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
        if (gWindow) {
            ANativeWindow_release(gWindow);
            gWindow = nullptr;
        }
        if (gMgr) {
            ACameraManager_delete(gMgr);
            gMgr = nullptr;
        }

        gLastSensorTsNs.store(0, std::memory_order_relaxed);
        gPrevTsNs.store(0, std::memory_order_relaxed);
        gFpsX100.store(0, std::memory_order_relaxed);
        gChosenFps.store(0, std::memory_order_relaxed);
        gLastError.clear();
    }

    static bool setFpsRangeLocked(int mn, int mx) {
        if (!gPreviewRequest) return false;
        int32_t fpsRange[2] = {mn, mx};
        camera_status_t st = ACaptureRequest_setEntry_i32(
                gPreviewRequest,
                ACAMERA_CONTROL_AE_TARGET_FPS_RANGE,
                2,
                fpsRange
        );
        return st == ACAMERA_OK;
    }

    static void chooseAndApplyFpsRangeLocked(const char *camId, int desiredFps) {
        gChosenFps.store(0, std::memory_order_relaxed);

        ACameraMetadata *chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(gMgr, camId, &chars) != ACAMERA_OK || !chars)
            return;

        ACameraMetadata_const_entry e{};
        std::vector<std::pair<int, int>> ranges;

        if (ACameraMetadata_getConstEntry(chars, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
                                          &e) ==
            ACAMERA_OK && e.count >= 2) {
            for (uint32_t i = 0; i + 1 < e.count; i += 2) {
                int mn = e.data.i32[i];
                int mx = e.data.i32[i + 1];
                ranges.emplace_back(mn, mx);
            }
        }

        ACameraMetadata_free(chars);
        if (ranges.empty()) return;

        const int want = std::max(kMinFps, desiredFps);

        int chosenMin = 120, chosenMax = 240;

        // 1) Exact fixed range (60-60)
        for (auto &r: ranges) {
            if (r.first == want && r.second == want) {
                chosenMin = want;
                chosenMax = want;
                break;
            }
        }

        // 2) Range ending at 60 (e.g. 30-60) -> pick highest min
        if (chosenMax == 0) {
            int bestMn = -1;
            for (auto &r: ranges) {
                if (r.second == want && r.first <= want) {
                    if (r.first > bestMn) {
                        bestMn = r.first;
                        chosenMin = r.first;
                        chosenMax = want;
                    }
                }
            }
        }

        // 3) Any range that contains 60, prefer smallest max above 60
        if (chosenMax == 0) {
            int bestMax = 0;
            int bestMin = 0;
            for (auto &r: ranges) {
                if (r.first <= want && r.second >= want) {
                    if (bestMax == 0 || r.second < bestMax) {
                        bestMax = r.second;
                        bestMin = r.first;
                    }
                }
            }
            if (bestMax != 0) {
                chosenMin = bestMin;
                chosenMax = bestMax;
            }
        }

        // 4) Fallback: highest available max
        if (chosenMax == 0) {
            int bestMax = 0, bestMin = 0;
            for (auto &r: ranges) {
                if (r.second > bestMax) {
                    bestMax = r.second;
                    bestMin = r.first;
                }
            }
            chosenMin = bestMin;
            chosenMax = bestMax;
        }

        if (chosenMax > 0) {
            bool ok = setFpsRangeLocked(chosenMin, chosenMax);
            if (!ok) return;

            // mümkünse 60-60 zorla
            if (!(chosenMin == want && chosenMax == want) && chosenMax >= want) {
                (void) setFpsRangeLocked(want, want);
            }

            gChosenFps.store(want, std::memory_order_relaxed);
        }
    }

    static void applyStabilityAndFovSettingsLocked(const char *camId) {
        if (!gMgr || !gPreviewRequest) return;

#if defined(ACAMERA_DISTORTION_CORRECTION_MODE) && defined(ACAMERA_DISTORTION_CORRECTION_MODE_OFF) && \
    defined(ACAMERA_DISTORTION_CORRECTION_AVAILABLE_MODES)
        // Distortion correction OFF => en geniş FOV (balıkgözü + kenar bozulması olabilir)
        {
            ACameraMetadata* chars = nullptr;
            if (ACameraManager_getCameraCharacteristics(gMgr, camId, &chars) == ACAMERA_OK && chars) {
                ACameraMetadata_const_entry dm{};
                bool supportsOff = false;
                if (ACameraMetadata_getConstEntry(chars, ACAMERA_DISTORTION_CORRECTION_AVAILABLE_MODES, &dm) == ACAMERA_OK &&
                    dm.count > 0) {
                    for (uint32_t i = 0; i < dm.count; i++) {
                        if (dm.data.u8[i] == ACAMERA_DISTORTION_CORRECTION_MODE_OFF) {
                            supportsOff = true;
                            break;
                        }
                    }
                }
                ACameraMetadata_free(chars);

                if (supportsOff) {
                    uint8_t off = ACAMERA_DISTORTION_CORRECTION_MODE_OFF;
                    (void)ACaptureRequest_setEntry_u8(gPreviewRequest, ACAMERA_DISTORTION_CORRECTION_MODE, 1, &off);
                }
            }
        }
#endif

        // Anti-banding 50Hz
        {
            uint8_t ab = ACAMERA_CONTROL_AE_ANTIBANDING_MODE_50HZ;
            (void) ACaptureRequest_setEntry_u8(gPreviewRequest, ACAMERA_CONTROL_AE_ANTIBANDING_MODE,
                                               1, &ab);
        }

        // Video stabilization OFF (daraltma/warp etkilerini azaltır)
        {
            uint8_t vs = ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
            (void) ACaptureRequest_setEntry_u8(gPreviewRequest,
                                               ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE, 1, &vs);
        }

        // Optical stabilization OFF (varsa)
        {
            uint8_t os = ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_OFF;
            (void) ACaptureRequest_setEntry_u8(gPreviewRequest,
                                               ACAMERA_LENS_OPTICAL_STABILIZATION_MODE, 1, &os);
        }

        // Noise/Edge FAST (fps korumak için)
        {
            uint8_t nr = ACAMERA_NOISE_REDUCTION_MODE_FAST;
            (void) ACaptureRequest_setEntry_u8(gPreviewRequest, ACAMERA_NOISE_REDUCTION_MODE, 1,
                                               &nr);
        }
        {
            uint8_t ed = ACAMERA_EDGE_MODE_FAST;
            (void) ACaptureRequest_setEntry_u8(gPreviewRequest, ACAMERA_EDGE_MODE, 1, &ed);
        }
        {
            uint8_t ca = ACAMERA_COLOR_CORRECTION_ABERRATION_MODE_FAST;
            (void) ACaptureRequest_setEntry_u8(gPreviewRequest,
                                               ACAMERA_COLOR_CORRECTION_ABERRATION_MODE, 1, &ca);
        }

        // Crop region'ı aktif array'e set ederek dijital crop/zoom ihtimalini minimize et
        ACameraMetadata *chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(gMgr, camId, &chars) == ACAMERA_OK && chars) {
            ACameraMetadata_const_entry e{};
            if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &e) ==
                ACAMERA_OK &&
                e.count >= 4) {
                int32_t crop[4] = {e.data.i32[0], e.data.i32[1], e.data.i32[2], e.data.i32[3]};
                (void) ACaptureRequest_setEntry_i32(gPreviewRequest, ACAMERA_SCALER_CROP_REGION, 4,
                                                    crop);
            }
            ACameraMetadata_free(chars);
        }
    }

    bool start(JNIEnv *env, jobject surface, int desiredFps) {
        std::lock_guard<std::mutex> lk(gLock);
        clearLastErrorLocked();
        closeAllLocked();

        const int wantFps = std::max(kMinFps, (int) desiredFps);

        gMgr = ACameraManager_create();
        if (!gMgr) {
            setLastErrorLocked("ACameraManager_create failed");
            return false;
        }

        gWindow = ANativeWindow_fromSurface(env, surface);
        if (!gWindow) {
            setLastErrorLocked("ANativeWindow_fromSurface failed");
            closeAllLocked();
            return false;
        }

        std::string camId = pickWidestBackCameraId();
        if (camId.empty()) {
            setLastErrorLocked("no back camera found");
            closeAllLocked();
            return false;
        }

        // Şimdilik Kotlin BACK_BUF_W/H ile birebir aynı tutuyoruz (stretch/ratio sorunu sıfırlansın)
        const int32_t prevW = kPreviewW;   // 1920
        const int32_t prevH = kPreviewH;   // 1080

        (void) ANativeWindow_setBuffersGeometry(gWindow, prevW, prevH, 0);
        ALOGI("BackCam preview geometry fixed to %dx%d", prevW, prevH);

        camera_status_t st = ACameraManager_openCamera(gMgr, camId.c_str(), &gDevCbs, &gDevice);
        if (st != ACAMERA_OK || !gDevice) {
            setLastErrorLocked(std::string("openCamera failed=") + std::to_string(st));
            closeAllLocked();
            return false;
        }

        if (ACaptureSessionOutputContainer_create(&gOutputs) != ACAMERA_OK) {
            setLastErrorLocked("OutputContainer_create failed");
            closeAllLocked();
            return false;
        }

        if (ACaptureSessionOutput_create(gWindow, &gSessionOutput) != ACAMERA_OK) {
            setLastErrorLocked("SessionOutput_create failed");
            closeAllLocked();
            return false;
        }

        if (ACaptureSessionOutputContainer_add(gOutputs, gSessionOutput) != ACAMERA_OK) {
            setLastErrorLocked("OutputContainer_add failed");
            closeAllLocked();
            return false;
        }

        // TEMPLATE_RECORD -> yüksek fps stabil
        if (ACameraDevice_createCaptureRequest(gDevice, TEMPLATE_PREVIEW, &gPreviewRequest) != ACAMERA_OK) {
            setLastErrorLocked("createCaptureRequest failed");
            closeAllLocked();
            return false;
        }

        if (ACameraOutputTarget_create(gWindow, &gTarget) != ACAMERA_OK) {
            setLastErrorLocked("OutputTarget_create failed");
            closeAllLocked();
            return false;
        }

        if (ACaptureRequest_addTarget(gPreviewRequest, gTarget) != ACAMERA_OK) {
            setLastErrorLocked("Request_addTarget failed");
            closeAllLocked();
            return false;
        }

        // FPS: 60 altı kabul yok
        chooseAndApplyFpsRangeLocked(camId.c_str(), wantFps);
        int chosen = gChosenFps.load(std::memory_order_relaxed);
        if (chosen < kMinFps) {
            setLastErrorLocked("Back camera 60fps target not available on this device/config");
            closeAllLocked();
            return false;
        }

        // Stabilite/FOV ayarları
        applyStabilityAndFovSettingsLocked(camId.c_str());

#if defined(ACAMERA_CONTROL_ZOOM_RATIO) && defined(ACAMERA_CONTROL_ZOOM_RATIO_RANGE)
        // Ultra-wide fiziksel ID seçildiyse: 1.0 = sensörün doğal geniş açısı (ek dijital zoom yok)
        applyZoomRatioLocked(camId.c_str(), 0.3f);
#endif

        // SurfaceFlinger için frame-rate hint
        trySetFrameRate(gWindow, (float) chosen,
                        (int32_t) ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);

        uint8_t af = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
        (void) ACaptureRequest_setEntry_u8(gPreviewRequest, ACAMERA_CONTROL_AF_MODE, 1, &af);

        gRepeatingStarted.store(false);
        st = ACameraDevice_createCaptureSession(gDevice, gOutputs, &gSessionCbs, &gSession);
        if (st != ACAMERA_OK) {
            setLastErrorLocked(std::string("createCaptureSession failed=") + std::to_string(st));
            closeAllLocked();
            return false;
        }

        startRepeatingLocked();
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lk(gLock);
        closeAllLocked();
    }

    long long lastSensorTimestampNs() { return gLastSensorTsNs.load(std::memory_order_relaxed); }

    int estimatedFpsX100() { return gFpsX100.load(std::memory_order_relaxed); }

    int chosenFps() { return gChosenFps.load(std::memory_order_relaxed); }

    std::string lastError() {
        std::lock_guard<std::mutex> lk(gLock);
        return gLastError;
    }

} // namespace backcam
