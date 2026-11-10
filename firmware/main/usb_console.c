#include "server.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
extern const uint8_t image_start[] asm("_binary_c6_image_bin_start");
extern const uint8_t image_end[] asm("_binary_c6_image_bin_end");
static void update_c6(void){
 if(wd_enter_maintenance()){ESP_LOGE("usb","Disconnect SWD session before updating C6");return;}
 esp_hosted_coprocessor_fwver_t v={0};
 if(esp_hosted_get_coprocessor_fwversion(&v)!=ESP_OK){ESP_LOGE("usb","Cannot read C6 version; reset P4 and check SDIO / C6 firmware");return;}
 bool activate=v.major1>2||(v.major1==2&&v.minor1>=6);
 esp_err_t e=esp_hosted_slave_ota_begin();
 if(e!=ESP_OK){ESP_LOGE("usb","C6 OTA begin failed: %s; reset P4 to exit maintenance",esp_err_to_name(e));return;}
 size_t size=image_end-image_start;
 for(size_t off=0;off<size;off+=1024){size_t n=size-off;if(n>1024)n=1024;
  e=esp_hosted_slave_ota_write((uint8_t*)image_start+off,n);
  if(e!=ESP_OK){ESP_LOGE("usb","C6 OTA write failed at %u; no retry, reset required",(unsigned)off);return;}
  if(off%65536==0)ESP_LOGI("usb","C6 OTA %u / %u",(unsigned)off,(unsigned)size);
  vTaskDelay(1);
 }
 e=esp_hosted_slave_ota_end();
 if(e==ESP_OK&&activate)e=esp_hosted_slave_ota_activate();
 if(e!=ESP_OK){ESP_LOGE("usb","C6 finalize failed: %s; verify version after reboot",esp_err_to_name(e));return;}
 ESP_LOGI("usb","C6_UPDATE_OK_REBOOT");vTaskDelay(pdMS_TO_TICKS(1500));esp_restart();
}
static int hex_digit(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static void usb_request(const char*text){
 uint8_t in[WD_MAX],out[WD_MAX];size_t n=strlen(text);
 if(n%2||n>WD_MAX*2)return;
 for(size_t i=0;i<n/2;i++){int a=hex_digit(text[2*i]),b=hex_digit(text[2*i+1]);if(a<0||b<0)return;in[i]=(a<<4)|b;}
 size_t count=wd_process(3,in,n/2,out);if(!count)return;
 char line[WD_MAX*2+10];memcpy(line,"WDHEX:",6);const char*d="0123456789ABCDEF";
 for(size_t i=0;i<count;i++){line[6+2*i]=d[out[i]>>4];line[7+2*i]=d[out[i]&15];}
 line[6+count*2]='\n';flockfile(stdout);fputc('\n',stdout);fwrite(line,1,7+count*2,stdout);fflush(stdout);funlockfile(stdout);
}
static void console_task(void*p){char line[WD_MAX*2+16];size_t n=0;
 while(1){int c=getchar();if(c==EOF){clearerr(stdin);vTaskDelay(pdMS_TO_TICKS(20));continue;}
  if(c=='\r')continue;
  if(c=='\n'){line[n]=0;if(!strcmp(line,"USB_DAP_ON")){usb_device_start();}else if(!strncmp(line,"WDHEX:",6))usb_request(line+6);else if(!strcmp(line,"UPDATE_C6"))update_c6();else if(!strcmp(line,"VERSION")){esp_hosted_coprocessor_fwver_t v={0};if(esp_hosted_get_coprocessor_fwversion(&v)==ESP_OK)ESP_LOGI("usb","C6 firmware %lu.%lu.%lu",(unsigned long)v.major1,(unsigned long)v.minor1,(unsigned long)v.patch1);}n=0;}
  else if(n<sizeof(line)-1)line[n++]=(char)c;else n=0;
 }
}
void usb_console_start(void){
 usb_serial_jtag_driver_config_t cfg={.tx_buffer_size=8192,.rx_buffer_size=8192};
 ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
 usb_serial_jtag_vfs_use_driver();
 xTaskCreate(console_task,"usb_console",16384,NULL,4,NULL);
}
