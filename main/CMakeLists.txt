idf_component_register(
    SRCS "main.c"
         "app_cfg.c"
         "app_log.c"
         "radio.c"
         "beacon_adv.c"
         "beacon_scan.c"
         "net_ap.c"
         "web_server.c"
         "storage.c"
         "status_led.c"
         "btn.c"
    INCLUDE_DIRS "."
    EMBED_FILES "www/index.html.gz"
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer bt json driver fatfs wear_levelling
)

# Board preset: idf.py -DAPP_BOARD=2 build   (1=DevKitC-1, 2=XIAO C6, 3=Waveshare Zero)
if(DEFINED APP_BOARD)
    message(STATUS "board preset: APP_BOARD=${APP_BOARD}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_ID=${APP_BOARD})
endif()

# ETag of the embedded SPA (see the root CMakeLists.txt), used for conditional GETs.
if(DEFINED BGW_UI_ETAG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_UI_ETAG="${BGW_UI_ETAG}")
endif()
