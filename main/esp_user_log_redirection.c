#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "esp_user_log_redirection.h"
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
static const char *TAG  = "log_redirection";

#define UDP_LOG_PORT (32321)


uint16_t g_log_remote_port = UDP_LOG_PORT;
#define MDEBUG_LOG_TIMEOUT_MS              (1 * 1000)
#define LOG_REDIRECTION_QUEUE_NUM   (128)
#define LOG_REDIRECTION_QUEUE_SIZE  (sizeof(log_info_t))
static QueueHandle_t g_log_redirection_queue = NULL;
static bool g_log_init_flag                  = false;
static esp_log_redirection_config_t *g_log_config = NULL;
typedef struct log_info
{
    struct tm time;
    esp_log_level_t level;
    size_t size;
    char *data;
} log_info_t;

typedef struct 
{
    int sock;
    struct sockaddr_storage source_addr;
}udp_info_t;
static QueueHandle_t udp_info_queue = NULL;
#define UDP_INFO_QUEUE_NUM   (1)
#define UDP_INFO_QUEUE_SIZE  (sizeof(udp_info_t))



static esp_err_t esp_websocket_send_log(char *log_buff,uint16_t log_len,httpd_handle_t hd)
{
    esp_err_t ret = ESP_OK;
    // httpd_ws_frame_t ws_pkt;
    
    // memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    // ws_pkt.payload = (uint8_t *)log_buff;
    // ws_pkt.len = strlen(log_buff);
    // ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    // static size_t max_clients = CONFIG_LWIP_MAX_LISTENING_TCP;
    // size_t fds = max_clients;
    // int client_fds[max_clients];

    // ret = httpd_get_client_list(hd, &fds, client_fds);

    // if ((ret != ESP_OK) || (fds == 0))
    // {
    //     return ret;
    // }

    // for (int i = 0; i < fds; i++) 
    // {
    //     int client_info = httpd_ws_get_fd_info(hd, client_fds[i]);
    //     if (client_info == HTTPD_WS_CLIENT_WEBSOCKET) 
    //     {
    //         ret = httpd_ws_send_frame_async(hd, client_fds[i], &ws_pkt);
    //     }
    // }
    return ret;
}

static ssize_t esp_log_redirection_vprintf(const char *fmt, va_list vp)
{
    size_t log_size = 0;
    log_info_t log_info;
    time_t now = 0;

    time(&now);
    localtime_r(&now, &log_info.time);
    log_info.size  = log_size = vasprintf(&log_info.data, fmt, vp);
    log_info.level = ESP_LOG_NONE;

    if (log_info.size == 0 ) 
    {
        ESP_LOG_REDIRECTION_FREE(log_info.data);
        return 0;
    }

    if (log_info.size > 7) 
    {
        uint8_t log_level_index = (log_info.data[0] == '\033') ? 7 : 0;

        switch (log_info.data[log_level_index]) 
        {
            case 'E':
                log_info.level = ESP_LOG_ERROR;
                break;

            case 'W':
                log_info.level = ESP_LOG_WARN;
                break;

            case 'I':
                log_info.level = ESP_LOG_INFO;
                break;

            case 'D':
                log_info.level = ESP_LOG_DEBUG;
                break;

            case 'V':
                log_info.level = ESP_LOG_VERBOSE;
                break;

            default:
            {
                break;
            }
        }
    }

    if (log_info.level <= g_log_config->log_level_uart) 
    {
        vprintf(fmt, vp); /**< Write log data to uart */
    }

    if (!g_log_redirection_queue || xQueueSend(g_log_redirection_queue, &log_info, 0) == pdFALSE) 
    {
        ESP_LOG_REDIRECTION_FREE(log_info.data);
    }
    return log_size;
}

#if 0
static void esp_log_redirection_send_task(void *arg)
{
    log_info_t log_info;

    for (; g_log_config;)
    {
        if (xQueueReceive(g_log_redirection_queue, &log_info, pdMS_TO_TICKS(MDEBUG_LOG_TIMEOUT_MS)) != pdPASS) 
        {
            continue;
        }

        if (g_log_config->log_level_udp != ESP_LOG_NONE
                && log_info.level <= g_log_config->log_level_udp) 
        {
            esp_websocket_send_log(log_info.data,log_info.size,g_websocket_hd);
        // esp_qcloud_log_iothub_write(log_info.data, log_info.size, log_info.level, &log_info.time); /**< Write log data to iothub */
        }

        ESP_LOG_REDIRECTION_FREE(log_info.data);
    }

    vTaskDelete(NULL);
}
#endif



esp_err_t esp_log_redirection_deinit()
{
    if (g_log_init_flag) 
    {
        return ESP_FAIL;
    }

    for (log_info_t *log_data = NULL;
            xQueueReceive(g_log_redirection_queue, &log_data, 0);) 
    {
        ESP_LOG_REDIRECTION_FREE(log_data);
        ESP_LOG_REDIRECTION_FREE(log_data->data);
    }
    g_log_init_flag = false;
    ESP_LOG_REDIRECTION_FREE(g_log_config);

    return ESP_OK;
}

