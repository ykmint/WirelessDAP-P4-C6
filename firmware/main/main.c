#include "server.h"
#include "board.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <string.h>
static void wifi_event(void*arg,esp_event_base_t base,int32_t id,void*data){
 if(base==WIFI_EVENT && (id==WIFI_EVENT_STA_START||id==WIFI_EVENT_STA_DISCONNECTED))esp_wifi_connect();
 if(base==IP_EVENT&&id==IP_EVENT_STA_GOT_IP){ip_event_got_ip_t*e=data;ESP_LOGI("wifi","STA IP: " IPSTR,IP2STR(&e->ip_info.ip));}
}
static void start_usb_device(void*arg){vTaskDelay(pdMS_TO_TICKS(3000));usb_device_start();vTaskDelete(NULL);}
void app_main(void){
 board_init();
 esp_err_t e=nvs_flash_init();if(e==ESP_ERR_NVS_NO_FREE_PAGES||e==ESP_ERR_NVS_NEW_VERSION_FOUND){ESP_ERROR_CHECK(nvs_flash_erase());e=nvs_flash_init();}ESP_ERROR_CHECK(e);
 ESP_ERROR_CHECK(esp_netif_init());ESP_ERROR_CHECK(esp_event_loop_create_default());
 esp_netif_create_default_wifi_ap();bool sta=strlen(CONFIG_WD_STA_SSID)>0;if(sta)esp_netif_create_default_wifi_sta();
 ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL));ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL));
 wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT();ESP_ERROR_CHECK(esp_wifi_init(&cfg));
 wifi_config_t ap={.ap={.ssid="WirelessDAP",.channel=1,.max_connection=2,.authmode=WIFI_AUTH_WPA2_PSK}};
 strlcpy((char*)ap.ap.password,CONFIG_WD_AP_PASSWORD,sizeof(ap.ap.password));
 ESP_ERROR_CHECK(esp_wifi_set_mode(sta?WIFI_MODE_APSTA:WIFI_MODE_AP));ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&ap));
 if(sta){wifi_config_t s={0};strlcpy((char*)s.sta.ssid,CONFIG_WD_STA_SSID,sizeof(s.sta.ssid));strlcpy((char*)s.sta.password,CONFIG_WD_STA_PASSWORD,sizeof(s.sta.password));ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&s));}
 ESP_ERROR_CHECK(esp_wifi_start());server_init();usb_console_start();ble_start();ESP_LOGI("dap","Wi-Fi TCP 6123; BLE readiness reported separately; target power OFF");
 xTaskCreate(start_usb_device,"usb_start",4096,NULL,3,NULL);
}
