# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# 固件版本号：OTA 比对用（esp_app_desc->version）
# 发布新版时把这里 +1，重新编译，上传 bin 到智控台
set(PROJECT_VER "1.0.1")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ai-esp32s3)

add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>")
