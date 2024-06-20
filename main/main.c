#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "wifi_conf_server.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "key_data.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/inet.h"
#include "lwip/lwip_napt.h"
#include "wifi_ap_sta.h"
#include "user_mdns.h"
#include "ota_by_sockets.h"
#include "esp_random.h"
#include "esp_user_log_redirection.h"
#include "file_server.h"
#include "driver/gpio.h"

// char pbuffer[2048];
// void esp_print_tasks(void)
// {
//     printf("--------------- heap:%lu ---------------------\r\n", esp_get_free_heap_size());
//     vTaskList(pbuffer);
//     printf("%s", pbuffer);
//     printf("----------------------------------------------\r\n");
// }

static const char *TAG = "Heltec WirelessBoot Server";
#define LED_1_PIN GPIO_NUM_33
#define LED_2_PIN GPIO_NUM_34
void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) 
    {
        // 1.OTA app partition table has a smaller NVS partition size than the non-OTA
        // partition table. This size mismatch may cause NVS initialization to fail.
        // 2.NVS partition contains data in new format and cannot be recognized by this version of code.
        // If this happens, we erase NVS partition and initialize NVS again.
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_ap_sta_init();

    gpio_set_direction(LED_1_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_2_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_1_PIN, 1);
    gpio_set_level(LED_2_PIN, 1);
    // ESP_ERROR_CHECK(wifi_conf_server());
    ESP_ERROR_CHECK(start_ota_file_server());
    ESP_LOGI(TAG, "wifi conf server started");

    ota_init(3232,false);

    // uint16_t log_remote_port =  10000+esp_random()%10000;
    uint16_t log_remote_port = 12345; // use fixed 
    user_mdns_init(3232,log_remote_port,false );
    esp_log_redirection_config_t log_config = {
        .log_level_uart = ESP_LOG_INFO,
        .log_level_udp = ESP_LOG_INFO
    };
    esp_log_redirection_init(&log_config,log_remote_port);

    while(1)
    {
        uint32_t val=(uint32_t)((esp_timer_get_time()/1000)& 0xFFFFFFFF);
        ESP_LOGI(TAG, "Log testing%ld %ld",val,esp_log_timestamp());
        ESP_LOGW(TAG, "Log testing");
        ESP_LOGE(TAG, "Log testing");
        // esp_print_tasks();
        vTaskDelay(6000/portTICK_PERIOD_MS);
    }
}