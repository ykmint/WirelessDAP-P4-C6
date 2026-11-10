#include "server.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "soc/lp_system_struct.h"
#include "esp_mac.h"
#include <string.h>
#include <stdio.h>
static QueueHandle_t hid_queue;
static volatile bool detached;
static bool started;
static char serial_number[32];
static const uint8_t hid_report[]={0x06,0x00,0xff,0x09,0x01,0xa1,0x01,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x40,0x09,0x01,0x81,0x02,0x95,0x40,0x09,0x01,0x91,0x02,0xc0};
static const tusb_desc_device_t device_desc={.bLength=sizeof(tusb_desc_device_t),.bDescriptorType=TUSB_DESC_DEVICE,.bcdUSB=0x0200,.bDeviceClass=TUSB_CLASS_MISC,.bDeviceSubClass=MISC_SUBCLASS_COMMON,.bDeviceProtocol=MISC_PROTOCOL_IAD,.bMaxPacketSize0=64,.idVendor=0x303a,.idProduct=0x4002,.bcdDevice=0x0100,.iManufacturer=1,.iProduct=2,.iSerialNumber=3,.bNumConfigurations=1};
static const uint8_t configuration[]={TUD_CONFIG_DESCRIPTOR(1,3,0,TUD_CONFIG_DESC_LEN+TUD_HID_DESC_LEN+TUD_CDC_DESC_LEN,0,100),TUD_HID_DESCRIPTOR(0,4,HID_ITF_PROTOCOL_NONE,sizeof(hid_report),0x81,64,1),TUD_CDC_DESCRIPTOR(1,5,0x82,8,0x03,0x83,64)};
static const char* strings[]={(char[]){0x09,0x04},"WirelessDAP","WirelessDAP CMSIS-DAP",serial_number,"CMSIS-DAP","WirelessDAP Control"};
const uint8_t*tud_hid_descriptor_report_cb(uint8_t instance){return hid_report;}
uint16_t tud_hid_get_report_cb(uint8_t instance,uint8_t report_id,hid_report_type_t type,uint8_t*buf,uint16_t n){return 0;}
void tud_hid_set_report_cb(uint8_t instance,uint8_t report_id,hid_report_type_t type,const uint8_t*buf,uint16_t n){if(n==64&&hid_queue)xQueueSend(hid_queue,buf,0);}
static void usb_event(tinyusb_event_t*event,void*arg){if(event->id==TINYUSB_EVENT_DETACHED)detached=true;}
static int hex(char c){if(c>='0'&&c<='9')return c-'0';if(c>='A'&&c<='F')return c-'A'+10;if(c>='a'&&c<='f')return c-'a'+10;return -1;}
static void cdc_line(char*line){
 if(strncmp(line,"WDHEX:",6)){return;}
 line+=6;size_t n=strlen(line);if(n%2||n>WD_MAX*2)return;
 uint8_t in[WD_MAX],out[WD_MAX];for(size_t i=0;i<n/2;i++){int a=hex(line[2*i]),b=hex(line[2*i+1]);if(a<0||b<0)return;in[i]=(a<<4)|b;}
 size_t r=wd_process(3,in,n/2,out);if(!r)return;char response[WD_MAX*2+8];memcpy(response,"WDHEX:",6);const char*d="0123456789ABCDEF";
 for(size_t i=0;i<r;i++){response[6+2*i]=d[out[i]>>4];response[7+2*i]=d[out[i]&15];}response[6+2*r]='\n';
 tinyusb_cdcacm_write_queue(0,(uint8_t*)response,7+2*r);tinyusb_cdcacm_write_flush(0,100);
}
static void usb_worker(void*arg){
 uint8_t req[64],reply[64];char line[WD_MAX*2+16];size_t used=0;
 while(1){
  if(detached){detached=false;wd_release(3);wd_release(4);used=0;}
  if(xQueueReceive(hid_queue,req,0)==pdTRUE){memset(reply,0,64);wd_usb_dap(req,reply);for(int i=0;i<100&&!tud_hid_ready();i++)vTaskDelay(pdMS_TO_TICKS(1));if(tud_hid_ready())tud_hid_report(0,reply,64);}
  uint8_t data[256];size_t n=0;if(tinyusb_cdcacm_read(0,data,sizeof(data),&n)==ESP_OK){for(size_t i=0;i<n;i++){char c=data[i];if(c=='\r')continue;if(c=='\n'){line[used]=0;cdc_line(line);used=0;}else if(used<sizeof(line)-1)line[used++]=c;else used=0;}}
  vTaskDelay(pdMS_TO_TICKS(1));
 }
}
esp_err_t usb_device_start(void){
 if(started){return ESP_OK;}
 uint8_t mac[6];esp_read_mac(mac,ESP_MAC_BASE);snprintf(serial_number,sizeof(serial_number),"%02X%02X%02X%02X%02X%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
 hid_queue=xQueueCreate(4,64);if(!hid_queue)return ESP_ERR_NO_MEM;
 tinyusb_config_t cfg=TINYUSB_DEFAULT_CONFIG(usb_event);cfg.port=TINYUSB_PORT_FULL_SPEED_0;cfg.descriptor.device=&device_desc;cfg.descriptor.full_speed_config=configuration;cfg.descriptor.string=strings;cfg.descriptor.string_count=6;
 esp_err_t e=tinyusb_driver_install(&cfg);if(e!=ESP_OK){vQueueDelete(hid_queue);hid_queue=NULL;return e;}
 tinyusb_config_cdcacm_t cdc={.cdc_port=0};e=tinyusb_cdcacm_init(&cdc);if(e!=ESP_OK)return e;
 // Volatile software selection only. Reset restores ROM USB Serial/JTAG pins.
 tud_disconnect();
 LP_SYS.usb_ctrl.sw_hw_usb_phy_sel=1;LP_SYS.usb_ctrl.sw_usb_phy_sel=1;
 vTaskDelay(pdMS_TO_TICKS(500));
 tud_connect();
 started=true;xTaskCreatePinnedToCore(usb_worker,"usb_dap",16384,NULL,5,NULL,1);return ESP_OK;
}
