/* MDNS-SD Query and advertise Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif_ip_addr.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "mdns.h"
#include "driver/gpio.h"
#include "netdb.h"
#include "user_mdns.h"
#include "esp_mac.h"
static const char * TAG = "user_mdns";
static char * generate_hostname(void);

void user_mdns_init(uint16_t ota_port,uint16_t log_remote_port,bool auth )
{
    char * hostname = generate_hostname();
    char udp_log_port[16];
    sprintf(udp_log_port,"%d",log_remote_port);
    //initialize mDNS
    ESP_ERROR_CHECK( mdns_init() );
    //set mDNS hostname (required if you want to advertise services)
    ESP_ERROR_CHECK( mdns_hostname_set(hostname) );
    ESP_LOGI(TAG, "mdns hostname set to: [%s]", hostname);

    mdns_txt_item_t arduTxtData[5] = {
        {(char*)"board"         ,(char*)"wifi bootloader"},
        {(char*)"tcp_check"     ,(char*)"no"},
        {(char*)"ssh_upload"    ,(char*)"no"},
        {(char*)"auth_upload"   ,(char*)"no"},
        {(char*)"udp_port"   ,(char*)"12345"}
    };

    if(mdns_service_add(NULL, "_arduino", "_tcp", ota_port, arduTxtData, 5)) 
    {
        ESP_LOGE(TAG,"Failed adding Arduino service");
    }
    if(mdns_service_txt_item_set("_arduino", "_tcp", "udp_port", udp_log_port))
    {
        ESP_LOGE(TAG,"Failed setting Arduino txt item");
    }
    if(auth && mdns_service_txt_item_set("_arduino", "_tcp", "auth_upload", "yes"))
    {
        ESP_LOGE(TAG,"Failed setting Arduino txt item");
    }
    free(hostname);
}

/** Generate host name based on sdkconfig, optionally adding a portion of MAC address to it.
 *  @return host name string allocated from the heap
 */
static char* generate_hostname(void)
{
    uint8_t mac[6];
    char   *hostname;
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (-1 == asprintf(&hostname, "%s-%02X%02X%02X", "esp32-s3", mac[3], mac[4], mac[5])) {
        abort();
    }
    return hostname;
}