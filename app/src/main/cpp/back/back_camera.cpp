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

    static int gSensorOrientationDeg = 0;
    static std::string gChosenCamId;

    static int readSensorOrientationDeg(const char *cameraId) {
        if (!gMgr || !cameraId) return 0;
        ACameraMetadata *chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(gMgr, cameraId, &chars) != ACAMERA_OK ||
            !chars) {
            return 0;
        }

        ACameraMetadata_const_entry e{};
        int deg = 0;
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_ORIENTATION, &e) == ACAMERA_OK &&
            e.count > 0) {
            deg = e.data.i32[0];
        }
        ACameraMetadata_free(chars);
        return deg;
    }

    static std::string gLastError;

    static constexpr int kMinFps = 60;
    static constexpr int kPreviewW = 1280;
    static constexpr int kPreviewH = 720;

    static void setLastErrorLocked(const std::string &msg) { gLastError = msg; }

    static void clearLastErrorLocked() { gLastError.clear(); }

    static void closeAllLocked();

    static bool chooseBestPreviewSize16by9(const char *camId, int32_t *outW, int32_t *outH) {
        if (!gMgr || !camId || !outW || !outH) return false;
        static constexpr int32_t kFmtPrivate = 120;
        static constexpr int32_t kTargetW = 1920;
        static constexpr int32_t kTargetH = 1080;

        ACameraMetadata *chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(gMgr, camId, &chars) != ACAMERA_OK || !chars)
            return false;

        ACameraMetadata_const_entry e{};
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                                          &e) != ACAMERA_OK || e.count < 4) {
            ACameraMetadata_free(chars);
            return false;
        }

        bool foundTarget = false;
        int32_t bestW = 0, bestH = 0;

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
            if (ACameraMetadata_getConstEntry(chars, ACAMERA_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS, &e) == ACAMERA_OK && e.count > 0) {
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
            float candF = getMinFocalLength(id);
            std::string candId = id;
            const auto phys = getPhysicalIds(id);
            for (const auto &pid: phys) {
                if (!isBackFacing(pid.c_str())) continue;
                float pf = getMinFocalLength(pid.c_str());
                if (pf < candF) {
                    candF = pf;
                    candId = pid;
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
            (void)ACaptureRequest_setEntry_f(gPreviewRequest, ACAMERA_CONTROL_ZOOM_RATIO, 1, &z);
            ALOGI("BackCam zoomRatio applied=%.2f (range %.2f..%.2f)", z, minZ, maxZ);
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

    static ACameraDevice_stateCallbacks gDevCbs = {nullptr, onDeviceDisconnected, onDeviceError};

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
            nullptr, onCaptureStarted, nullptr, onCaptureCompleted, nullptr, nullptr, nullptr,
            nullptr
    };

    static void startRepeatingLocked() {
        if (!gSession || !gPreviewRequest) return;
        if (gRepeatingStarted.exchange(true)) return;
        int seqId = 0;
        camera_status_t st = ACameraCaptureSession_setRepeatingRequest(gSession, &gCapCbs, 1,
                                                                       &gPreviewRequest, &seqId);
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

    static ACameraCaptureSession_stateCallbacks gSessionCbs = {nullptr, onSessionClosed,
                                                               onSessionReady, onSessionActive};

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
        gSensorOrientationDeg = 0;
        gChosenCamId.clear();
        gLastSensorTsNs.store(0, std::memory_order_relaxed);
        gPrevTsNs.store(0, std::memory_order_relaxed);
        gFpsX100.store(0, std::memory_order_relaxed);
        gChosenFps.store(0, std::memory_order_relaxed);
        gLastError.clear();
    }

    static bool setFpsRangeLocked(int mn, int mx) {
        if (!gPreviewRequest) return false;
        int32_t fpsRange[2] = {mn, mx};
        camera_status_t st = ACaptureRequest_setEntry_i32(gPreviewRequest,
                                                          ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2,
                                                          fpsRange);
        return st == ACAMERA_OK;
    }

    static void chooseAndApplyFpsRangeLocked(const char *camId, int desiredFps) {
        gChosenFps.store(0, std::memory_order_relaxed);
        ACameraMetadata *chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(gMgr, camId, &chars) != ACAMERA_OK ||
            !chars)
            return;

        ACameraMetadata_const_entry e{};
        std::vector<std::pair<int, int>> ranges;
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
                                          &e) == ACAMERA_OK && e.count >= 2) {
            for (uint32_t i = 0; i + 1 < e.count; i += 2) {
                ranges.emplace_back(e.data.i32[i], e.data.i32[i + 1]);
            }
        }
        ACameraMetadata_free(chars);
        if (ranges.empty()) return;

        int target = 30;
        int chosenMin = 30, chosenMax = 30;
        bool found = false;

        for (auto &r: ranges) {
            if (r.first == 30 && r.second == 30) {
                chosenMin = 30;
                chosenMax = 30;
                found = true;
                break;
            }
        }
        if (!found) {
            for (auto &r: ranges) {
                if (r.first <= 30 && r.second >= 30) {
                    chosenMin = r.first;
                    chosenMax = r.second;
                    found = true;
                    break;
                }
            }
        }

        if (found) {
            setFpsRangeLocked(chosenMin, chosenMax);
            gChosenFps.store(30, std::memory_order_relaxed);
        }
    }

    static void applyStabilityAndFovSettingsLocked(const char *camId) {
        if (!gMgr || !gPreviewRequest) return;

        ACameraMetadata *chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(gMgr, camId, &chars) == ACAMERA_OK && chars) {
            ACameraMetadata_const_entry e{};
            if (ACameraMetadata_getConstEntry(chars, ACAMERA_CONTROL_AE_COMPENSATION_RANGE, &e) ==
                ACAMERA_OK && e.count == 2) {
                int32_t minEv = e.data.i32[0];
                int32_t maxEv = e.data.i32[1];
                int32_t targetEv = 0; // +2 sıcak/kırmızı yapabiliyor -> nötrle
                targetEv = std::max(minEv, std::min(targetEv, maxEv));
                (void)ACaptureRequest_setEntry_i32(
                        gPreviewRequest,
                        ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION,
                        1,
                        &targetEv
                );
            }
            ACameraMetadata_free(chars);
        }

        {
            uint8_t ab = ACAMERA_CONTROL_AE_ANTIBANDING_MODE_50HZ;
            (void) ACaptureRequest_setEntry_u8(gPreviewRequest, ACAMERA_CONTROL_AE_ANTIBANDING_MODE,
                                               1, &ab);
        }

        {
            uint8_t awb = ACAMERA_CONTROL_AWB_MODE_FLUORESCENT;
            (void)ACaptureRequest_setEntry_u8(gPreviewRequest, ACAMERA_CONTROL_AWB_MODE, 1, &awb);
        }

        uint8_t ccm = ACAMERA_COLOR_CORRECTION_MODE_FAST;

        {
            uint8_t vs = ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
            (void) ACaptureRequest_setEntry_u8(gPreviewRequest,
                                               ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE, 1, &vs);
        }
        {
            uint8_t os = ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_OFF;
            (void) ACaptureRequest_setEntry_u8(gPreviewRequest,
                                               ACAMERA_LENS_OPTICAL_STABILIZATION_MODE, 1, &os);
        }

        ACameraMetadata *chars2 = nullptr;
        if (ACameraManager_getCameraCharacteristics(gMgr, camId, &chars2) == ACAMERA_OK && chars2) {
            ACameraMetadata_const_entry e{};

            ACameraMetadata_free(chars2);
        }
    }

    bool start(JNIEnv *env, jobject surface, int desiredFps) {
        std::lock_guard<std::mutex> lk(gLock);
        clearLastErrorLocked();
        closeAllLocked();

        const int wantFps = 60;

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

        gChosenCamId = camId;
        gSensorOrientationDeg = readSensorOrientationDeg(camId.c_str());
        ALOGI("BackCam chosenId=%s sensorOrientation=%d", gChosenCamId.c_str(),
              gSensorOrientationDeg);

        const int32_t prevW = kPreviewW;
        const int32_t prevH = kPreviewH;

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

        if (ACameraDevice_createCaptureRequest(gDevice, TEMPLATE_PREVIEW, &gPreviewRequest) !=
            ACAMERA_OK) {
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

        chooseAndApplyFpsRangeLocked(camId.c_str(), wantFps);

        applyStabilityAndFovSettingsLocked(camId.c_str());


        trySetFrameRate(gWindow, (float) 30,
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

    int sensorOrientationDeg() { return gSensorOrientationDeg; }

    std::string chosenCameraId() { return gChosenCamId; }

    int chosenFps() { return gChosenFps.load(std::memory_order_relaxed); }

    std::string lastError() {
        std::lock_guard<std::mutex> lk(gLock);
        return gLastError;
    }

} // namespace backcam