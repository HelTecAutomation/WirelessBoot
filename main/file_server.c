#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"

#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"

#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "key_data.h"
#include "cJSON.h"
#include "key_data.h"
#include "wifi_ap_sta.h"
#include "errno.h"
#include "esp_mac.h"
#include <string.h>

#define HTTPD_REC_DATA_TIMEOUT (9000) //ms
/* Max length a file path can have on storage */
#define FILE_PATH_MAX (64)

/* Max size of an individual file. Make sure this
 * value is same as that set in upload_script.html */
#define MAX_FILE_SIZE   (0x250000) // 200 KB
#define MAX_FILE_SIZE_STR "0x250000"

/* Scratch buffer size */
#define SCRATCH_BUFSIZE  8192

struct file_server_data {
    /* Base path of file storage */
    char base_path[ESP_VFS_PATH_MAX + 1];

    /* Scratch buffer for temporary storage during file transfer */
    char scratch[SCRATCH_BUFSIZE];
};
#define PARTITION_NAME_LEN (16) // 算上结束符
#define APP_PARTITION_NAME        "app"
#define SECOND_APP_PARTITION_NAME "secondApp"
#define FLASH_APP_PARTITION_NAME  "flashApp"
#define NVS_PARTITION_NAME        "nvs"

static firmware_info_t flash_app_info,app_info,second_app_info;

