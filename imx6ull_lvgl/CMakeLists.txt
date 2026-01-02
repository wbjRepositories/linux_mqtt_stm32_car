cmake_minimum_required(VERSION 3.10)


# 显式追加编译参数 (强制加上 -O0 和 -g，双重保险)
set(CMAKE_BUILD_TYPE "Debug")
set(CMAKE_C_FLAGS_DEBUG "$ENV{CFLAGS} -O0 -g -ggdb -Wall")

set(EXECUTABLE_OUTPUT_PATH ../bin)
# 配置 ARM 交叉编译
set(CMAKE_SYSTEM_NAME Linux) #设置目标系统名字
set(CMAKE_SYSTEM_PROCESSOR arm) #设置目标处理器架构
set(TOOLCHAIN_DIR /home/wbj/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/arm-buildroot-linux-gnueabihf/sysroot)
set(CMAKE_SYSROOT ${TOOLCHAIN_DIR}/)
# 指定交叉编译器 arm-gcc 和 arm-g++
set(CMAKE_C_COMPILER /home/wbj/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/bin/arm-buildroot-linux-gnueabihf-gcc)

# 为编译器添加编译选项
set(CMAKE_C_FLAGS "-march=armv7ve -mfpu=neon -mfloat-abi=hard -mcpu=cortex-a7")
set(CMAKE_CXX_FLAGS "-march=armv7ve -mfpu=neon -mfloat-abi=hard -mcpu=cortex-a7")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

project(project1)


file(GLOB_RECURSE LVGL_SOURCES ${CMAKE_SOURCE_DIR}/lvgl/src/*.c)

# lv_drivers/display
file(GLOB LV_DRIVERS_DISPLAY_SOURCES ${CMAKE_SOURCE_DIR}/lv_drivers/display/*.c)

# lv_drivers/input
file(GLOB LV_DRIVERS_INPUT_SOURCES ${CMAKE_SOURCE_DIR}/lv_drivers/indev/*.c)

# 合并所有源文件
set(ALL_SOURCES
    src/main.c
    ${LVGL_SOURCES}
    ${LV_DRIVERS_DISPLAY_SOURCES}
    ${LV_DRIVERS_INPUT_SOURCES}
)

add_executable(lvglTest ${ALL_SOURCES})


# include 目录
target_include_directories(lvglTest PRIVATE
    ${CMAKE_SOURCE_DIR}           # lv_conf.h
    ${CMAKE_SOURCE_DIR}/lvgl      # lvgl.h
    ${CMAKE_SOURCE_DIR}/lv_drivers
    include
)

# 查找 pthread 库
#find_package(Threads REQUIRED)

# 链接 pthread 库到可执行文件
#target_link_libraries(lvglTest PRIVATE Threads::Threads rt)

target_link_libraries(lvglTest PRIVATE pthread rt)
