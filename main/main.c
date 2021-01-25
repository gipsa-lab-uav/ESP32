/*
 *   This file is part of DroneBridge: https://github.com/seeul8er/DroneBridge
 *
 *   Copyright 2018 Wolfgang Christl
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 */

#include <stdio.h>
#include <nvs_flash.h>
#include <esp_wifi_types.h>
#include <mdns.h>
#include <string.h>
#include <driver/gpio.h>
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "db_esp32_control.h"
#include "http_server.h"
#include "db_esp32_comm.h"
#include "db_protocol.h"

#define STA_MAXIMUM_RETRY 3

EventGroupHandle_t wifi_event_group;
static const char *TAG = "DB_ESP32";
static int s_retry_num = 0;

uint8_t DEFAULT_SSID[32] = "DroneBridge ESP32";
uint8_t DEFAULT_PWD[64] = "dronebridge";
uint8_t DEFAULT_CHANNEL = 6;
uint8_t SERIAL_PROTOCOL = 2;  // 1,2=MSP, 3,4,5=MAVLink/transparent
uint8_t DB_UART_PIN_TX = GPIO_NUM_17;
uint8_t DB_UART_PIN_RX = GPIO_NUM_16;
uint32_t DB_UART_BAUD_RATE = 115200;
uint16_t TRANSPARENT_BUF_SIZE = 64;
uint8_t LTM_FRAME_NUM_BUFFER = 1;

void init_wifi_ap();
void init_wifi_sta();

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    wifi_event_ap_staconnected_t *event;
    wifi_event_ap_stadisconnected_t* evente;
    if(event_base==WIFI_EVENT)
    {
	switch (event_id) {
        case SYSTEM_EVENT_AP_START:
            ESP_LOGI(TAG, "Wifi AP started!");
            xEventGroupSetBits(wifi_event_group, BIT2);
            break;
        case SYSTEM_EVENT_AP_STOP:
            ESP_LOGI(TAG, "Wifi AP stopped!");
            break;
        case SYSTEM_EVENT_AP_STACONNECTED:
            event = (wifi_event_ap_staconnected_t *) event_data;
            ESP_LOGI(TAG, "Client connected - station:"MACSTR", AID=%d", MAC2STR(event->mac), event->aid);
            break;
        case SYSTEM_EVENT_AP_STADISCONNECTED:
            evente = (wifi_event_ap_stadisconnected_t*) event_data;
            ESP_LOGI(TAG, "Client disconnected - station:"MACSTR", AID=%d",
                     MAC2STR(evente->mac), evente->aid);
            break;
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
	    if (s_retry_num < STA_MAXIMUM_RETRY) {
		esp_wifi_connect();
		s_retry_num++;
		ESP_LOGI(TAG, "retry to connect to the AP");
	    } else {
		xEventGroupSetBits(wifi_event_group, BIT1);
		ESP_LOGI(TAG,"connect to the AP fail");
	    }
	    break;

        default:
            break;
	}
    }
    else if(event_base==IP_EVENT && event_id==IP_EVENT_STA_GOT_IP)
    {
	s_retry_num = 0;
	ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
	ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
	//ESP_ERROR_CHECK(tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_STA, &event->ip_info));
	//ESP_ERROR_CHECK(tcpip_adapter_sta_start(TCPIP_ADAPTER_IF_STA, &event->ip_info));
	xEventGroupSetBits(wifi_event_group, BIT0|BIT2);
    }
}

void start_mdns_service()
{
    xEventGroupWaitBits(wifi_event_group, BIT2, false, true, portMAX_DELAY);
    //initialize mDNS service
    esp_err_t err = mdns_init();
    if (err) {
        printf("MDNS Init failed: %d\n", err);
        return;
    }
    ESP_ERROR_CHECK(mdns_hostname_set("dronebridge"));
    ESP_ERROR_CHECK(mdns_instance_name_set("DroneBridge for ESP32"));

    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_db_proxy", "_tcp", APP_PORT_PROXY, NULL, 0));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_db_comm", "_tcp", APP_PORT_COMM, NULL, 0));
    ESP_ERROR_CHECK(mdns_service_instance_name_set("_http", "_tcp", "DroneBridge for ESP32"));
}


