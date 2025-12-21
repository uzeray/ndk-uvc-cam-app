// native-lib.cpp

#include <jni.h>
#include <string>
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
