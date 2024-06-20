#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "mbedtls/md5.h"
#include "esp_ota_ops.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
static void tcp_client_task(void *pvParameters);

#define U_FLASH   0
#define U_SPIFFS  100
#define U_AUTH    200

typedef struct 
{
    uint8_t cmd;
    uint32_t ota_ip;
    uint16_t ota_port;
    uint32_t ota_file_size;
    char ota_file_md5[36];
}ota_info_t;

typedef enum 
{
  OTA_IDLE,
  OTA_WAITAUTH,
  OTA_RUNUPDATE
} ota_state_t;

#define PORT 3232

QueueHandle_t udp_to_tcp_ota_info_queue;
#define UDP_TO_TCP_OTA_INFO_QUEUE_SIZE (sizeof(ota_info_t))
#define UDP_TO_TCP_OTA_INFO_QUEUE_NUM  1

static const char *TAG = "ota";
static uint16_t g_ota_port;
static bool g_auth;
static void udp_ota_data_handle(char* raw_data,ota_info_t *ota_info)
{
	sscanf(raw_data, "%hhd %hd %ld %s", &ota_info->cmd,&ota_info->ota_port,&ota_info->ota_file_size,ota_info->ota_file_md5);
}

static void udp_server_task(void *pvParameters)
{
    char tx_buffer[128];
    char rx_buffer[128];
    char addr_str[128];
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    struct sockaddr_in dest_addr;
    ota_info_t udp_server_ota_info;
    ota_state_t udp_server_ota_state = OTA_IDLE;
    while (1) {

        if (addr_family == AF_INET) {
            struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
            dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
            dest_addr_ip4->sin_family = AF_INET;
            dest_addr_ip4->sin_port = htons(PORT);
            ip_protocol = IPPROTO_IP;
        }

        int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (sock < 0) 
        {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket created");

#if defined(CONFIG_LWIP_NETBUF_RECVINFO) && !defined(CONFIG_EXAMPLE_IPV6)
        int enable = 1;
        lwip_setsockopt(sock, IPPROTO_IP, IP_PKTINFO, &enable, sizeof(enable));
#endif

        int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        }
        // ESP_LOGI(TAG, "Socket bound, port %d", PORT);

        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t socklen = sizeof(source_addr);

#if defined(CONFIG_LWIP_NETBUF_RECVINFO) && !defined(CONFIG_EXAMPLE_IPV6)
        struct iovec iov;
        struct msghdr msg;
        struct cmsghdr *cmsgtmp;
        u8_t cmsg_buf[CMSG_SPACE(sizeof(struct in_pktinfo))];

        iov.iov_base = rx_buffer;
        iov.iov_len = sizeof(rx_buffer);
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);
        msg.msg_flags = 0;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_name = (struct sockaddr *)&source_addr;
        msg.msg_namelen = socklen;
#endif

        while (1) {
            ESP_LOGI(TAG, "Waiting for data");
#if defined(CONFIG_LWIP_NETBUF_RECVINFO) && !defined(CONFIG_EXAMPLE_IPV6)
            int len = recvmsg(sock, &msg, 0);
#else
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
#endif
            // Error occurred during receiving
            if (len < 0) {
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                break;
            }
            // Data received
            else {
                // Get the sender's ip address as string
                if (source_addr.ss_family == PF_INET) 
                {
                    inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
#if defined(CONFIG_LWIP_NETBUF_RECVINFO) && !defined(CONFIG_EXAMPLE_IPV6)
                    for ( cmsgtmp = CMSG_FIRSTHDR(&msg); cmsgtmp != NULL; cmsgtmp = CMSG_NXTHDR(&msg, cmsgtmp) ) 
                    {
                        if ( cmsgtmp->cmsg_level == IPPROTO_IP && cmsgtmp->cmsg_type == IP_PKTINFO ) 
                        {
                            struct in_pktinfo *pktinfo;
                            pktinfo = (struct in_pktinfo*)CMSG_DATA(cmsgtmp);
                            ESP_LOGI(TAG, "dest ip: %s\n", inet_ntoa(pktinfo->ipi_addr));
                        }
                    }
#endif
                }

                rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string...
                // ESP_LOGI(TAG, "%s", rx_buffer);

                switch (udp_server_ota_state)
                {
                    case OTA_IDLE:
                    {
                        udp_ota_data_handle(rx_buffer,&udp_server_ota_info);
                        udp_server_ota_info.ota_ip = inet_addr(addr_str);
                        if(g_auth ==true)  
                        {
                            mbedtls_md5_context md5_ctx;
                            unsigned char encrypt[32];
                            unsigned char decrypt[64];
                            sprintf((char*)encrypt ,"%lld",esp_timer_get_time());
                            mbedtls_md5_init(&md5_ctx);
                            mbedtls_md5_starts(&md5_ctx);
                            mbedtls_md5_update(&md5_ctx, encrypt, strlen((char *)encrypt));
                            mbedtls_md5_finish(&md5_ctx, decrypt);

                            // printf("MD5加密前:[%s]\n", encrypt);
                            // printf("MD5加密后(32位):");
                            uint8_t len_offset;
                            len_offset = sprintf(tx_buffer ,"AUTH ");
                            for(int i = 0; i < 16; i++)
                            {
                                // printf("%02x", decrypt[i]);
                                len_offset += sprintf(tx_buffer+len_offset ,"%02x",decrypt[i]);
                            }
                            // printf("\r\n");
                            mbedtls_md5_free(&md5_ctx);
                            // sprintf(tx_buffer ,"AUTH %s",decrypt);
                            // printf("%s",tx_buffer);
                            int err = sendto(sock, tx_buffer, strlen(tx_buffer), 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
                            if (err < 0)
                            {
                                ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                                break;
                            }
                            udp_server_ota_state = OTA_WAITAUTH;
                        }
                        else
                        {
                            sprintf(tx_buffer ,"OK");
                            int err = sendto(sock, tx_buffer, strlen(tx_buffer), 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
                            if (err < 0)
                            {
                                ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                                break;
                            }
                            xQueueOverwrite(udp_to_tcp_ota_info_queue,(void*)(&udp_server_ota_info));
                            tcp_client_task(NULL);
                            udp_server_ota_state = OTA_RUNUPDATE;
                        }            
                        break;
                    }
                    case OTA_WAITAUTH:
                    {
                        udp_server_ota_state = OTA_IDLE;
                        break;
                    }
                    case OTA_RUNUPDATE:
                    {
                        vTaskDelay(1*100/portTICK_PERIOD_MS);
                        udp_server_ota_state = OTA_IDLE;
                        break;
                    }
                    default:
                        break;
                }
                // ESP_LOGI(TAG, "Received %d bytes from %s: %d", len, addr_str, udp_server_ota_info.ota_ip);
            }
        }

        if (sock != -1) {
            ESP_LOGE(TAG, "Shutting down socket and restarting...");
            shutdown(sock, 0);
            close(sock);
        }
    }
    vTaskDelete(NULL);
}

static void tcp_client_task(void *pvParameters)
{
    char rx_buffer[1461];
    char tx_buffer[128];
    char host_ip[] = "1";
    int addr_family = 0;
    int ip_protocol = 0;
    ota_info_t tcp_ota_info;
    uint32_t receive_len;
    mbedtls_md5_context md5_ctx;
    unsigned char md5_decrypt[32];
    esp_ota_handle_t update_handle = 0 ;
    const esp_partition_t *update_partition = NULL;
    esp_err_t err;
    // while (1)
    {
        if(xQueueReceive(udp_to_tcp_ota_info_queue, &tcp_ota_info ,portMAX_DELAY))
        {
            const esp_partition_t *flashApp_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_1,"flashApp");
            update_partition = esp_ota_get_next_update_partition(flashApp_partition);
            assert(update_partition != NULL);
            ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
                (int)update_partition->subtype, (unsigned int)update_partition->address);
            err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
                esp_ota_abort(update_handle);
                // continue;
                return;
            }
            receive_len = 0;
            mbedtls_md5_init(&md5_ctx);
            mbedtls_md5_starts(&md5_ctx);

            struct sockaddr_in dest_addr;
            dest_addr.sin_addr.s_addr = tcp_ota_info.ota_ip;
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(tcp_ota_info.ota_port);
            addr_family = AF_INET;
            ip_protocol = IPPROTO_IP;

            int sock =  socket(addr_family, SOCK_STREAM, ip_protocol);
            if (sock < 0) 
            {
                ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
                // break;
                return;
            }
            ESP_LOGI(TAG, "Socket created, connecting to %s:%d", host_ip, (int)PORT);

            int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(struct sockaddr_in));
            if (err != 0)
            {
                ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
                // break;
                return;
            }
            ESP_LOGI(TAG, "Successfully connected");
           

            while (1)
            {
                int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
                // Error occurred during receiving
                if (len < 0) {
                    ESP_LOGE(TAG, "recv failed: errno %d", errno);
                    break;
                }
                // Data received
                else {
                    rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string
                    ESP_LOGI(TAG, "Received %d all %d:%d", (int)len, (int)receive_len,(int)tcp_ota_info.ota_file_size);
                    err = esp_ota_write( update_handle, (const void *)rx_buffer, len);
                    if (err != ESP_OK)
                    {
                        esp_ota_abort(update_handle);
                        break;
                    }
                    mbedtls_md5_update(&md5_ctx, (unsigned char*)rx_buffer,len);
                }
                sprintf(tx_buffer,"%u",len);
                int err = send(sock, tx_buffer, strlen(tx_buffer), 0);
                if (err < 0) 
                {
                    ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                    break;
                }
                receive_len += len;
                if(receive_len == tcp_ota_info.ota_file_size)
                {
                    sprintf(tx_buffer,"OK");
                    int err = send(sock, tx_buffer, strlen(tx_buffer), 0);
                    if (err < 0) 
                    {
                        ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                        break;
                    }
                    break;
                }
            }
            err = esp_ota_end(update_handle);
            if (err != ESP_OK) 
            {
                if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
                    ESP_LOGE(TAG, "Image validation failed, image is corrupted");
                } else {
                    ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
                }
            }
            if (sock != -1) {
                ESP_LOGE(TAG, "Shutting down socket and restarting...");
                shutdown(sock, 0);
                close(sock);
            }

            mbedtls_md5_finish(&md5_ctx, md5_decrypt);
            // printf("MD5加密后(32位):");
            uint8_t len_offset=0;
            char md5_check[64];
            for(int i = 0; i < 16; i++)
            {
                // printf("%02x", md5_decrypt[i]);
                len_offset += sprintf(md5_check+len_offset ,"%02x",(uint8_t)md5_decrypt[i]);
            }
            mbedtls_md5_free(&md5_ctx);
            // printf("\r\n ota_file_md5 %s\r\n",tcp_ota_info.ota_file_md5);
            if(strncmp(md5_check,tcp_ota_info.ota_file_md5, strlen(md5_check)) ==0)
            {
                printf("MD5 check success\r\n");
                err = esp_ota_set_boot_partition(update_partition);
                if (err != ESP_OK) 
                {
                    ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
                }
                esp_restart();
            }
        }
    }
    // vTaskDelete(NULL);
}


void ota_init(uint16_t ota_port,bool auth )
{
    g_ota_port = ota_port;
    g_auth = auth;
    udp_to_tcp_ota_info_queue= xQueueCreate(UDP_TO_TCP_OTA_INFO_QUEUE_NUM, UDP_TO_TCP_OTA_INFO_QUEUE_SIZE);
	if( udp_to_tcp_ota_info_queue == 0 ) 	
    {
        ESP_LOGE(TAG,"failed to create queue1= %p ",udp_to_tcp_ota_info_queue);
    }
    xTaskCreatePinnedToCore(udp_server_task, "udp_server", 20480, (void*)AF_INET, configMAX_PRIORITIES-2, NULL,1);
    // xTaskCreatePinnedToCore(tcp_client_task, "tcp_client", 10240, NULL, 6, NULL,1);
}