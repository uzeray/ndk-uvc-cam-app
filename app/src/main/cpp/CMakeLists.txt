cmake_minimum_required(VERSION 3.22.1)
project(camcpp)

# OpenCVConfig.cmake burada:
set(OpenCV_DIR "${CMAKE_SOURCE_DIR}/third_party/OpenCV-android-sdk/sdk/native/jni")
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)

add_library(camcpp SHARED
        native-lib.cpp
        back/back_camera.cpp
        uvc/uvc_camera.cpp
)

target_compile_features(camcpp PRIVATE cxx_std_17)

target_include_directories(camcpp PRIVATE
        ${OpenCV_INCLUDE_DIRS}
)

find_library(log-lib log)
find_library(android-lib android)
find_library(dl-lib dl)

target_link_libraries(camcpp
        ${log-lib}
        ${android-lib}
        ${dl-lib}
        camera2ndk
        ${OpenCV_LIBS}
)
