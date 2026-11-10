#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
void interfaces_close(void);
void interfaces_guard(void);
bool interfaces_active(void);
uint8_t interfaces_request(uint8_t op,const uint8_t*p,size_t n,uint8_t*out,size_t*len);