static const char *TAG = "ota_server";
static esp_err_t restart_execute_handle(httpd_req_t *req)
{
    esp_restart();
    return ESP_OK;
}
static esp_err_t chip_id_json_handle(httpd_req_t *req)
{
    char chip_id_str[64];
    uint8_t mac[6];
    uint32_t license[4];
    key_get_heltec_license(license,sizeof(license));
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    cJSON* cjson_main_ob = NULL;
	char* reply_data = NULL;
	cjson_main_ob = cJSON_CreateObject();
    sprintf(chip_id_str,"%02X%02X%02X%02X%02X%02X",mac[5],mac[4],mac[3],mac[2],mac[1],mac[0]);
	cJSON_AddStringToObject(cjson_main_ob, "chipId", chip_id_str);
    sprintf(chip_id_str,"0x%08lX,0x%08lX,0x%08lX,0x%08lX",license[0],license[1],license[2],license[3]);
	cJSON_AddStringToObject(cjson_main_ob, "license", chip_id_str);

	reply_data = cJSON_Print(cjson_main_ob);
    // printf("\r\n %s\r\n", reply_data);

    httpd_resp_sendstr_chunk(req,reply_data);
    httpd_resp_sendstr_chunk(req, NULL);

    cJSON_Delete(cjson_main_ob);
    cJSON_free(reply_data);

    return ESP_OK;
}
static esp_err_t chip_id_execute_handle(httpd_req_t *req)
{
    cJSON* cjson_chip_id_data = NULL;
    cJSON* cjson_val_0 = NULL;
    cJSON* cjson_val_1 = NULL;
    cJSON* cjson_val_2 = NULL;
    cJSON* cjson_val_3 = NULL;
    uint32_t chip_id[4];
    char buf[SCRATCH_BUFSIZE];
    int received;

    /* Content length of the request gives
     * the size of the file being uploaded */
    int remaining = req->content_len;
    uint32_t receive_timeout = (uint32_t)((esp_timer_get_time()/1000)& 0xFFFFFFFF);

    while (remaining > 0)
    {
        if(((uint32_t)((esp_timer_get_time()/1000)& 0xFFFFFFFF) - receive_timeout) > HTTPD_REC_DATA_TIMEOUT )
        {
            httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT,"Receive firmware timeout!");
            /* Return failure to close underlying connection else the
            * incoming file content will keep the socket busy */
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Remaining size : %d", remaining);
        /* Receive the file part by part into a buffer */
        if ((received = httpd_req_recv(req, buf, MIN(remaining, SCRATCH_BUFSIZE))) <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry if timeout occurred */
                continue;
            }

            /* In case of unrecoverable error,
             * close and delete the unfinished file*/
            ESP_LOGE(TAG, "File reception failed!");
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
            return ESP_FAIL;
        }
        // printf(" %s \r\n",buf);
        /* Keep track of remaining size of
         * the file left to be uploaded */
        remaining -= received;
        receive_timeout = (uint32_t)((esp_timer_get_time()/1000)& 0xFFFFFFFF);
    }
    cjson_chip_id_data = cJSON_Parse(buf);
    if(cjson_chip_id_data==NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi configuration failed, json format extraction error!");
        return ESP_FAIL;
    }
    cjson_val_0 = cJSON_GetObjectItem(cjson_chip_id_data, "value0");
    cjson_val_1 = cJSON_GetObjectItem(cjson_chip_id_data, "value1");
    cjson_val_2 = cJSON_GetObjectItem(cjson_chip_id_data, "value2");
    cjson_val_3 = cJSON_GetObjectItem(cjson_chip_id_data, "value3");

    if((cjson_val_0 == NULL) || (cjson_val_1==NULL)|| (cjson_val_2==NULL)|| (cjson_val_3==NULL))
    {
        cJSON_Delete(cjson_chip_id_data);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi configuration failed, json format extraction error!");
        return ESP_FAIL;
    }

    chip_id[0]=(uint32_t)strtoull(cjson_val_0->valuestring,NULL,10);
    chip_id[1]=(uint32_t)strtoull(cjson_val_1->valuestring,NULL,10);
    chip_id[2]=(uint32_t)strtoull(cjson_val_2->valuestring,NULL,10);
    chip_id[3]=(uint32_t)strtoull(cjson_val_3->valuestring,NULL,10);
    // printf(" %08lX %08lX %08lX %08lX \r\n",chip_id[0],chip_id[1],chip_id[2],chip_id[3]);

    key_set_heltec_license(chip_id,sizeof(chip_id));

    if(cjson_chip_id_data!=NULL)
    {
        cJSON_Delete(cjson_chip_id_data);
    }

    httpd_resp_sendstr(req, "LoRaWAN license is configured successfully!");
    return ESP_OK;
}
static esp_err_t wifi_conf_execute_handle(httpd_req_t *req)
{
    cJSON* cjson_wifi_conf_data = NULL;
    cJSON* cjson_ssid_val = NULL;
    cJSON* cjson_password_val = NULL;
    wifi_user_conf_t wifi_conf;

    char buf[SCRATCH_BUFSIZE];
    int received;

    int remaining = req->content_len;
    uint32_t receive_timeout = (uint32_t)((esp_timer_get_time()/1000)& 0xFFFFFFFF);

    while (remaining > 0)
    {
        if(((uint32_t)((esp_timer_get_time()/1000)& 0xFFFFFFFF) - receive_timeout) > HTTPD_REC_DATA_TIMEOUT )
        {
            httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT,"Receive firmware timeout!");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Remaining size : %d", remaining);
        /* Receive the file part by part into a buffer */
        if ((received = httpd_req_recv(req, buf, MIN(remaining, SCRATCH_BUFSIZE))) <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry if timeout occurred */
                continue;
            }

            /* In case of unrecoverable error,
             * close and delete the unfinished file*/
            ESP_LOGE(TAG, "File reception failed!");
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
            return ESP_FAIL;
        }
        // printf(" %s \r\n",buf);
        remaining -= received;
        receive_timeout = (uint32_t)((esp_timer_get_time()/1000)& 0xFFFFFFFF);
    }
    cjson_wifi_conf_data = cJSON_Parse(buf);
    if(cjson_wifi_conf_data==NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi configuration failed, json format extraction error!");
        return ESP_FAIL;
    }
    cjson_ssid_val = cJSON_GetObjectItem(cjson_wifi_conf_data, "ssid");
    cjson_password_val = cJSON_GetObjectItem(cjson_wifi_conf_data, "password");
    if((cjson_ssid_val == NULL) || (cjson_password_val==NULL))
    {
        cJSON_Delete(cjson_wifi_conf_data);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi configuration failed, json format extraction error!");
        return ESP_FAIL;
    }
    if((strlen(cjson_ssid_val->valuestring) >= 32) || (strlen(cjson_password_val->valuestring) >= 64))
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi configuration failed, input length too long!");
        return ESP_FAIL;
    }
    
    memset(&wifi_conf,'\0',sizeof(wifi_user_conf_t));
    memcpy(wifi_conf.ssid,cjson_ssid_val->valuestring,strlen(cjson_ssid_val->valuestring));
    memcpy(wifi_conf.password,cjson_password_val->valuestring,strlen(cjson_password_val->valuestring));
    wifi_conf.conf_valid  = CONF_VALID_VAL;
    key_set_wifi_user_conf(&wifi_conf,sizeof(wifi_user_conf_t));

    if(cjson_wifi_conf_data!=NULL)
    {
        cJSON_Delete(cjson_wifi_conf_data);
    }

    xQueueOverwrite(wifi_user_wifi_conf_queue,(void*)(&wifi_conf));
    httpd_resp_sendstr(req, "WiFi information is configured successfully!");
    return ESP_OK;
}

