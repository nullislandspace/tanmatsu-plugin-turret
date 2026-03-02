cmake_minimum_required(VERSION 3.16)
set(PLUGIN_NAME "turrent")
set(TANMATSU_PLUGIN_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../tanmatsu-launcher/tools/plugin-sdk")
project(${PLUGIN_NAME} C)
include(${TANMATSU_PLUGIN_SDK}/plugin-build.cmake)

set(PLUGIN_SOURCES
    src/main.c
    src/audio.c
)

build_tanmatsu_plugin(${PLUGIN_NAME} "${PLUGIN_SOURCES}")
