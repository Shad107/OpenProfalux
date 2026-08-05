idf_component_register(
    SRCS
        "main.c"
        "keeloq.c"
        "profalux.c"
        "cc1101.c"
        "wifi_bridge.c"
        "mqtt_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        mqtt
        json
        driver
)