static esp_err_t wifi_conf_json_handle(httpd_req_t *req)
{
    wifi_user_conf_t wifi_conf;
    key_get_wifi_user_conf(&wifi_conf,sizeof(wifi_user_conf_t));

    cJSON* cjson_main_ob = NULL;
	char* reply_data = NULL;
	cjson_main_ob = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_main_ob, "ssid", wifi_conf.ssid);

	cJSON_AddStringToObject(cjson_main_ob, "password", wifi_conf.password);

	reply_data = cJSON_Print(cjson_main_ob);
    // printf("\r\n %s\r\n", reply_data);

    httpd_resp_sendstr_chunk(req,reply_data);
    httpd_resp_sendstr_chunk(req, NULL);

    cJSON_Delete(cjson_main_ob);
    cJSON_free(reply_data);

    return ESP_OK;
}
/* Handler to redirect incoming GET request for /index.html to /
 * This can be overridden by uploading file with same name */
static esp_err_t index_html_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "307 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);  // Response body can be empty
    return ESP_OK;
}

static void server_get_partition_name(char * partition_name,const char *filename)
{
    if( filename ==NULL)
    {
        memcpy(partition_name,"err",4);
        return;
    }
    if(strstr(filename,APP_PARTITION_NAME)!=NULL)
    {
        memcpy(partition_name,APP_PARTITION_NAME,sizeof(APP_PARTITION_NAME));
    }
    else if(strstr(filename,SECOND_APP_PARTITION_NAME)!=NULL)
    {
        memcpy(partition_name,SECOND_APP_PARTITION_NAME,sizeof(SECOND_APP_PARTITION_NAME));
    }
    else if(strstr(filename,FLASH_APP_PARTITION_NAME)!=NULL)
    {
        memcpy(partition_name,FLASH_APP_PARTITION_NAME,sizeof(FLASH_APP_PARTITION_NAME));
    }
    else if(strstr(filename,NVS_PARTITION_NAME)!=NULL)
    {
        memcpy(partition_name,NVS_PARTITION_NAME,sizeof(NVS_PARTITION_NAME));
    }  
    else
    {
        memcpy(partition_name,"err",4);
    }
}
/* Handler to respond with an icon file embedded in flash.
 * Browsers expect to GET website icon at URI /favicon.ico.
 * This can be overridden by uploading file with same name */
static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
    extern const unsigned char favicon_ico_end[]   asm("_binary_favicon_ico_end");
    const size_t favicon_ico_size = (favicon_ico_end - favicon_ico_start);
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_size);
    return ESP_OK;
}

/* Send HTTP response with a run-time generated html consisting of
 * a list of all files and folders under the requested path.
 * In case of SPIFFS this returns empty list when path is any
 * string other than '/', since SPIFFS doesn't support directories */
