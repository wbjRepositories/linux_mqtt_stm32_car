cmake_minimum_required(VERSION 3.10)

# --- 交叉编译配置开始 (建议放在 project() 之前) ---
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# 定义工具链根目录 (方便后续引用)
set(TOOLCHAIN_ROOT /home/wbj/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot)
set(TOOLCHAIN_DIR ${TOOLCHAIN_ROOT}/arm-buildroot-linux-gnueabihf/sysroot)

set(CMAKE_SYSROOT ${TOOLCHAIN_DIR})

# 指定 C 和 C++ 编译器 (防止 CMake 报错)
set(CMAKE_C_COMPILER ${TOOLCHAIN_ROOT}/bin/arm-buildroot-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_ROOT}/bin/arm-buildroot-linux-gnueabihf-g++)

# 搜索路径模式配置
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# --- 交叉编译配置结束 ---

project(project1 C CXX) # 显式声明支持 C 和 C++

# 设置构建类型
set(CMAKE_BUILD_TYPE "Debug")

# 编译参数：追加而不是覆盖
# 这里的 -O0 -g 用于调试，ARM 架构参数用于生成正确的机器码
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=armv7ve -mfpu=neon -mfloat-abi=hard -mcpu=cortex-a7 -Wall")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv7ve -mfpu=neon -mfloat-abi=hard -mcpu=cortex-a7 -Wall")

# Debug 模式下的额外参数
set(CMAKE_C_FLAGS_DEBUG "-O0 -g -ggdb")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -ggdb")

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/bin) # 建议使用绝对路径，避免混淆

# --- 源文件收集 ---
file(GLOB_RECURSE LVGL_SOURCES ${CMAKE_SOURCE_DIR}/lvgl/src/*.c)
file(GLOB LV_DRIVERS_DISPLAY_SOURCES ${CMAKE_SOURCE_DIR}/lv_drivers/display/*.c)
file(GLOB LV_DRIVERS_INPUT_SOURCES ${CMAKE_SOURCE_DIR}/lv_drivers/indev/*.c)

# 如果你确定启用了 FFmpeg 支持
file(GLOB LVGL_SOURCES_FFMPEG ${CMAKE_SOURCE_DIR}/lvgl/src/extra/libs/ffmpeg/*.c)

set(ALL_SOURCES
    src/main.c
    ${LVGL_SOURCES}
    ${LV_DRIVERS_DISPLAY_SOURCES}
    ${LV_DRIVERS_INPUT_SOURCES}
    ${LVGL_SOURCES_FFMPEG}
)

add_executable(lvglTest ${ALL_SOURCES})

# --- FFmpeg 配置 ---
# 假设 FFmpeg 的头文件和库位于交叉编译工具链的 sysroot 中
# 如果不在 sysroot，需要手动指定绝对路径，例如: set(FFMPEG_DIR /path/to/ffmpeg/install)
set(FFMPEG_INCLUDE_DIR ${CMAKE_SYSROOT}/usr/include) 

target_include_directories(lvglTest PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lvgl
    ${CMAKE_SOURCE_DIR}/lv_drivers
    ${CMAKE_SOURCE_DIR}/lvgl/src/extra/libs/ffmpeg
    ${CMAKE_SOURCE_DIR}/include  # 修正了 //include 的写法
    ${FFMPEG_INCLUDE_DIR}        # 现在这个变量有定义了
)

# --- 链接库 ---
# 注意：库的顺序很重要，依赖别人的库要放在前面
target_link_libraries(lvglTest PRIVATE
    # FFmpeg 库 (如果未启用 FFmpeg 功能，请注释掉这些)
    avcodec
    avformat
    avutil
    swscale
    swresample
    asound

    # 系统基础库
    pthread 
    rt
    m  # 数学库，LVGL 必须
)