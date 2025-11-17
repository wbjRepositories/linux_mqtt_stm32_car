cmake_minimum_required(VERSION 3.10.2)
project(MQTTClient C)
message(STATUS "CMake version: " ${CMAKE_VERSION})
message(STATUS "CMake system name: " ${CMAKE_SYSTEM_NAME})
message(STATUS "CMake system processor: " ${CMAKE_SYSTEM_PROCESSOR})

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)




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




add_executable(mqttClient mqttClient.c)

#target_include_directories(mqttClient PRIVATE /home/book/tools/paho.mqtt.c-1.3.8/install/include)
#target_include_directories(mqttClient PRIVATE ${TOOLCHAIN_DIR}/lib)
target_link_libraries(mqttClient PRIVATE paho-mqtt3c pthread rt)