static esp_err_t http_resp_dir_html(httpd_req_t *req, const char *dirpath)
{
    /* Get handle to embedded file upload script */
    extern const unsigned char firmware_update_start[] asm("_binary_firmware_update_html_start");
    extern const unsigned char firmware_update_end[]   asm("_binary_firmware_update_html_end");
    const size_t firmware_update_size = (firmware_update_end - firmware_update_start);
    extern const unsigned char wifi_conf_start[] asm("_binary_wifi_conf_html_start");
    extern const unsigned char wifi_conf_end[]   asm("_binary_wifi_conf_html_end");
    const size_t wifi_conf_size = (wifi_conf_end - wifi_conf_start);
    extern const unsigned char lorawan_license_start[] asm("_binary_lorawan_license_html_start");
    extern const unsigned char lorawan_license_end[]   asm("_binary_lorawan_license_html_end");
    const size_t lorawan_license_size = (lorawan_license_end - lorawan_license_start);

    char temp[64];
    /* Add file upload form and script which on execution sends a POST request to /upload */
    httpd_resp_send_chunk(req, (const char *)firmware_update_start, firmware_update_size);

    if(app_info.firmware_valid_flag == FIRMWARE_VALID)
    {
        sprintf(temp,"<tr><td>app</td><td>%ld bytes</td><td>",app_info.partition_size);
        httpd_resp_sendstr_chunk(req, temp);
        httpd_resp_sendstr_chunk(req, app_info.firmware_name);
        httpd_resp_sendstr_chunk(req, "</td><td>");
        httpd_resp_sendstr_chunk(req, app_info.firmware_size);
        httpd_resp_sendstr_chunk(req, " bytes</td><td>");
        httpd_resp_sendstr_chunk(req, app_info.firmware_upload_time);
        // httpd_resp_sendstr_chunk(req, "</td><td><form id=\"runApp\" method=\"post\" action=\"/run/app\"><button type=\"submit\">Run</button></form></td><td><form id=\"eraseApp\" method=\"post\" action=\"/erase/app\"><button type=\"submit\">Erase</button></form></td></tr>");
        httpd_resp_sendstr_chunk(req, "</td><td><button onclick=\"firmwareStatusConf('/run/app')\">Run</button></td><td><button onclick=\"firmwareStatusConf('/erase/app')\">Erase</button></td></tr>");
    }

    if(second_app_info.firmware_valid_flag == FIRMWARE_VALID)
    {
        sprintf(temp,"<tr><td>secondApp</td><td>%ld bytes</td><td>",second_app_info.partition_size);
        httpd_resp_sendstr_chunk(req, temp);
        // httpd_resp_sendstr_chunk(req, "<tr><td>flashApp</td><td>655360 bytes</td><td>");
        httpd_resp_sendstr_chunk(req, second_app_info.firmware_name);
        httpd_resp_sendstr_chunk(req, "</td><td>");
        httpd_resp_sendstr_chunk(req, second_app_info.firmware_size);
        httpd_resp_sendstr_chunk(req, " bytes</td><td>");
        httpd_resp_sendstr_chunk(req, second_app_info.firmware_upload_time);

        httpd_resp_sendstr_chunk(req, "</td><td><button onclick=\"firmwareStatusConf('/run/secondApp')\">Run</button></td><td><button onclick=\"firmwareStatusConf('/erase/secondApp')\">Erase</button></td></tr>");
    }

    if(flash_app_info.firmware_valid_flag == FIRMWARE_VALID)
    {
        sprintf(temp,"<tr><td>flashApp</td><td>%ld bytes</td><td>",flash_app_info.partition_size);
        httpd_resp_sendstr_chunk(req, temp);
        // httpd_resp_sendstr_chunk(req, "<tr><td>flashApp</td><td>655360 bytes</td><td>");
        httpd_resp_sendstr_chunk(req, flash_app_info.firmware_name);
        httpd_resp_sendstr_chunk(req, "</td><td>");
        httpd_resp_sendstr_chunk(req, flash_app_info.firmware_size);
        httpd_resp_sendstr_chunk(req, " bytes</td><td>");
        httpd_resp_sendstr_chunk(req, flash_app_info.firmware_upload_time);

        // httpd_resp_sendstr_chunk(req, "</td><td><form id=\"runFlashApp\" method=\"post\" action=\"/run/flashApp\"><button type=\"submit\">Run</button></form></td><td><form id=\"eraseFlashApp\" method=\"post\" action=\"/erase/flashApp\"><button type=\"submit\">Erase</button></form></td></tr>");
        httpd_resp_sendstr_chunk(req, "</td><td><button onclick=\"firmwareStatusConf('/run/flashApp')\">Run</button></td><td><button onclick=\"firmwareStatusConf('/erase/flashApp')\">Erase</button></td></tr>");
    }

    httpd_resp_sendstr_chunk(req, "<tr><td>nvs</td><td>20480 bytes</td><td>");
    httpd_resp_sendstr_chunk(req, "- </td><td>");
    httpd_resp_sendstr_chunk(req, "- </td><td>");
    httpd_resp_sendstr_chunk(req, "- </td><td> -</td><td><button onclick=\"firmwareStatusConf('/erase/nvs')\">Erase</button></td></tr>");
    httpd_resp_sendstr_chunk(req,"<a href=\"https://docs.heltec.org/en/node/esp32/wireless_boot/index.html\" target=\"_blank\">WirelessBoot Usage Guide</a>");

    /* Finish the file list table */
    httpd_resp_sendstr_chunk(req, "</tbody></table>");

    httpd_resp_send_chunk(req, (const char *)wifi_conf_start,wifi_conf_size);
    httpd_resp_send_chunk(req, (const char *)lorawan_license_start,lorawan_license_size);

    /* Send remaining chunk of HTML file to complete it */
    httpd_resp_sendstr_chunk(req, "</body></html>");

    /* Send empty chunk to signal HTTP response completion */
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}


/* Copies the full path into destination buffer and returns
 * pointer to path (skipping the preceding base path) */
static const char* get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);

    const char *quest = strchr(uri, '?');
    if (quest) {
        pathlen = MIN(pathlen, quest - uri);
    }
    const char *hash = strchr(uri, '#');
    if (hash) {
        pathlen = MIN(pathlen, hash - uri);
    }

    if (base_pathlen + pathlen + 1 > destsize) {
        /* Full path string won't fit into destination buffer */
        return NULL;
    }

    /* Construct full path (base + path) */
    strcpy(dest, base_path);
    strlcpy(dest + base_pathlen, uri, pathlen + 1);

    // printf(" %s  \r\n", dest + base_pathlen);

    /* Return pointer to path, skipping the base */
    return dest + base_pathlen;
}

