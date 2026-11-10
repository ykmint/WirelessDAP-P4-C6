#pragma once
#include <stdbool.h>
void board_init(void);
void board_safe(void);
int board_vref_mv(void);
int board_power(bool on);
int board_fault(void);

bool board_power_enabled(void);
