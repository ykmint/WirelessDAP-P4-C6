#include "board.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
static bool target_power_enabled;
static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t cal;
void board_safe(void){gpio_set_direction(7,GPIO_MODE_INPUT);gpio_set_direction(8,GPIO_MODE_INPUT);gpio_set_level(9,0);target_power_enabled=false;}
int board_fault(void){return !gpio_get_level(10);}
void board_init(void){
 gpio_set_level(9,0);gpio_set_direction(9,GPIO_MODE_OUTPUT);board_safe();
 gpio_config_t g={.pin_bit_mask=(1ULL<<10)|(1ULL<<20)|(1ULL<<22),.mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE};
 ESP_ERROR_CHECK(gpio_config(&g));
 adc_oneshot_unit_init_cfg_t u={.unit_id=ADC_UNIT_1};ESP_ERROR_CHECK(adc_oneshot_new_unit(&u,&adc));
 adc_oneshot_chan_cfg_t ch={.bitwidth=ADC_BITWIDTH_DEFAULT,.atten=ADC_ATTEN_DB_12};
 ESP_ERROR_CHECK(adc_oneshot_config_channel(adc,ADC_CHANNEL_5,&ch));
 adc_cali_curve_fitting_config_t cc={.unit_id=ADC_UNIT_1,.chan=ADC_CHANNEL_5,.atten=ADC_ATTEN_DB_12,.bitwidth=ADC_BITWIDTH_DEFAULT};
 if(adc_cali_create_scheme_curve_fitting(&cc,&cal)!=ESP_OK){cal=NULL;ESP_LOGE("board","ADC calibration unavailable: SWD locked");}
}
int board_vref_mv(void){
 if(!cal){return -1;}
 int sum=0;
 for(int i=0;i<16;i++){int raw,mv;if(adc_oneshot_read(adc,ADC_CHANNEL_5,&raw)!=ESP_OK||adc_cali_raw_to_voltage(cal,raw,&mv)!=ESP_OK)return -1;sum+=mv;}
 return sum/8; /* average 16 samples, restore 100k:100k divider */
}
int board_power(bool on){
 if(!on){board_safe();return 0;}
 /* Never drive a target rail already powered externally. */
 int mv=board_vref_mv();
 if(mv<0||mv>400)return -1;
 gpio_set_level(9,1);target_power_enabled=true;vTaskDelay(pdMS_TO_TICKS(100));
 if(board_fault()){board_safe();return -2;}return 0;
}

bool board_power_enabled(void){return target_power_enabled;}