/* Handler to download a file kept on the server */
static esp_err_t index_loading_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];

    const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                             req->uri, sizeof(filepath));
    if (!filename) 
    {
        ESP_LOGE(TAG, "Filename is too long");
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    /* If name has trailing '/', respond with directory contents */
    if (filename[strlen(filename) - 1] == '/') 
    {
        return http_resp_dir_html(req, filepath);
    }
    
    /* If file not present on SPIFFS check if URI
        * corresponds to one of the hardcoded paths */
    if (strcmp(filename, "/index.html") == 0) 
    {
        return index_html_get_handler(req);
    } 
    else if (strcmp(filename, "/favicon.ico") == 0) 
    {
        return favicon_get_handler(req);
    }
    else if(strcmp(filename, "/wifi_conf.json") == 0) 
    {
        return wifi_conf_json_handle(req);
    }
    else if(strcmp(filename, "/get_chip_id.json") == 0) 
    {
        return chip_id_json_handle(req);
    } 
  
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "address not found");
    return ESP_FAIL;

    /* Respond with an empty chunk to signal HTTP response completion */
#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
#endif
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Handler to upload a file onto the server */
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    char partition_name[PARTITION_NAME_LEN];
    firmware_info_t *firmware_info;
    char str_temp[128];
    /* Skip leading "/upload" from URI to get filename */
    /* Note sizeof() counts NULL termination hence the -1 */
    const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                             req->uri + sizeof("/upload") - 1, sizeof(filepath));
    if (!filename) 
    {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    /* Filename cannot have a trailing '/' */
    if (filename[strlen(filename) - 1] == '/') 
    {
        ESP_LOGE(TAG, "Invalid filename : %s", filename);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid filename");
        return ESP_FAIL;
    }

    server_get_partition_name(partition_name,filename);


    esp_err_t err;
    /* update handle : set by esp_ota_begin(), must be freed via esp_ota_end() */
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = NULL;

    if((strncmp(APP_PARTITION_NAME,partition_name,strlen(APP_PARTITION_NAME))==0))
    {
        firmware_info = &app_info;
        update_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_0,"app");
        firmware_info->partition_size =  update_partition->size;
        firmware_info->firmware_valid_flag = FIRMWARE_INVALID;
        key_set_app_info(firmware_info,sizeof(firmware_info_t));
    }
    else if(strncmp(SECOND_APP_PARTITION_NAME,partition_name,strlen(SECOND_APP_PARTITION_NAME))==0)
    {
        firmware_info = &second_app_info;
        update_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_2,"secondApp");
        firmware_info->partition_size =  update_partition->size;
        firmware_info->firmware_valid_flag = FIRMWARE_INVALID;
        key_set_second_app_info(firmware_info,sizeof(firmware_info_t));
    }
    else if(strncmp(FLASH_APP_PARTITION_NAME,partition_name,strlen(FLASH_APP_PARTITION_NAME))==0)
    {
        firmware_info = &flash_app_info;
        // const esp_partition_t *app_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_0,"app");
        // update_partition = esp_ota_get_next_update_partition(app_partition);
        update_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_1,"flashApp");
        firmware_info->partition_size =  update_partition->size;
        firmware_info->firmware_valid_flag = FIRMWARE_INVALID;
        key_set_flash_app_info(firmware_info,sizeof(firmware_info_t));
    }
    else
    {
        ESP_LOGE(TAG, "Invalid partition name: %s", partition_name);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid partition name");
        return ESP_FAIL;
    }
    if(update_partition == NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,"Partition lookup failed.");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%"PRIx32,
             update_partition->subtype, update_partition->address);

    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        esp_ota_abort(update_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
        return ESP_FAIL;
    }

    /* File cannot be larger than a limit */
    if (req->content_len > firmware_info->partition_size) 
    {
        ESP_LOGE(TAG, "Firmware too large : %d bytes", req->content_len);
        /* Respond with 400 Bad Request */
        sprintf(str_temp,"Firmware size must be less than %ld !",firmware_info->partition_size);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,str_temp);
        /* Return failure to close underlying connection else the
         * incoming file content will keep the socket busy */
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Receiving firmware : %s...", filename);

    /* Retrieve the pointer to scratch buffer for temporary storage */
    char *buf = ((struct file_server_data *)req->user_ctx)->scratch;
    int received;

    /* Content length of the request gives
     * the size of the file being uploaded */
    int remaining = req->content_len;
    uint32_t receive_timeout = (uint32_t)((esp_timer_get_time()/1000)& 0xFFFFFFFF);
    while (remaining > 0 )
    {
        if(((uint32_t)((esp_timer_get_time()/1000)& 0xFFFFFFFF) - receive_timeout) > HTTPD_REC_DATA_TIMEOUT )
        {
            httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT,"Receive firmware timeout!");
            /* Return failure to close underlying connection else the
            * incoming file content will keep the socket busy */
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Remaining size : %d ", remaining);
        /* Receive the file part by part into a buffer */
        if ((received = httpd_req_recv(req, buf, MIN(remaining, SCRATCH_BUFSIZE))) <= 0) 
        {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) 
            {
                /* Retry if timeout occurred */
                continue;
            }
            esp_ota_abort(update_handle);
            ESP_LOGE(TAG, "firmware reception failed!");
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive firmware");
            return ESP_FAIL;
        }
