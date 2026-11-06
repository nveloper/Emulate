cmake_minimum_required(VERSION 3.22.1)
project(emufrontend CXX)

add_library(emufrontend SHARED
    frontend.cpp
    audio.cpp
    timestretch.cpp
    video.cpp
    netplay.cpp
    jni_bridge.cpp)

target_include_directories(emufrontend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(emufrontend PRIVATE -O2 -Wall -Wno-unused-parameter)
target_link_libraries(emufrontend android log aaudio EGL GLESv2 dl)
