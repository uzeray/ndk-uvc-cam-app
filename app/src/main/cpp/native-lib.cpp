#include <jni.h>
#include <string>

#include <android/bitmap.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "back/back_camera.h"
#include "uvc/uvc_camera.h"

extern "C" JNIEXPORT jboolean JNICALL
Java_com_uzera_camcpp_MainActivity_nativeStartBackPreview(JNIEnv *env, jobject, jobject surface,
                                                          jint desiredFps) {
    return backcam::start(env, surface, (int) desiredFps) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_uzera_camcpp_MainActivity_nativeStopBackPreview(JNIEnv *, jobject) {
    backcam::stop();
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_uzera_camcpp_MainActivity_nativeGetBackLastSensorTimestampNs(JNIEnv *, jobject) {
    return (jlong) backcam::lastSensorTimestampNs();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_uzera_camcpp_MainActivity_nativeGetBackEstimatedFpsX100(JNIEnv *, jobject) {
    return (jint) backcam::estimatedFpsX100();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_uzera_camcpp_MainActivity_nativeGetBackChosenFps(JNIEnv *, jobject) {
    return (jint) backcam::chosenFps();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_uzera_camcpp_MainActivity_nativeGetBackLastError(JNIEnv *env, jobject) {
    std::string s = backcam::lastError();
    return env->NewStringUTF(s.c_str());
}

// -------- UVC --------

extern "C" JNIEXPORT jboolean JNICALL
Java_com_uzera_camcpp_MainActivity_nativeStartExternalPreview(JNIEnv *env, jobject, jobject surface,
                                                              jint desiredFps) {
    return uvc::start(env, surface, (int) desiredFps) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_uzera_camcpp_MainActivity_nativeStopExternalPreview(JNIEnv *, jobject) {
    uvc::stop();
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_uzera_camcpp_MainActivity_nativeGetExtLastSensorTimestampNs(JNIEnv *, jobject) {
    return (jlong) uvc::lastFrameTimestampNs();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_uzera_camcpp_MainActivity_nativeGetExtEstimatedFpsX100(JNIEnv *, jobject) {
    return (jint) uvc::estimatedFpsX100();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_uzera_camcpp_MainActivity_nativeGetExtChosenFps(JNIEnv *, jobject) {
    return (jint) uvc::chosenFps();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_uzera_camcpp_MainActivity_nativeGetExtLastError(JNIEnv *env, jobject) {
    std::string s = uvc::lastError();
    return env->NewStringUTF(s.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_uzera_camcpp_MainActivity_nativeGetExtChosenMode(JNIEnv *env, jobject) {
    std::string s = uvc::chosenMode();
    return env->NewStringUTF(s.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_uzera_camcpp_MainActivity_nativeGetOpenCvVersion(JNIEnv *env, jobject) {
    return env->NewStringUTF(CV_VERSION);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_uzera_camcpp_MainActivity_nativeOpenCvSmokeTest(JNIEnv *env, jobject) {
    cv::Mat m(32, 32, CV_8UC1, cv::Scalar(128));
    cv::GaussianBlur(m, m, cv::Size(5, 5), 0.0);
    int sum = (int) cv::sum(m)[0];
    std::string s = "OK sum=" + std::to_string(sum);
    return env->NewStringUTF(s.c_str());
}

// ============================================================
//  OpenCV Seam Blend
//  backStrip: (W x overlap)
//  extStrip : (W x overlap)
//  outBand  : (W x 2*overlap)
//  Gaussian blur + crossfade to naturalize boundary
// ============================================================

static inline bool
lockBitmapRGBA(JNIEnv *env, jobject bmp, AndroidBitmapInfo &info, void **pixels) {
    if (!bmp) return false;
    if (AndroidBitmap_getInfo(env, bmp, &info) != ANDROID_BITMAP_RESULT_SUCCESS) return false;
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) return false;
    if (AndroidBitmap_lockPixels(env, bmp, pixels) != ANDROID_BITMAP_RESULT_SUCCESS) return false;
    return true;
}

static inline void unlockBitmap(JNIEnv *env, jobject bmp) {
    if (bmp) AndroidBitmap_unlockPixels(env, bmp);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_uzera_camcpp_MainActivity_nativeBlendSeam(JNIEnv *env, jobject,
                                                   jobject backStrip,
                                                   jobject extStrip,
                                                   jobject outBand,
                                                   jint overlapPx) {
    if (overlapPx <= 4) return JNI_FALSE;

    AndroidBitmapInfo bi{}, ei{}, oi{};
    void *bp = nullptr;
    void *ep = nullptr;
    void *op = nullptr;

    if (!lockBitmapRGBA(env, backStrip, bi, &bp)) return JNI_FALSE;
    if (!lockBitmapRGBA(env, extStrip, ei, &ep)) {
        unlockBitmap(env, backStrip);
        return JNI_FALSE;
    }
    if (!lockBitmapRGBA(env, outBand, oi, &op)) {
        unlockBitmap(env, extStrip);
        unlockBitmap(env, backStrip);
        return JNI_FALSE;
    }

    const int W = (int) bi.width;
    const int H = (int) bi.height;

    const int overlap = (int) overlapPx;
    const int outH = overlap * 2;

    bool ok = true;

    // validate sizes
    if ((int) ei.width != W || (int) ei.height != H) ok = false;
    if ((int) oi.width != W || (int) oi.height != outH) ok = false;
    if (H != overlap) ok = false;

    if (ok) {
        cv::Mat back(H, W, CV_8UC4, bp);
        cv::Mat ext(H, W, CV_8UC4, ep);
        cv::Mat out(outH, W, CV_8UC4, op);

        // Build vertical crossfade band:
        // y in [0..outH-1]
        // - top half samples back rows [0..overlap-1]
        // - bottom half samples ext rows [0..overlap-1]
        // - alpha decreases smoothly from 1 to 0 across outH
        for (int y = 0; y < outH; y++) {
            float a = 1.0f - (float) y / (float) (outH - 1); // back weight
            int by = (y < overlap) ? y : (overlap - 1);
            int ey = (y < overlap) ? 0 : (y - overlap);

            if (ey < 0) ey = 0;
            if (ey > overlap - 1) ey = overlap - 1;

            cv::Mat br = back.row(by);
            cv::Mat er = ext.row(ey);
            cv::Mat orow = out.row(y);

            cv::addWeighted(br, a, er, 1.0f - a, 0.0, orow);
        }

        // Gaussian blur to naturalize seam (light, fast)
        // sigmaY tuned; sigmaX kept small to avoid horizontal softness
        cv::GaussianBlur(out, out, cv::Size(0, 0), 1.6, 0.6);
    }

    unlockBitmap(env, outBand);
    unlockBitmap(env, extStrip);
    unlockBitmap(env, backStrip);

    return ok ? JNI_TRUE : JNI_FALSE;
}
