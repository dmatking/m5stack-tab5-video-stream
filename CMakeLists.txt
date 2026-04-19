# Required CMake Version
cmake_minimum_required(VERSION 3.16)

# WiFi credentials from ~/.esp_creds (WIFI_SSID, WIFI_PASS).
# Must be set before include(project.cmake) — IDF reads SDKCONFIG_DEFAULTS early.
# sdkconfig.defaults must be listed explicitly when appending; IDF drops the
# automatic fallback otherwise.
list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "$ENV{HOME}/.esp_creds")
    list(APPEND SDKCONFIG_DEFAULTS "$ENV{HOME}/.esp_creds")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(m5stack-tab5-video-stream)

