#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

esp_err_t   net_ap_start(void);
bool        net_ap_is_up(void);
uint8_t     net_ap_sta_count(void);
esp_netif_t *net_ap_netif(void);
/* Change SSID/password and persist it (clients are briefly disconnected) */
esp_err_t   net_ap_set_creds(const char *ssid, const char *pass);
