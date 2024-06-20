idf_component_register(SRCS "main.c" "file_server.c"  "ota_by_sockets.c" "esp_user_log_redirection.c"  "key_data.c" "wifi_ap_sta.c" "user_mdns.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES "lorawan_license.html" "favicon.ico" "firmware_update.html"  wifi_conf.html
                    )
