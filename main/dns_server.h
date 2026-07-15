/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Adapted for esp_wifi_remote: accepts explicit esp_netif_t* handle
 * instead of resolving by ifkey string (which may not work with
 * esp_wifi_remote's AP netif).
 */

#pragma once

#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DNS_SERVER_MAX_ITEMS
#define DNS_SERVER_MAX_ITEMS 1
#endif

#define DNS_SERVER_CONFIG_SINGLE(queried_name, netif_key)  {        \
        .num_of_entries = 1,                                        \
        .item = { { .name = queried_name, .if_key = netif_key } }   \
        }

/**
 * @brief Definition of one DNS entry: NAME - IP (or the netif whose IP to answer)
 *
 * @note Please use string literals (or ensure they are valid during dns_server lifetime) as names, since
 * we don't take copies of the config values `name` and `if_key`
 */
typedef struct dns_entry_pair {
    const char* name;       /**<! Exact match of the name field of the DNS query to answer */
    const char* if_key;     /**<! Use this network interface IP to answer, only if NULL, use the static IP below */
    esp_ip4_addr_t ip;      /**<! Constant IP address to answer this query, if "if_key==NULL" */
} dns_entry_pair_t;

typedef struct dns_server_config {
    int num_of_entries;                             /**<! Number of rules specified in the config struct */
    dns_entry_pair_t item[DNS_SERVER_MAX_ITEMS];    /**<! Array of pairs */
} dns_server_config_t;

typedef struct dns_server_handle *dns_server_handle_t;

/**
 * @brief Set up and start a simple DNS server that will respond to all A queries (IPv4)
 * based on configured rules.
 *
 * @param config Configuration structure listing the pairs of (name, IP/netif-id)
 * @return dns_server's handle on success, NULL on failure
 */
dns_server_handle_t start_dns_server(dns_server_config_t *config);

/**
 * @brief Start a DNS server bound to a specific netif handle.
 *
 * This is the preferred API for esp_wifi_remote setups where the AP netif
 * may not be discoverable by ifkey string (e.g. "WIFI_AP_DEF").
 * All DNS A queries will be redirected to the IP address of ap_netif.
 *
 * @param ap_netif  The SoftAP netif handle (from esp_netif_create_default_wifi_ap)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if ap_netif is NULL
 */
esp_err_t start_dns_server_on_netif(esp_netif_t *ap_netif);

/**
 * @brief Stops and destroys DNS server's task and structs
 * @param handle DNS server's handle to destroy
 */
void stop_dns_server(dns_server_handle_t handle);

#ifdef __cplusplus
}
#endif
