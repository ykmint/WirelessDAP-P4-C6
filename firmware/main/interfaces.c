#include "interfaces.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
typedef struct {
 uint8_t mode;
 uint32_t rate,rx_dropped;
 int uart_port;
 bool uart_installed;
 QueueHandle_t uart_events;
 i2c_master_bus_handle_t i2c_bus;
 spi_device_handle_t spi_dev;
 bool spi_bus_up;
 twai_node_handle_t can_node;
 bool can_enabled;
 QueueHandle_t can_rx;
 SemaphoreHandle_t can_done;
 bool can_tx_ok;
 uint8_t can_data[8];
 twai_frame_t can_tx;
} interface_state;
static interface_state ports[7];
typedef struct{uint8_t data[14];} can_packet;
static uint32_t rd32(const uint8_t*p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void wr32(uint8_t*p,uint32_t x){for(int i=0;i<4;i++)p[i]=x>>(8*i);}
static bool ttl(uint8_t m){return m==1||m==2||m==4||m==5;}
static bool voltage_ok(void){int mv=board_vref_mv();return mv>=2700&&mv<=3600&&!board_fault();}
static void release_pins(unsigned port){
 static const int pins[7][4]={{-1,-1,-1,-1},{11,12,-1,-1},{13,23,-1,-1},{28,29,30,-1},{51,52,-1,-1},{2,3,4,5},{31,32,-1,-1}};
 for(unsigned i=0;i<4;i++){int pin=pins[port][i];if(pin<0)continue;gpio_reset_pin(pin);gpio_pullup_dis(pin);gpio_pulldown_dis(pin);gpio_set_direction(pin,GPIO_MODE_INPUT);}
 if(port==6){gpio_set_level(31,1);gpio_set_direction(31,GPIO_MODE_OUTPUT);}
 if(port==3){gpio_set_level(30,0);gpio_set_direction(30,GPIO_MODE_OUTPUT);}
}
static void close_port(interface_state*state){
 unsigned port=(unsigned)(state-ports);
 if(state->uart_installed){uart_driver_delete(state->uart_port);state->uart_installed=false;state->uart_events=NULL;}
 if(state->i2c_bus){i2c_del_master_bus(state->i2c_bus);state->i2c_bus=NULL;}
 if(state->spi_dev){spi_bus_remove_device(state->spi_dev);state->spi_dev=NULL;}
 if(state->spi_bus_up){spi_bus_free(SPI2_HOST);state->spi_bus_up=false;}
 if(state->can_node){if(state->can_enabled)twai_node_disable(state->can_node);twai_node_delete(state->can_node);state->can_node=NULL;state->can_enabled=false;}
 if(state->can_rx){vQueueDelete(state->can_rx);state->can_rx=NULL;}
 if(state->can_done){vSemaphoreDelete(state->can_done);state->can_done=NULL;}
 state->mode=0;release_pins(port);
}
void interfaces_close(void){for(unsigned i=1;i<=6;i++)close_port(&ports[i]);}
static unsigned enabled_mask(void){unsigned mask=0;for(unsigned i=1;i<=6;i++)if(ports[i].mode)mask|=1U<<i;return mask;}
bool interfaces_active(void){return enabled_mask()!=0;}
void interfaces_guard(void){if(voltage_ok())return;for(unsigned i=1;i<=6;i++)if(ttl(i)&&ports[i].mode)close_port(&ports[i]);}
static bool on_can_rx(twai_node_handle_t h,const twai_rx_done_event_data_t*e,void*u){
 interface_state*state=u;
 uint8_t bytes[8]={0};twai_frame_t f={.buffer=bytes,.buffer_len=8};BaseType_t wake=pdFALSE;
 if(twai_node_receive_from_isr(h,&f)==ESP_OK&&f.header.dlc<=8&&!f.header.fdf){
  can_packet p={0};wr32(p.data,f.header.id);p.data[4]=f.header.ide|(f.header.rtr<<1);p.data[5]=f.header.dlc;
  if(!f.header.rtr)memcpy(p.data+6,bytes,f.header.dlc);
  if(xQueueSendFromISR(state->can_rx,&p,&wake)!=pdTRUE)state->rx_dropped++;
 }return wake==pdTRUE;
}
static bool on_can_tx(twai_node_handle_t h,const twai_tx_done_event_data_t*e,void*u){interface_state*state=u;BaseType_t wake=pdFALSE;state->can_tx_ok=e->is_tx_success;xSemaphoreGiveFromISR(state->can_done,&wake);return wake==pdTRUE;}
static uint8_t failure(uint8_t*out,size_t*len,const char*text){*len=strlen(text);memcpy(out,text,*len);return 5;}
static uint8_t result(esp_err_t e,uint8_t*out,size_t*len){if(e==ESP_OK)return 0;return failure(out,len,esp_err_to_name(e));}
// Address-only probe through the normal transaction path, which resets the
// controller FSM after a NACK. No target data byte is written and no retry occurs.
static esp_err_t probe_address(interface_state*state,uint8_t address){
 // A completed controller transaction can precede the physical STOP rising edge.
 // Wait for both lines and a full bus-free interval before issuing the next START.
 int64_t deadline=esp_timer_get_time()+10000;
 for(;;){
  if(gpio_get_level(52)&&gpio_get_level(51)){
   esp_rom_delay_us(5);
   if(gpio_get_level(52)&&gpio_get_level(51))break;
  }
  if(esp_timer_get_time()>=deadline)return ESP_ERR_INVALID_STATE;
  esp_rom_delay_us(10);
 }
 i2c_master_dev_handle_t dev=NULL;
 i2c_device_config_t cfg={.device_address=I2C_DEVICE_ADDRESS_NOT_USED,.dev_addr_length=I2C_ADDR_BIT_LEN_7,.scl_speed_hz=state->rate,.scl_wait_us=10000};
 esp_err_t e=i2c_master_bus_add_device(state->i2c_bus,&cfg,&dev);if(e!=ESP_OK)return e;
 uint8_t byte=address<<1;
 i2c_operation_job_t ops[]={
  {.command=I2C_MASTER_CMD_START},
  {.command=I2C_MASTER_CMD_WRITE,.write={.ack_check=true,.data=&byte,.total_bytes=1}},
  {.command=I2C_MASTER_CMD_STOP}
 };
 e=i2c_master_execute_defined_operations(dev,ops,3,50);
 i2c_master_bus_rm_device(dev);
 return e==ESP_ERR_INVALID_RESPONSE?ESP_ERR_NOT_FOUND:e;
}
static bool can_rate(uint32_t r){return r==10000||r==20000||r==50000||r==100000||r==125000||r==250000||r==500000||r==800000||r==1000000;}
static esp_err_t configure(interface_state*state,const uint8_t*p){
 uint8_t m=p[0],a=p[5],b=p[6],c=p[7];uint32_t hz=rd32(p+1);esp_err_t e;
 if(m>6)return ESP_ERR_INVALID_ARG;
 if(m>=1&&m<=3&&(hz<300||hz>1000000||a>2||b<5||b>8||(c!=1&&c!=2)))return ESP_ERR_INVALID_ARG;
 if(m==4&&(hz<10000||hz>400000||a||b||c))return ESP_ERR_INVALID_ARG;
 if(m==5&&(hz<10000||hz>10000000||a>3||b>1||c))return ESP_ERR_INVALID_ARG;
 if(m==6&&(!can_rate(hz)||a>2||b||c))return ESP_ERR_INVALID_ARG;
 if(ttl(m)&&!voltage_ok())return ESP_ERR_INVALID_STATE;
 close_port(state);state->rx_dropped=0;state->rate=hz;
 if(!m)return ESP_OK;
 #define TRY(x) do{e=(x);if(e!=ESP_OK)goto fail;}while(0)
 if(m<=3){
  int port=m==3?UART_NUM_0:(m==1?UART_NUM_1:UART_NUM_2);int tx=m==1?11:m==2?13:28,rx=m==1?12:m==2?23:29;
  uart_config_t cfg={.baud_rate=hz,.data_bits=(uart_word_length_t)(b-5),.parity=a==0?UART_PARITY_DISABLE:a==1?UART_PARITY_ODD:UART_PARITY_EVEN,.stop_bits=c==1?UART_STOP_BITS_1:UART_STOP_BITS_2,.flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
  TRY(uart_driver_install(port,4096,0,32,&state->uart_events,0));state->uart_port=port;state->uart_installed=true;TRY(uart_param_config(port,&cfg));TRY(uart_set_pin(port,tx,rx,m==3?30:UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE));
  if(m==3)TRY(uart_set_mode(port,UART_MODE_RS485_HALF_DUPLEX));
 }else if(m==4){
  i2c_master_bus_config_t cfg={.i2c_port=-1,.sda_io_num=52,.scl_io_num=51,.clk_source=I2C_CLK_SRC_DEFAULT,.glitch_ignore_cnt=7};
  cfg.flags.enable_internal_pullup=true;
  TRY(i2c_new_master_bus(&cfg,&state->i2c_bus));
 }else if(m==5){
  spi_bus_config_t bus={.mosi_io_num=3,.miso_io_num=4,.sclk_io_num=2,.quadwp_io_num=-1,.quadhd_io_num=-1,.data4_io_num=-1,.data5_io_num=-1,.data6_io_num=-1,.data7_io_num=-1,.max_transfer_sz=240};
  TRY(spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO));state->spi_bus_up=true;
  spi_device_interface_config_t cfg={.clock_speed_hz=hz,.mode=a,.spics_io_num=5,.queue_size=1,.flags=b?SPI_DEVICE_BIT_LSBFIRST:0};
  TRY(spi_bus_add_device(SPI2_HOST,&cfg,&state->spi_dev));
 }else{
  state->can_rx=xQueueCreate(64,sizeof(can_packet));state->can_done=xSemaphoreCreateBinary();if(!state->can_rx||!state->can_done){e=ESP_ERR_NO_MEM;goto fail;}
  twai_onchip_node_config_t cfg={.io_cfg={.tx=31,.rx=32,.quanta_clk_out=-1,.bus_off_indicator=-1},.bit_timing={.bitrate=hz},.fail_retry_cnt=0,.tx_queue_depth=1};
  cfg.flags.enable_listen_only=a==1;cfg.flags.enable_self_test=a==2;cfg.flags.enable_loopback=a==2;
  TRY(twai_new_node_onchip(&cfg,&state->can_node));twai_event_callbacks_t cb={.on_rx_done=on_can_rx,.on_tx_done=on_can_tx};TRY(twai_node_register_event_callbacks(state->can_node,&cb,state));
  TRY(twai_node_enable(state->can_node));state->can_enabled=true;
 }
 state->mode=m;return ESP_OK;
 fail:close_port(state);return e;
 #undef TRY
}
uint8_t interfaces_request(uint8_t op,const uint8_t*p,size_t n,uint8_t*out,size_t*len){
 *len=0;
 if(op==16){if(n)return 3;*len=sprintf((char*)out,"{\"j1_api\":2,\"parallel\":true,\"modes\":[\"UART1\",\"UART2\",\"RS485\",\"I2C\",\"SPI\",\"CAN\"],\"max_bytes\":240,\"can_fd\":false,\"power_5v_switch\":false}");return 0;}
 if(op==17){if(n!=8||p[0]>6)return 3;if(!p[0]){interfaces_close();return 0;}return result(configure(&ports[p[0]],p),out,len);}
 if(op==22){if(n!=1||p[0]<1||p[0]>6)return 3;close_port(&ports[p[0]]);return 0;}
 if(op==21&&n==0){*len=snprintf((char*)out,255,"{\"enabled_mask\":%u,\"parallel\":true}",enabled_mask());return 0;}
 if(n<1||p[0]<1||p[0]>6)return 3;
 interface_state*state=&ports[p[0]];p++;n--;
 if(op==21){
  if(n)return 3;
  if(state->uart_events){uart_event_t ev;while(xQueueReceive(state->uart_events,&ev,0)==pdTRUE)if(ev.type==UART_FIFO_OVF||ev.type==UART_BUFFER_FULL||ev.type==UART_PARITY_ERR||ev.type==UART_FRAME_ERR)state->rx_dropped++;}
  twai_node_status_t st={0};twai_node_record_t rec={0};if(state->can_node)twai_node_get_info(state->can_node,&st,&rec);
  *len=snprintf((char*)out,255,"{\"mode\":%u,\"rate\":%lu,\"rx_errors\":%lu,\"can_state\":%d,\"can_tx_errors\":%u,\"can_rx_errors\":%u,\"bus_errors\":%lu}",state->mode,(unsigned long)state->rate,(unsigned long)state->rx_dropped,st.state,st.tx_error_count,st.rx_error_count,(unsigned long)rec.bus_err_num);return 0;
 }
 if(!state->mode)return failure(out,len,"Interface is closed; apply configuration first");
 if(ttl(state->mode)&&!voltage_ok()){close_port(state);return failure(out,len,"VREF must be 2.7-3.6V; interface closed");}
 if(op==19){
  if(n)return 3;
  if(state->mode<=3){int r=uart_read_bytes(state->uart_port,out,240,0);if(r<0)return failure(out,len,"UART receive failed");*len=r;return 0;}
  if(state->mode==6){can_packet f;while(*len+14<=240&&xQueueReceive(state->can_rx,&f,0)==pdTRUE){memcpy(out+*len,f.data,14);*len+=14;}return 0;}
  return failure(out,len,"This mode uses synchronous transfer");
 }
 if(op==20){
  if(state->mode!=4||n!=2||p[0]<8||p[1]>0x77||p[1]<p[0]||p[1]-p[0]>=16)return 3;
  for(int a=p[0];a<=p[1];a++){esp_err_t e=probe_address(state,a);if(e==ESP_OK)out[(*len)++]=a;else if(e!=ESP_ERR_NOT_FOUND)return result(e,out,len);}return 0;
 }
 if(op!=18)return 3;
 if(state->mode<=3){
  if(!n||n>240)return 3;
  if(n*12000>state->rate*250)return failure(out,len,"UART packet exceeds 250ms; reduce bytes or increase baud");
  // With no TX ring buffer this waits for FIFO space; n is bounded.
  int r=uart_write_bytes(state->uart_port,p,n);if(r!=(int)n)return failure(out,len,"UART write failed");
  uint32_t wait=100+(n*12*1000+state->rate-1)/state->rate;esp_err_t e=uart_wait_tx_done(state->uart_port,pdMS_TO_TICKS(wait));return result(e,out,len);
 }
 if(state->mode==5){if(!n||n>240)return 3;spi_transaction_t t={.length=n*8,.tx_buffer=p,.rx_buffer=out};esp_err_t e=spi_device_polling_transmit(state->spi_dev,&t);if(e==ESP_OK)*len=n;return result(e,out,len);}
 if(state->mode==4){
  if(n<2||n>242||p[0]<8||p[0]>0x77||p[1]>240||(n==2&&p[1]==0))return 3;
  i2c_master_dev_handle_t dev=NULL;i2c_device_config_t cfg={.device_address=p[0],.dev_addr_length=I2C_ADDR_BIT_LEN_7,.scl_speed_hz=state->rate,.scl_wait_us=10000};
  esp_err_t e=i2c_master_bus_add_device(state->i2c_bus,&cfg,&dev);if(e!=ESP_OK)return result(e,out,len);
  if(n>2&&p[1])e=i2c_master_transmit_receive(dev,p+2,n-2,out,p[1],250);
  else if(n>2)e=i2c_master_transmit(dev,p+2,n-2,250);else e=i2c_master_receive(dev,out,p[1],250);
  i2c_master_bus_rm_device(dev);if(e==ESP_OK)*len=p[1];return result(e,out,len);
 }
 if(state->mode==6){
  if(n<6||p[4]>3||p[5]>8||n!=(size_t)(6+((p[4]&2)?0:p[5]))||rd32(p)>((p[4]&1)?0x1fffffff:0x7ff))return 3;
  memset(&state->can_tx,0,sizeof(state->can_tx));memset(state->can_data,0,8);state->can_tx.header.id=rd32(p);state->can_tx.header.ide=p[4]&1;state->can_tx.header.rtr=(p[4]>>1)&1;state->can_tx.header.dlc=p[5];state->can_tx.buffer=state->can_data;state->can_tx.buffer_len=state->can_tx.header.rtr?0:p[5];if(!(p[4]&2))memcpy(state->can_data,p+6,p[5]);
  xSemaphoreTake(state->can_done,0);state->can_tx_ok=false;esp_err_t e=twai_node_transmit(state->can_node,&state->can_tx,50);if(e!=ESP_OK)return result(e,out,len);
  if(xSemaphoreTake(state->can_done,pdMS_TO_TICKS(250))!=pdTRUE){close_port(state);return failure(out,len,"CAN TX timeout; interface closed; no replay");}
  if(!state->can_tx_ok){return failure(out,len,"CAN TX failed (ACK/bus error); no replay");}
  return 0;
 }
 return 3;
}