#if 0
        if (( remaining == req->content_len) && received > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
        {
            esp_app_desc_t new_app_info;
            memcpy(&new_app_info, &buf[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));
            ESP_LOGE(TAG, "New firmware version: %s", new_app_info.version);
            ESP_LOGE(TAG, "project_name: %s", new_app_info.project_name);
            ESP_LOGE(TAG, "time: %s data: %s", new_app_info.time,new_app_info.date);
            ESP_LOGE(TAG, "idf_ver: %s ", new_app_info.idf_ver);
        }
#endif
        err = esp_ota_write( update_handle, (const void *)buf, received);
        if (err != ESP_OK)
        {
            esp_ota_abort(update_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_write failed");
            return ESP_FAIL;
        }
        remaining -= received;
        receive_timeout = (uint32_t)((esp_timer_get_time()/1000)& 0xFFFFFFFF);
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) 
    {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED)
        {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Image validation failed, image is corrupted");
        } else
        {
            ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
        }
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_end faile");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) 
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_set_boot_partition failed ");
        return ESP_FAIL;
    }
    int time_index=0,name_index=0;
    for(int i =1,flag_num=0;filename[i]!='\0';i++)
    {
        if((filename[i] == '_') && (flag_num <= 1))
        {
            flag_num +=1;
        }
        else if(flag_num == 1)
        {
            if(filename[i] == '.')
            {
                firmware_info->firmware_upload_time[time_index++] = ' ';
            }
            else
            {
                firmware_info->firmware_upload_time[time_index++] = filename[i];
            }
        }
        else if(flag_num >  1)
        {
            firmware_info->firmware_name[name_index++] = filename[i];
        }
    }
    firmware_info->firmware_upload_time[time_index++] = '\0';
    firmware_info->firmware_name[name_index++]   = '\0';
    firmware_info->firmware_valid_flag = FIRMWARE_VALID;
    sprintf(firmware_info->firmware_size,"%d",req->content_len);

#if 0
    printf("%s\r\n",firmware_info->firmware_upload_time);
    printf("%s\r\n",firmware_info->firmware_name);
    printf("%s\r\n",firmware_info->firmware_size);
    printf("%s\r\n",partition_name);
#endif

    if((strncmp(APP_PARTITION_NAME,partition_name,strlen(APP_PARTITION_NAME))==0))
    {
        key_set_app_info(firmware_info,sizeof(firmware_info_t));
    }
    else if(strncmp(SECOND_APP_PARTITION_NAME,partition_name,strlen(SECOND_APP_PARTITION_NAME))==0)
    {
        key_set_second_app_info(firmware_info,sizeof(firmware_info_t));
    }
    else if(strncmp(FLASH_APP_PARTITION_NAME,partition_name,strlen(FLASH_APP_PARTITION_NAME))==0)
    {
        key_set_flash_app_info(firmware_info,sizeof(firmware_info_t));
    }


    /* Close file upon upload completion */
    ESP_LOGI(TAG, "firmware reception complete");
    /* Redirect onto root to see the updated file list */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
#endif
    httpd_resp_sendstr(req, "File uploaded successfully");
    return ESP_OK;
}

