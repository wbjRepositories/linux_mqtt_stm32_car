cmake_minimum_required(VERSION 3.10)


# 显式追加编译参数 (强制加上 -O0 和 -g，双重保险)
set(CMAKE_BUILD_TYPE "Debug")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -g -ggdb -fno-omit-frame-pointer -funwind-tables")

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

project(camera)
# --- 添加 FFmpeg 库支持 ---

# 定义您需要的 FFmpeg 库组件
# 常用顺序：avformat -> avcodec -> swresample -> swscale -> avutil
# avutil 是其他库的基础，通常放在最后或显式指定依赖
set(FFMPEG_COMPONENTS
    avformat
    avcodec
    swresample
    swscale
    avutil
    avdevice
    # 根据您的需求，可能还需要以下库：make
    # avdevice # 如果需要访问硬件设备，如V4L2，则取消注释
    # postproc # 如果需要后处理，通常不需要
)

# 查找 FFmpeg 库
set(FFMPEG_LIBRARIES "")
foreach(component IN LISTS FFMPEG_COMPONENTS)
    find_library(FFMPEG_${component}_LIBRARY
        NAMES ${component}
        HINTS
            ${CMAKE_SYSROOT}/usr/lib
            ${CMAKE_SYSROOT}/lib # 有些系统可能在 /lib
        NO_DEFAULT_PATH # 确保只在 HINTS 指定的路径中查找，不查找系统默认路径（宿主系统）
    )
    if(NOT FFMPEG_${component}_LIBRARY)
        message(FATAL_ERROR "Could not find FFmpeg library: ${component}. Please ensure FFmpeg is cross-compiled and installed in your sysroot (${CMAKE_SYSROOT}/usr/lib).")
    endif()
    list(APPEND FFMPEG_LIBRARIES "${FFMPEG_${component}_LIBRARY}")
endforeach()

# 查找 FFmpeg 头文件路径
# 假设 FFmpeg 头文件位于 ${CMAKE_SYSROOT}/usr/include/
# 例如：${CMAKE_SYSROOT}/usr/include/libavcodec/avcodec.h
#       ${CMAKE_SYSROOT}/usr/include/libavutil/avutil.h
find_path(FFMPEG_INCLUDE_DIR
    NAMES libavdevice/avdevice.h libavcodec/avcodec.h libavformat/avformat.h libavutil/avutil.h # 查找其中一个存在即可
    HINTS
        ${CMAKE_SYSROOT}/usr/include
    NO_DEFAULT_PATH # 确保只在 HINTS 指定的路径中查找，不查找系统默认路径（宿主系统）
)
if(NOT FFMPEG_INCLUDE_DIR)
    message(FATAL_ERROR "Could not find FFmpeg include directory (e.g., libavcodec/avcodec.h). Please ensure FFmpeg headers are installed in your sysroot (${CMAKE_SYSROOT}/usr/include).")
endif()

message(STATUS "Found FFmpeg Libraries: ${FFMPEG_LIBRARIES}")
message(STATUS "Found FFmpeg Include Dir: ${FFMPEG_INCLUDE_DIR}")

# --- 构建可执行文件 ---
add_executable(camera camera_ffmpeg.c)

# 添加 FFmpeg 头文件路径
target_include_directories(camera PRIVATE
    ${FFMPEG_INCLUDE_DIR}
)

# 链接 FFmpeg 库以及其他需要的库
target_link_libraries(camera PRIVATE
    rt
    jpeg
    pthread
    ${FFMPEG_LIBRARIES} # 添加 FFmpeg 库
)