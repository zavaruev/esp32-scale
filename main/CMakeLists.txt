# Generated headers are real build steps, not configure-time side effects.
#
# The web page used to be embedded with execute_process(), which runs only when
# CMake configures. On an incremental build the HTML changed, the build reported
# success, and the device kept serving the old page - indistinguishable from a
# browser cache problem. add_custom_command() with the header as OUTPUT fixes
# that.
set(WEB_HTML  "${CMAKE_CURRENT_LIST_DIR}/web/index.html")
set(WEB_HEADER "${CMAKE_CURRENT_LIST_DIR}/index_html.h")

# IDF evaluates this file twice: once for the real build and once through
# "cmake -P" while scanning component requirements. add_custom_command is not
# permitted in script mode, hence the guard - CMake documents CMAKE_SCRIPT_MODE_FILE
# as set exactly when the -P form is used.
if(NOT DEFINED CMAKE_SCRIPT_MODE_FILE)
    add_custom_command(
        OUTPUT  "${WEB_HEADER}"
        COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../tools/embed_web.py"
        DEPENDS "${WEB_HTML}" "${CMAKE_CURRENT_LIST_DIR}/../tools/embed_web.py"
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
        COMMENT "Embedding main/web/index.html"
        VERBATIM
    )
endif()

idf_component_register(
    SRCS "scale_app.c"
         "scale_filter.c"
         "weight_task.c"
         "web_server.c"
         "ota.c"
         "hx711.c"
         "st7789.c"
         "buttons.c"
         "audio.c"
         "ui.c"
         "mcp_server.c"
         "${WEB_HEADER}"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio
             esp_driver_spi
             esp_lcd
             esp_lvgl_port
             lvgl
             esp_timer
             nvs_flash
             esp_wifi
             esp_netif
             esp_event
             esp_http_server
             esp_http_client
             app_update
             esp_partition
             bootloader_support)