static esp_err_t run_post_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    char partition_name[PARTITION_NAME_LEN];
    firmware_info_t *run_app_info;
    char str_temp[128];
    /* Skip leading "/delete" from URI to get filename */
    /* Note sizeof() counts NULL termination hence the -1 */
    const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                             req->uri  + sizeof("/run") - 1, sizeof(filepath));
    if (!filename)
    {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    /* Filename cannot have a trailing '/' */
    if (filename[strlen(filename) - 1] == '/')
    {
        ESP_LOGE(TAG, "Invalid filename : %s", filename);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid filename");
        return ESP_FAIL;
    }
    server_get_partition_name(partition_name,filename);
   
    if((strncmp(APP_PARTITION_NAME,partition_name,strlen(APP_PARTITION_NAME))==0))
    {
        run_app_info = &app_info;
        const esp_partition_t *app_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_0,"app");
        if(run_app_info->firmware_valid_flag == FIRMWARE_VALID)
        {
            esp_err_t err = esp_ota_set_boot_partition(app_partition);
            if (err != ESP_OK) 
            {
                ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to set running partition.");
                return ESP_FAIL;
            }
        }
        else
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "There is no firmware in this partition.");
            return ESP_FAIL;
        }
    }
    else if(strncmp(SECOND_APP_PARTITION_NAME,partition_name,strlen(SECOND_APP_PARTITION_NAME))==0)
    {
        run_app_info = &second_app_info;
        const esp_partition_t *secondApp_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_2,"secondApp");
        if(run_app_info->firmware_valid_flag == FIRMWARE_VALID)
        {
            esp_err_t err = esp_ota_set_boot_partition(secondApp_partition);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to set running partition.");
                return ESP_FAIL;
            }
        }
        else
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "There is no firmware in this partition.");
            return ESP_FAIL;
        }
    }
    else if(strncmp(FLASH_APP_PARTITION_NAME,partition_name,strlen(FLASH_APP_PARTITION_NAME))==0)
    {
        run_app_info = &flash_app_info;
        const esp_partition_t *flashApp_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_1,"flashApp");
        if(run_app_info->firmware_valid_flag == FIRMWARE_VALID)
        {
            esp_err_t err = esp_ota_set_boot_partition(flashApp_partition);
            if (err != ESP_OK) 
            {
                ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to set running partition.");
                return ESP_FAIL;
            }
        }
        else
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "There is no firmware in this partition.");
            return ESP_FAIL;
        }
    }
    else
    {
        ESP_LOGE(TAG, "Invalid partition name: %s", partition_name);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid partition name");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Next Run Partition : %s", partition_name);

#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
#endif
    sprintf(str_temp,"Setting up the %s partition runs successfully.",partition_name);
    httpd_resp_send(req,str_temp,strlen(str_temp));
    return ESP_OK;
}


/* Handler to delete a file from the server */
static esp_err_t erase_post_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    char partition_name[PARTITION_NAME_LEN];
    firmware_info_t *erase_app_info;
    char str_temp[64];
    /* Skip leading "/delete" from URI to get filename */
    /* Note sizeof() counts NULL termination hence the -1 */
    const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                             req->uri  + sizeof("/erase") - 1, sizeof(filepath));
    if (!filename)
    {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    /* Filename cannot have a trailing '/' */
    if (filename[strlen(filename) - 1] == '/')
    {
        ESP_LOGE(TAG, "Invalid filename : %s", filename);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid filename");
        return ESP_FAIL;
    }
    server_get_partition_name(partition_name,filename);

    
    if((strncmp(APP_PARTITION_NAME,partition_name,strlen(APP_PARTITION_NAME))==0))
    {
        erase_app_info = &app_info;
        const esp_partition_t *app_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_0,"app");
        if(erase_app_info->firmware_valid_flag == FIRMWARE_VALID)
        {
            esp_err_t err = esp_partition_erase_range(app_partition, 0, app_partition->size);
            if (err != ESP_OK) 
            {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Partition erase failed.");
                return ESP_FAIL;
            }
            erase_app_info->firmware_valid_flag = FIRMWARE_INVALID;
            key_set_app_info(erase_app_info,sizeof(firmware_info_t));
        }
    }
    else if(strncmp(SECOND_APP_PARTITION_NAME,partition_name,strlen(SECOND_APP_PARTITION_NAME))==0)
    {
        erase_app_info = &second_app_info;
        const esp_partition_t *secondApp_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_2,"secondApp");
        if(erase_app_info->firmware_valid_flag == FIRMWARE_VALID)
        {
            esp_err_t err = esp_partition_erase_range(secondApp_partition, 0, secondApp_partition->size);
            if (err != ESP_OK) 
            {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Partition erase failed.");
                return ESP_FAIL;
            }
            erase_app_info->firmware_valid_flag = FIRMWARE_INVALID;
            key_set_flash_app_info(erase_app_info,sizeof(firmware_info_t));
        }
    }
    else if(strncmp(FLASH_APP_PARTITION_NAME,partition_name,strlen(FLASH_APP_PARTITION_NAME))==0)
    {
        erase_app_info = &flash_app_info;
        const esp_partition_t *flashApp_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_1,"flashApp");
        if(erase_app_info->firmware_valid_flag == FIRMWARE_VALID)
        {
            esp_err_t err = esp_partition_erase_range(flashApp_partition, 0, flashApp_partition->size);
            if (err != ESP_OK) 
            {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Partition erase failed.");
                return ESP_FAIL;
            }
            erase_app_info->firmware_valid_flag = FIRMWARE_INVALID;
            key_set_flash_app_info(erase_app_info,sizeof(firmware_info_t));
        }
    }
    else if(strncmp(NVS_PARTITION_NAME,partition_name,strlen(NVS_PARTITION_NAME))==0)
    {
        esp_err_t nvs_err = nvs_flash_erase();
        if(nvs_err !=ESP_OK )
        {
            ESP_LOGE(TAG, "Nvs flash erase fail.");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Nvs flash erase fail.");
            return ESP_FAIL;
        }
    }
    else
    {
        ESP_LOGE(TAG, "Invalid partition name: %s", partition_name);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid partition name");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "The partition %s has been erased.", partition_name);

#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
#endif
    sprintf(str_temp,"The %s partition has been erased.",partition_name);
    httpd_resp_send(req,str_temp,strlen(str_temp));
    return ESP_OK;
}



