// uvc_camera.h

#pragma once

#include <jni.h>
#include <string>

namespace uvc {
    bool start(JNIEnv *env, jobject surface, int desiredFps);

    void stop();

    long long lastFrameTimestampNs();

    int estimatedFpsX100();

    int chosenFps();

    std::string lastError();

    // YENİ
    std::string chosenMode();   // örn: "YUYV 1280x720"
}
