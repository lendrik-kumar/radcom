idf_component_register(
    SRCS
        "main.c"
        "packet.c"
        "radio.c"
        "router.c"
        "wifi_ap.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_http_server
        esp_netif
        esp_event
        driver
        spi_flash
        lwip
        json
        esp_timer
    EMBED_FILES
        "../client_dist/index.html.gz"
)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/../client_dist/index.html.gz"
    COMMAND python3 build_ui.py
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

add_custom_target(build_ui ALL DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../client_dist/index.html.gz")
add_dependencies(${COMPONENT_LIB} build_ui)
