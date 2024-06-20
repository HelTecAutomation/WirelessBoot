#pragma once

#ifdef __cplusplus
extern "C" {
#endif /**< _cplusplus */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <esp_http_server.h>

#include "esp_log.h"

#define ESP_LOG_REDIRECTION_CALLOC calloc
#define ESP_LOG_REDIRECTION_MALLOC malloc
#define ESP_LOG_REDIRECTION_FREE   free

typedef struct {
    esp_log_level_t log_level_uart;
    esp_log_level_t log_level_udp;
} esp_log_redirection_config_t;

esp_err_t esp_log_redirection_init(const esp_log_redirection_config_t *config,uint16_t log_remote_port);

#ifdef __cplusplus
}
#endif /**< _cplusplus */