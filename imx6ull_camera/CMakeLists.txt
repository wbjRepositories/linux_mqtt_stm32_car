cmake_minimum_required(VERSION 3.10)
project(camera)

# 显式追加编译参数 (强制加上 -O0 和 -g，双重保险)
set(CMAKE_BUILD_TYPE "Debug")
set(CMAKE_C_FLAGS_DEBUG "$ENV{CFLAGS} -O0 -g -ggdb -Wall")

# 配置 ARM 交叉编译
set(CMAKE_SYSTEM_NAME Linux) #设置目标系统名字
set(CMAKE_SYSTEM_PROCESSOR arm) #设置目标处理器架构
set(TOOLCHAIN_DIR /home/book/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/arm-buildroot-linux-gnueabihf/sysroot)
set(CMAKE_SYSROOT ${TOOLCHAIN_DIR}/)
# 指定交叉编译器 arm-gcc 和 arm-g++
set(CMAKE_C_COMPILER /home/book/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/bin/arm-buildroot-linux-gnueabihf-gcc)

# 为编译器添加编译选项
set(CMAKE_C_FLAGS "-march=armv7ve -mfpu=neon -mfloat-abi=hard -mcpu=cortex-a7")
set(CMAKE_CXX_FLAGS "-march=armv7ve -mfpu=neon -mfloat-abi=hard -mcpu=cortex-a7")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

add_executable(camera camera.c)

target_link_libraries(camera PRIVATE rt jpeg pthread)