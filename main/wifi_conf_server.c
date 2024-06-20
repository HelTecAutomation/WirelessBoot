#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_err.h"
#include "esp_log.h"

#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "key_data.h"
#include "wifi_ap_sta.h"
#include "wifi_conf_server.h"
/* Scratch buffer size */
#define SCRATCH_BUFSIZE  1024

static const char *TAG = "wifi_conf_server";


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


/* Handler to download a file kept on the server */
static esp_err_t wifi_conf_index_handle(httpd_req_t *req)
{
    /* Get handle to embedded file upload script */
    extern const unsigned char wifi_conf_start[] asm("_binary_wifi_conf_html_start");
    extern const unsigned char wifi_conf_end[]   asm("_binary_wifi_conf_html_end");
    const size_t wifi_conf_size = (wifi_conf_end - wifi_conf_start);

    /* Add file upload form and script which on execution sends a POST request to /upload */
    httpd_resp_send_chunk(req, (const char *)wifi_conf_start, wifi_conf_size);
    /* Send empty chunk to signal HTTP response completion */
    httpd_resp_sendstr_chunk(req, NULL);

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

    /* Content length of the request gives
     * the size of the file being uploaded */
    int remaining = req->content_len;

    while (remaining > 0) {

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
    }
    cjson_wifi_conf_data = cJSON_Parse(buf);
    cjson_ssid_val = cJSON_GetObjectItem(cjson_wifi_conf_data, "ssid");
    cjson_password_val = cJSON_GetObjectItem(cjson_wifi_conf_data, "password");
    if((strlen(cjson_ssid_val->valuestring) >= 32) || (strlen(cjson_password_val->valuestring) >= 64))
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi configuration failed, input length too long!");
        return ESP_FAIL;
    }
    
    // printf("%s %d  %s\r\n", cjson_ssid_val->valuestring,strlen(cjson_ssid_val->valuestring), cjson_password_val->valuestring);
    memset(&wifi_conf,'\0',sizeof(wifi_user_conf_t));
    memcpy(wifi_conf.ssid,cjson_ssid_val->valuestring,strlen(cjson_ssid_val->valuestring));
    memcpy(wifi_conf.password,cjson_password_val->valuestring,strlen(cjson_password_val->valuestring));
    wifi_conf.conf_valid  = CONF_VALID_VAL;
    key_set_wifi_user_conf(&wifi_conf);
    
    /* Get handle to embedded file upload script */
    extern const unsigned char wifi_conf_start[] asm("_binary_wifi_conf_html_start");
    extern const unsigned char wifi_conf_end[]   asm("_binary_wifi_conf_html_end");
    const size_t wifi_conf_size = (wifi_conf_end - wifi_conf_start);

    /* Add file upload form and script which on execution sends a POST request to /upload */
    httpd_resp_send_chunk(req, (const char *)wifi_conf_start, wifi_conf_size);
    /* Send empty chunk to signal HTTP response completion */
    httpd_resp_sendstr_chunk(req, NULL);
    xQueueOverwrite(wifi_user_wifi_conf_queue,(void*)(&wifi_conf));
    return ESP_OK;
}

static esp_err_t wifi_conf_json_handle(httpd_req_t *req)
{
    wifi_user_conf_t wifi_conf;
    key_get_wifi_user_conf(&wifi_conf);

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


/* Function to start the file server */
esp_err_t wifi_conf_server(void)
{

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    /* Use the URI wildcard matching function in order to
     * allow the same handler to respond to multiple different
     * target URIs which match the wildcard scheme */
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting HTTP Server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start file server!");
        return ESP_FAIL;
    }

    /* URI handler for getting uploaded files */
    httpd_uri_t wifi_conf_index = {
        .uri       = "/",  // Match all URIs of type /path/to/file
        .method    = HTTP_GET,
        .handler   = wifi_conf_index_handle,
    };
    httpd_register_uri_handler(server, &wifi_conf_index);

    httpd_uri_t wifi_conf_execute = {
        .uri       = "/wifi_conf",   // Match all URIs of type /delete/path/to/file
        .method    = HTTP_POST,
        .handler   = wifi_conf_execute_handle,
    };
    httpd_register_uri_handler(server, &wifi_conf_execute);

    httpd_uri_t wifi_conf_json = {
        .uri       = "/wifi_conf.json",   // Match all URIs of type /delete/path/to/file
        .method    = HTTP_GET,
        .handler   = wifi_conf_json_handle,
    };
    httpd_register_uri_handler(server, &wifi_conf_json);
    return ESP_OK;
}