#pragma once

#include <jni.h>
#include <string>

namespace backcam {
    bool start(JNIEnv *env, jobject surface, int desiredFps);

    void stop();

    long long lastSensorTimestampNs();

    int estimatedFpsX100();

    int chosenFps();

    std::string lastError();

    int sensorOrientationDeg();
    std::string chosenCameraId();
}