/* Function to start the file server */
esp_err_t start_ota_file_server(void)
{
    key_get_app_info((void*)&app_info,sizeof(firmware_info_t));
    key_get_second_app_info((void*)&second_app_info,sizeof(firmware_info_t));
    key_get_flash_app_info((void*)&flash_app_info,sizeof(firmware_info_t));
    const esp_partition_t *next_run_partition = NULL;
    if(app_info.firmware_valid_flag == FIRMWARE_VALID)
    {
         next_run_partition= esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_0,"app");
    }
    else if(second_app_info.firmware_valid_flag == FIRMWARE_VALID)
    {
        next_run_partition= esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_2,SECOND_APP_PARTITION_NAME);
    }
    else if(flash_app_info.firmware_valid_flag == FIRMWARE_VALID)
    {
        next_run_partition= esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_1,"flashApp");
    }
    else
    {
        ESP_LOGE(TAG, "There is no valid firmware."); 
    }
    if(next_run_partition !=NULL)
    {
        esp_err_t err = esp_ota_set_boot_partition(next_run_partition);
        if (err != ESP_OK) 
        {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
        }
    }
    static struct file_server_data *server_data = NULL;
    const char base_path[] = "base";
    if (server_data)
    {
        ESP_LOGE(TAG, "File server already started");
        return ESP_ERR_INVALID_STATE;
    }

    /* Allocate memory for server data */
    server_data = calloc(1, sizeof(struct file_server_data));
    if (!server_data) 
    {
        ESP_LOGE(TAG, "Failed to allocate memory for server data");
        return ESP_ERR_NO_MEM;
    }
    strlcpy(server_data->base_path, base_path,
            sizeof(server_data->base_path));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    /* Use the URI wildcard matching function in order to
     * allow the same handler to respond to multiple different
     * target URIs which match the wildcard scheme */
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size   = 20480;
    ESP_LOGI(TAG, "Starting HTTP Server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start file server!");
        return ESP_FAIL;
    }

    /* URI handler for getting uploaded files */
    httpd_uri_t firmware_index = {
        .uri       = "/*",  // Match all URIs of type /path/to/file
        .method    = HTTP_GET,
        .handler   = index_loading_handler,
        .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &firmware_index);

    /* URI handler for uploading files to server */
    httpd_uri_t firmware_upload = {
        .uri       = "/upload/*",   // Match all URIs of type /upload/path/to/file
        .method    = HTTP_POST,
        .handler   = upload_post_handler,
        .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &firmware_upload);

    /* URI handler for deleting files from server */
    httpd_uri_t firmware_erase = {
        .uri       = "/erase/*",   // Match all URIs of type /delete/path/to/file
        .method    = HTTP_POST,
        .handler   = erase_post_handler,
        .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &firmware_erase);


    /* URI handler for deleting files from server */
    httpd_uri_t firmware_run_select = {
        .uri       = "/run/*",   // Match all URIs of type /delete/path/to/file
        .method    = HTTP_POST,
        .handler   = run_post_handler,
        .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &firmware_run_select);

    httpd_uri_t wifi_conf_execute = {
        .uri       = "/wifi_conf/*",   // Match all URIs of type /delete/path/to/file
        .method    = HTTP_POST,
        .handler   = wifi_conf_execute_handle,
    };
    httpd_register_uri_handler(server, &wifi_conf_execute);

    httpd_uri_t chid_id_execute = {
        .uri       = "/chid_id/*",   // Match all URIs of type /delete/path/to/file
        .method    = HTTP_POST,
        .handler   = chip_id_execute_handle,
    };
    httpd_register_uri_handler(server, &chid_id_execute);
    
    httpd_uri_t restart_execute = {
        .uri       = "/restart",   // Match all URIs of type /delete/path/to/file
        .method    = HTTP_POST,
        .handler   = restart_execute_handle,
    };
    httpd_register_uri_handler(server, &restart_execute);
    return ESP_OK;
}