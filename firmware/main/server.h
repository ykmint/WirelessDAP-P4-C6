#pragma once
#include <stdint.h>
#include <stddef.h>
#define WD_MAX 266
void server_init(void);
size_t wd_process(int owner,const uint8_t *in,size_t n,uint8_t *out);
void wd_release(int owner);
void ble_start(void);

int wd_enter_maintenance(void);
void usb_console_start(void);

#include "esp_err.h"
esp_err_t usb_device_start(void);
void wd_usb_dap(const uint8_t*request,uint8_t*response);