static void esp_log_redirection_send_task(void *pvParameters)
{        
    log_info_t log_info;
    udp_info_t udp_send_info;
    while (1) {
        if(xQueuePeek(udp_info_queue, (void*)(&udp_send_info) ,portMAX_DELAY))
        {
            if (xQueueReceive(g_log_redirection_queue, &log_info, pdMS_TO_TICKS(MDEBUG_LOG_TIMEOUT_MS)) != pdPASS) 
            {
                continue;
            }
            int err = sendto(udp_send_info.sock, log_info.data, log_info.size, 0, (struct sockaddr *)&(udp_send_info.source_addr), sizeof(udp_send_info.source_addr));
            if (err < 0) {
                ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                // break;
            }
        }
    }
        
    
    vTaskDelete(NULL);
}

static void esp_log_redirection_rec_task(void *pvParameters)
{
    char rx_buffer[128];
    char addr_str[128];
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    struct sockaddr_in dest_addr;
    udp_info_t udp_rec_info;

    while (1) {

        if (addr_family == AF_INET) {
            struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
            dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
            dest_addr_ip4->sin_family = AF_INET;
            dest_addr_ip4->sin_port = htons(g_log_remote_port);
            ip_protocol = IPPROTO_IP;
        } 

        int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket created");
#if defined(CONFIG_LWIP_NETBUF_RECVINFO) && !defined(CONFIG_EXAMPLE_IPV6)
        int enable = 1;
        lwip_setsockopt(sock, IPPROTO_IP, IP_PKTINFO, &enable, sizeof(enable));
#endif

#if defined(CONFIG_EXAMPLE_IPV4) && defined(CONFIG_EXAMPLE_IPV6)
        if (addr_family == AF_INET6) {
            // Note that by default IPV6 binds to both protocols, it is must be disabled
            // if both protocols used at the same time (used in CI)
            int opt = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
        }
#endif

        int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        }
        ESP_LOGI(TAG, "Socket bound, port %d", g_log_remote_port);

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
#if defined(CONFIG_LWIP_NETBUF_RECVINFO) && !defined(CONFIG_EXAMPLE_IPV6)
            int len = recvmsg(sock, &msg, 0);
#else
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
#endif
            // Error occurred during receiving
            if ((len < 0 )&& (errno!=EAGAIN)) {
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                break;
            }
            // Data received
            else {
                // Get the sender's ip address as string
                if (source_addr.ss_family == PF_INET) {
                    inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
#if defined(CONFIG_LWIP_NETBUF_RECVINFO) && !defined(CONFIG_EXAMPLE_IPV6)
                    for ( cmsgtmp = CMSG_FIRSTHDR(&msg); cmsgtmp != NULL; cmsgtmp = CMSG_NXTHDR(&msg, cmsgtmp) ) {
                        if ( cmsgtmp->cmsg_level == IPPROTO_IP && cmsgtmp->cmsg_type == IP_PKTINFO ) {
                            struct in_pktinfo *pktinfo;
                            pktinfo = (struct in_pktinfo*)CMSG_DATA(cmsgtmp);
                            ESP_LOGI(TAG, "dest ip: %s\n", inet_ntoa(pktinfo->ipi_addr));
                        }
                    }
#endif
                } 

                rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string...
                ESP_LOGI(TAG, "Received %d bytes from %s:", len, addr_str);
                ESP_LOGI(TAG, "%s", rx_buffer);
                udp_rec_info.sock = sock;
                memcpy(&(udp_rec_info.source_addr),&source_addr,sizeof(source_addr));
                xQueueOverwrite(udp_info_queue,(void*)&udp_rec_info);
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

esp_err_t esp_log_redirection_init(const esp_log_redirection_config_t *config,uint16_t log_remote_port)
{
    if (g_log_init_flag)
    {
        return ESP_FAIL;
    }
    g_log_config = ESP_LOG_REDIRECTION_CALLOC(1, sizeof(esp_log_redirection_config_t));
    memcpy(g_log_config, config, sizeof(esp_log_redirection_config_t));
    g_log_remote_port = log_remote_port;

    /**< Register espnow log redirect function */
    esp_log_set_vprintf(esp_log_redirection_vprintf);

    g_log_redirection_queue = xQueueCreate(LOG_REDIRECTION_QUEUE_NUM, LOG_REDIRECTION_QUEUE_SIZE);
    udp_info_queue = xQueueCreate(UDP_INFO_QUEUE_NUM, UDP_INFO_QUEUE_SIZE);

    xTaskCreatePinnedToCore(esp_log_redirection_send_task, "log_send", 12 * 1024, (void*)AF_INET, 6 ,NULL, 0);
    xTaskCreatePinnedToCore(esp_log_redirection_rec_task, "log_rec", 12 * 1024, (void*)AF_INET, 6 ,NULL, 0);

    ESP_LOGI(TAG, "log initialized successfully");

    g_log_init_flag = true;

    return ESP_OK;
}