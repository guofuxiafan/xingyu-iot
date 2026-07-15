set(srcs "simple_video_server_example.c"
         "camera_source.c"
         "ws_streamer.c"
         "wifi_cred_store.c"
         "remote_log.c"
         "provisioning_button.c"
         "dns_server.c"
         "provisioning_manager.c"
         "sta_connect.c")
set(html_files "../frontend/gzipped/index.html.gz"
               "../frontend/gzipped/loading.jpg.gz"
               "../frontend/gzipped/favicon.ico.gz"
               "../frontend/gzipped/assets/index.js.gz"
               "../frontend/gzipped/assets/index.css.gz")

idf_component_register(SRCS "${srcs}"
                       PRIV_INCLUDE_DIRS .
                        PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_http_server esp_event esp_driver_gpio
                                      esp_mm esp_timer json lwip mdns esp_websocket_client example_video_common
                       EMBED_TXTFILES ${html_files}
                       EMBED_FILES root.html)
