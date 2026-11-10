#pragma once
#include <stdint.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#define __STATIC_INLINE static inline
#define __STATIC_FORCEINLINE static inline __attribute__((always_inline))
#define __WEAK __attribute__((weak))
#define __NOP() __asm__ volatile("nop")
#define CPU_CLOCK 1000000U
#define IO_PORT_WRITE_CYCLES 1U
#define DAP_SWD 1
#define DAP_JTAG 0
#define DAP_DEFAULT_PORT 1U
#define DAP_DEFAULT_SWJ_CLOCK 100000U
#define DAP_PACKET_SIZE 64U
#define DAP_PACKET_COUNT 1U
#define SWO_UART 0
#define SWO_MANCHESTER 0
#define SWO_STREAM 0
#define TIMESTAMP_CLOCK 0U
#define DAP_UART 0
#define TARGET_FIXED 0
#define DELAY_SLOW_CYCLES 1U
static inline uint8_t dap_string(char *out,const char *in){strcpy(out,in);return strlen(in)+1;}
static inline uint8_t DAP_GetVendorString(char *s){return dap_string(s,"WirelessDAP");}
static inline uint8_t DAP_GetProductString(char *s){return dap_string(s,"P4-C6 SWD");}
static inline uint8_t DAP_GetSerNumString(char *s){return dap_string(s,"WIRELESS-P4-C6");}
static inline uint8_t DAP_GetProductFirmwareVersionString(char *s){return dap_string(s,"0.1.0");}
static inline uint8_t DAP_GetTargetDeviceVendorString(char*s){return 0;}
static inline uint8_t DAP_GetTargetDeviceNameString(char*s){return 0;}
static inline uint8_t DAP_GetTargetBoardVendorString(char*s){return 0;}
static inline uint8_t DAP_GetTargetBoardNameString(char*s){return 0;}
static inline void PORT_OFF(void){gpio_set_direction(7,GPIO_MODE_INPUT);gpio_set_direction(8,GPIO_MODE_INPUT);}
static inline void PORT_SWD_SETUP(void){gpio_set_level(7,1);gpio_set_level(8,1);gpio_set_direction(7,GPIO_MODE_INPUT_OUTPUT);gpio_set_direction(8,GPIO_MODE_INPUT_OUTPUT);}
static inline void PORT_JTAG_SETUP(void){}
static inline uint32_t PIN_SWCLK_TCK_IN(void){return gpio_get_level(7);}
static inline void PIN_SWCLK_TCK_SET(void){gpio_set_level(7,1);}
static inline void PIN_SWCLK_TCK_CLR(void){gpio_set_level(7,0);}
static inline uint32_t PIN_SWDIO_TMS_IN(void){return gpio_get_level(8);}
static inline void PIN_SWDIO_TMS_SET(void){gpio_set_level(8,1);}
static inline void PIN_SWDIO_TMS_CLR(void){gpio_set_level(8,0);}
static inline uint32_t PIN_SWDIO_IN(void){return gpio_get_level(8);}
static inline void PIN_SWDIO_OUT(uint32_t b){gpio_set_level(8,b&1);}
static inline void PIN_SWDIO_OUT_ENABLE(void){gpio_set_direction(8,GPIO_MODE_INPUT_OUTPUT);}
static inline void PIN_SWDIO_OUT_DISABLE(void){gpio_set_direction(8,GPIO_MODE_INPUT);}
static inline uint32_t PIN_nRESET_IN(void){return 1;}
static inline void PIN_nRESET_OUT(uint32_t b){}
static inline uint32_t PIN_nTRST_IN(void){return 1;}
static inline void PIN_nTRST_OUT(uint32_t b){}
static inline uint32_t PIN_TDI_IN(void){return 0;}
static inline uint32_t PIN_TDO_IN(void){return 0;}
static inline void PIN_TDI_OUT(uint32_t b){}
static inline void LED_CONNECTED_OUT(uint32_t b){}
static inline void LED_RUNNING_OUT(uint32_t b){}
static inline void DAP_SETUP(void){PORT_OFF();}
static inline uint8_t RESET_TARGET(void){return 0;}

#define DAP_UART_USB_COM_PORT 0
static inline uint32_t TIMESTAMP_GET(void){return (uint32_t)esp_timer_get_time();}
