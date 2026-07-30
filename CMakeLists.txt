# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Cấu hình số phiên bản thiết bị tại đây (thay đổi theo nhu cầu của bạn):
set(PROJECT_VER "2.0.2")

project(RFID)
