# The embedded web page must be regenerated whenever main/web/index.html changes,
# not merely when CMake reconfigures.
#
# The first version used execute_process(), which runs at configure time only. On
# an incremental build it silently did nothing: the HTML was edited, the build
# reported success, and the device kept serving the previous page. That is the
# worst possible failure - it looks like a caching problem in the browser.
#
# add_custom_command with the header as OUTPUT and the HTML as DEPENDS makes it a
# real build step. Changing the header then also changes web_server.c's input, so
# the component is recompiled. The generated header stays checked in so a fresh
# clone builds without Python.
set(WEB_HTML  "${CMAKE_CURRENT_LIST_DIR}/web/index.html")
set(WEB_HEADER "${CMAKE_CURRENT_LIST_DIR}/index_html.h")

# IDF evaluates this file twice: once for the real build and once through
# "cmake -P" while scanning for component requirements. add_custom_command is not
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
         "${WEB_HEADER}"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio
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