void init_wifi_ap()
{
    /* AP MODE */
    wifi_config_t ap_config = {
            .ap = {
                    .ssid = "Init DroneBridge ESP32",
                    .ssid_len = 0,
                    .authmode = WIFI_AUTH_WPA_PSK,
                    .channel = DEFAULT_CHANNEL,
                    .ssid_hidden = 0,
                    .beacon_interval = 100,
                    .max_connection = 10
            },
    };

    xthal_memcpy(ap_config.ap.ssid, DEFAULT_SSID, 32);
    xthal_memcpy(ap_config.ap.password, DEFAULT_PWD, 64);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(ESP_IF_WIFI_AP, WIFI_PROTOCOL_11B));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    wifi_country_t wifi_country = {.cc = "XX", .schan = 1, .nchan = 13, .policy = WIFI_COUNTRY_POLICY_MANUAL};
    ESP_ERROR_CHECK(esp_wifi_set_country(&wifi_country));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_AP, "DBESP32"));

    ESP_ERROR_CHECK(tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP));
    tcpip_adapter_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192,168,2,1);
    IP4_ADDR(&ip_info.gw, 192,168,2,1);
    IP4_ADDR(&ip_info.netmask, 255,255,255,0);
    ESP_ERROR_CHECK(tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &ip_info));
    ESP_ERROR_CHECK(tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP));

}

void init_wifi_sta()
{
    /* STATION Mode */
    wifi_config_t sta_config = {
        .sta = {
            .ssid = "Init DroneBridge ESP32",
            .password = "dronebridge",
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,

        },
    };
    xthal_memcpy(sta_config.sta.ssid, DEFAULT_SSID, 32);
    xthal_memcpy(sta_config.sta.password, DEFAULT_PWD, 64);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );
    
}


void init_wifi(){
    wifi_event_group = xEventGroupCreate();
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    
    init_wifi_sta();
    
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
					   BIT0 | BIT1,
					   pdFALSE,
					   pdFALSE,
					   portMAX_DELAY);
    if (bits & BIT1)
    {
	ESP_LOGI(TAG, "Start AP");

	init_wifi_ap();
    }
}


void read_settings_nvs(){
    nvs_handle my_handle;
    if (nvs_open("settings", NVS_READONLY, &my_handle) == ESP_ERR_NVS_NOT_FOUND){
        // First start
        nvs_close(my_handle);
        write_settings_to_nvs();
    } else {
        ESP_LOGI(TAG, "Reading settings from NVS");
        size_t required_size = 0;
        ESP_ERROR_CHECK(nvs_get_str(my_handle, "ssid", NULL, &required_size));
        char* ssid = malloc(required_size);
        ESP_ERROR_CHECK(nvs_get_str(my_handle, "ssid", ssid, &required_size));
        memcpy(DEFAULT_SSID, ssid, required_size);

        ESP_ERROR_CHECK(nvs_get_str(my_handle, "wifi_pass", NULL, &required_size));
        char* wifi_pass = malloc(required_size);
        ESP_ERROR_CHECK(nvs_get_str(my_handle, "wifi_pass", wifi_pass, &required_size));
        memcpy(DEFAULT_PWD, wifi_pass, required_size);

        ESP_ERROR_CHECK(nvs_get_u8(my_handle, "wifi_chan", &DEFAULT_CHANNEL));
        ESP_ERROR_CHECK(nvs_get_u32(my_handle, "baud", &DB_UART_BAUD_RATE));
        ESP_ERROR_CHECK(nvs_get_u8(my_handle, "gpio_tx", &DB_UART_PIN_TX));
        ESP_ERROR_CHECK(nvs_get_u8(my_handle, "gpio_rx", &DB_UART_PIN_RX));
        ESP_ERROR_CHECK(nvs_get_u8(my_handle, "proto", &SERIAL_PROTOCOL));
        ESP_ERROR_CHECK(nvs_get_u16(my_handle, "trans_pack_size", &TRANSPARENT_BUF_SIZE));
        ESP_ERROR_CHECK(nvs_get_u8(my_handle, "ltm_per_packet", &LTM_FRAME_NUM_BUFFER));
        nvs_close(my_handle);
        free(wifi_pass);
        free(ssid);
    }
}


void app_main()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    read_settings_nvs();
    esp_log_level_set("*", ESP_LOG_INFO);
    init_wifi();
    start_mdns_service();
    control_module();
    start_tcp_server();
    communication_module();
}
