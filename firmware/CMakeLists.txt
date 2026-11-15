# Dispenser V2 firmware (ESP-IDF project, target esp32c3).
# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly.
#
# The application version comes from version.txt in this directory (read by
# ESP-IDF). Without it ESP-IDF would fall back to `git describe`, which reports
# the Android release tags of this monorepo.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dispenser)
