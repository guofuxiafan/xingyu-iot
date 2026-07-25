set(srcs "simple_video_server_example.c"
         "camera_source.c"
         "ws_streamer.c"
         "wifi_cred_store.c"
         "remote_log.c"
         "provisioning_button.c"
         "dns_server.c"
         "provisioning_manager.c"
         "sta_connect.c"
         "voice_output.c")
set(html_files "../frontend/gzipped/index.html.gz"
               "../frontend/gzipped/loading.jpg.gz"
               "../frontend/gzipped/favicon.ico.gz"
               "../frontend/gzipped/assets/index.js.gz"
               "../frontend/gzipped/assets/index.css.gz")

idf_component_register(SRCS "${srcs}"
                       PRIV_INCLUDE_DIRS .
                        PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_http_server esp_event esp_driver_gpio
                                      esp_mm esp_timer json lwip mdns esp_websocket_client example_video_common
                                      esp_driver_i2s esp_driver_i2c spiffs esp_partition esp_codec_dev esp-sr
                       EMBED_TXTFILES ${html_files}
                       EMBED_FILES root.html)

if(CONFIG_EXAMPLE_VOICE_OUTPUT_ENABLE)
    spiffs_create_partition_image(storage ../data FLASH_IN_PROJECT)

    idf_build_get_property(build_components BUILD_COMPONENTS)
    if(esp-sr IN_LIST build_components)
        set(esp_sr_component esp-sr)
    else()
        set(esp_sr_component espressif__esp-sr)
    endif()
    idf_component_get_property(esp_sr_path ${esp_sr_component} COMPONENT_DIR)
    set(voice_data_image
        ${esp_sr_path}/esp-tts/esp_tts_chinese/esp_tts_voice_data_xiaoxin_small.dat)
    add_custom_target(voice_data ALL DEPENDS ${voice_data_image})
    add_dependencies(flash voice_data)
    partition_table_get_partition_info(voice_size "--partition-name voice_data" "size")
    partition_table_get_partition_info(voice_offset "--partition-name voice_data" "offset")
    if("${voice_size}" AND "${voice_offset}")
        esptool_py_flash_to_partition(flash "voice_data" "${voice_data_image}")
    else()
        message(FATAL_ERROR "voice_data partition is required for offline TTS")
    endif()
endif()
