#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/inet.h"
#include "lwip/lwip_napt.h"
#include "freertos/event_groups.h"
#include "key_data.h"
#include "wifi_ap_sta.h"
#include "esp_mac.h"

#define EXAMPLE_ESP_WIFI_SSID      "WirelessBoot"
#define EXAMPLE_ESP_WIFI_PASS      "heltec.org"
#define EXAMPLE_ESP_WIFI_CHANNEL   1
#define EXAMPLE_MAX_STA_CONN       2

static const char *TAG = "AP_STA";

/****************************************************sta****************************/
static void wifi_sta_connect_handle(void *pvParameters);
/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
#define ESP_MAXIMUM_RETRY (5)

QueueHandle_t wifi_user_wifi_conf_queue;
#define WIFI_USER_WIFI_CONF_QUEUE_SIZE (sizeof(wifi_user_conf_t))
#define WIFI_USER_WIFI_CONF_QUEUE_NUM  1

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGE(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));
    wifi_user_conf_t flash_wifi_conf;
    key_get_wifi_user_conf(&flash_wifi_conf,sizeof(wifi_user_conf_t));

    wifi_config_t wifi_sta_config = {
        .sta = {
            // .ssid = (unsigned char*)wifi_conf.ssid,
            // .password = (unsigned char*)wifi_conf.password,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    if(flash_wifi_conf.conf_valid != CONF_VALID_VAL)
    {
        sprintf(flash_wifi_conf.ssid,"myssid");
        sprintf(flash_wifi_conf.password,"mypassword");
        key_set_wifi_user_conf(&flash_wifi_conf,sizeof(wifi_user_conf_t));
    }
    memcpy(wifi_sta_config.sta.ssid,flash_wifi_conf.ssid,strlen(flash_wifi_conf.ssid)+1);
    memcpy(wifi_sta_config.sta.password,flash_wifi_conf.password,strlen(flash_wifi_conf.password)+1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config) );

    wifi_user_wifi_conf_queue= xQueueCreate(WIFI_USER_WIFI_CONF_QUEUE_NUM, WIFI_USER_WIFI_CONF_QUEUE_SIZE);
	if( wifi_user_wifi_conf_queue == 0 ) 	
    {
        ESP_LOGE(TAG,"failed to create queue1= %p ",wifi_user_wifi_conf_queue);
    }
    xTaskCreatePinnedToCore(&wifi_sta_connect_handle, "wifi_sta_connect_handle", 8*1024, NULL, configMAX_PRIORITIES -2, NULL,  0);  

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    // /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
    //  * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    // EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
    //         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
    //         pdFALSE,
    //         pdFALSE,
    //         portMAX_DELAY);

    // /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
    //  * happened. */
    // if (bits & WIFI_CONNECTED_BIT) {
    //     ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
    //              EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    // } else if (bits & WIFI_FAIL_BIT) {
    //     ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
    //              EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    // } else {
    //     ESP_LOGE(TAG, "UNEXPECTED EVENT");
    // }

    // /* The event will not be processed after unregister */
    // ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    // ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    // vEventGroupDelete(s_wifi_event_group);
}

static void wifi_sta_conf_update(wifi_user_conf_t conf)
{
    wifi_config_t wifi_sta_config = {
        .sta = {
            // .ssid = (unsigned char*)wifi_conf.ssid,
            // .password = (unsigned char*)wifi_conf.password,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    memcpy(wifi_sta_config.sta.ssid,conf.ssid,strlen(conf.ssid)+1);
    memcpy(wifi_sta_config.sta.password,conf.password,strlen(conf.password)+1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config) );
    esp_wifi_connect();
}

static void wifi_sta_connect_handle(void *pvParameters)
{   
    wifi_user_conf_t wifi_conf;

    while (1)
    {
        if(xQueueReceive(wifi_user_wifi_conf_queue, &wifi_conf ,portMAX_DELAY))
        {
            ESP_LOGE(TAG, "wifi reconnect");
            wifi_sta_conf_update(wifi_conf);
        }
    }
    
}
/****************************************************sta****************************/

/****************************************************ap****************************/
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

void wifi_init_softap(void)
{

    esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    char ssid_str[64];
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);                                                        
    sprintf(ssid_str,"%s_%02X%02X",EXAMPLE_ESP_WIFI_SSID,mac[5],mac[4]);
    wifi_config_t wifi_config = {
        .ap = {
            // .ssid = ssid_str,
            // .ssid_len = strlen(ssid_str),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    wifi_config.ap.ssid_len = strlen(ssid_str);
    memcpy(wifi_config.ap.ssid,ssid_str,strlen(ssid_str));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    // ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
    //          EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS, EXAMPLE_ESP_WIFI_CHANNEL);
}
/****************************************************ap****************************/

// static void wifi_set_ap_forward_gw(uint32_t ip_addr) 
// {
//     // Enable DNS (offer) for dhcp server
// 	dhcps_offer_t dhcps_dns_value = OFFER_DNS;
// 	dhcps_set_option_info(6, &dhcps_dns_value, sizeof(dhcps_dns_value));
// 	// // Set custom dns server address for dhcp server 默认跟随路由器 【推荐换成国内DNS】
//     ip_addr_t dnsserver;
//     dnsserver.u_addr.ip4.addr = htonl(ip_addr);
// 	dnsserver.type = IPADDR_TYPE_V4;
// 	dhcps_dns_setserver(&dnsserver);
// }

// static void wifi_ap_open_forward(uint32_t ip_addr) 
// {
// #if IP_NAPT
// 	// !!! 必须启动sta后再设置，不然ap无网络 !!! Set to ip address of softAP netif (Default is 192.168.4.1)
//     // u32_t napt_netif_ip = ip_addr;
//     u32_t napt_netif_ip = 0xC0A80401;
// 	ip_napt_enable(htonl(napt_netif_ip), 1);
// #endif
// }



void wifi_ap_sta_init(void)
{
    ESP_LOGI(TAG, "wifi_ap_sta_init");
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_init_softap();
    wifi_init_sta();
    ESP_ERROR_CHECK(esp_wifi_start());

    // vTaskDelay(5000);

    //     wifi_config_t wifi_config = {
    //     .sta = {
    //         .ssid = "TP-LINK_B8BC",
    //         .password = "heltec_test",
    //         /* Setting a password implies station will connect to all security modes including WEP/WPA.
    //          * However these modes are deprecated and not advisable to be used. Incase your Access point
    //          * doesn't support WPA2, these mode can be enabled by commenting below line */
	//      .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
    //     },
    // };
    // ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    // esp_wifi_connect();
}