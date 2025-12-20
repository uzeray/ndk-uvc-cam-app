cmake_minimum_required(VERSION 3.22.1)
project(camcpp)

add_library(camcpp SHARED
        native-lib.cpp
        back/back_camera.cpp
        uvc/uvc_camera.cpp
        third_party/stb_image_impl.cpp
)

target_compile_features(camcpp PRIVATE cxx_std_17)

find_library(log-lib log)
find_library(android-lib android)
find_library(dl-lib dl)

target_link_libraries(camcpp
        ${log-lib}
        ${android-lib}
        ${dl-lib}
        camera2ndk
)
