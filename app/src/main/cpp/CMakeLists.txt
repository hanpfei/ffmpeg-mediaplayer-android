cmake_minimum_required(VERSION 3.4.1)
project(echo LANGUAGES C CXX)

set(library_path ${CMAKE_SOURCE_DIR}/../libs)

#将第三方库作为动态库引用
add_library(avutil
        SHARED
        IMPORTED)
add_library(swscale
        SHARED
        IMPORTED)
add_library(swresample
        SHARED
        IMPORTED)
add_library(avcodec
        SHARED
        IMPORTED)
add_library(avformat
        SHARED
        IMPORTED)
add_library(avfilter
        SHARED
        IMPORTED)
add_library(avdevice
        SHARED
        IMPORTED)

# 指定第三方库的绝对路径
set_target_properties(avutil
        PROPERTIES IMPORTED_LOCATION
        ${library_path}/${ANDROID_ABI}/libavutil.so)
set_target_properties(swscale
        PROPERTIES IMPORTED_LOCATION
        ${library_path}/${ANDROID_ABI}/libswscale.so)
set_target_properties(swresample
        PROPERTIES IMPORTED_LOCATION
        ${library_path}/${ANDROID_ABI}/libswresample.so)
set_target_properties(avcodec
        PROPERTIES IMPORTED_LOCATION
        ${library_path}/${ANDROID_ABI}/libavcodec.so)
set_target_properties(avformat
        PROPERTIES IMPORTED_LOCATION
        ${library_path}/${ANDROID_ABI}/libavformat.so)
set_target_properties(avfilter
        PROPERTIES IMPORTED_LOCATION
        ${library_path}/${ANDROID_ABI}/libavfilter.so)
set_target_properties(avdevice
        PROPERTIES IMPORTED_LOCATION
        ${library_path}/${ANDROID_ABI}/libavdevice.so)


add_library(echo
  SHARED
    audio_encoded_frame_file_writer.cpp
    audio_encoded_frame_decoder.cpp
    audio_frame_writer.cpp
    audio_main.cpp
    audio_player.cpp
    audio_recorder.cpp
    audio_effect.cpp
    audio_common.cpp
    debug_utils.cpp
    media_demuxer.cpp
    media_player.cpp
    media_utils.cpp
    video_encoded_frame_decoder.cpp
    video_encoded_frame_writer.cpp
    video_frame_writer.cpp)

target_include_directories(echo
        PRIVATE
        ${CMAKE_SOURCE_DIR}/libs/include)

#include libraries needed for echo lib
target_link_libraries(echo
  PRIVATE
    OpenSLES
    android
    log
    atomic
    avutil
    swscale
    swresample
    avcodec
    avformat
    avfilter
    avdevice)

target_compile_options(echo
  PRIVATE
    -Wall -Werror)
