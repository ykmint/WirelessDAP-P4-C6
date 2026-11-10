#include "server.h"
#include "board.h"
#include "interfaces.h"
#include "DAP_config.h"
#include "DAP.h"
#include "sdkconfig.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static SemaphoreHandle_t mutex;
static int current_owner;
static bool maintenance;
static bool hid_target_auto;
static uint32_t sequence;
static int64_t last_seen;
static uint16_t get16(const uint8_t*p){return p[0]|(p[1]<<8);}
static void release_locked(void){interfaces_close();board_safe();PORT_OFF();current_owner=0;}
int wd_enter_maintenance(void){xSemaphoreTake(mutex,portMAX_DELAY);int busy=current_owner!=0;if(!busy){maintenance=true;board_safe();}xSemaphoreGive(mutex);return busy;}
void wd_release(int owner){xSemaphoreTake(mutex,portMAX_DELAY);if(current_owner==owner)release_locked();xSemaphoreGive(mutex);}
static bool dap_valid(const uint8_t *p){
 switch(p[0]){
 case 0:case 1:case 2:case 3:case 7:case 8:case 0x10:case 0x11:case 0x12:case 0x13:return true;
 case 4:return get16(p+2)<=100 && get16(p+4)<=100;
 case 0x1d:{int off=2,out=2;for(int i=0;i<p[1];i++){if(off>=64)return false;int v=p[off++],n=v&63;if(!n)n=64;if(v&128)out+=(n+7)/8;else off+=(n+7)/8;if(off>64||out>64)return false;}return true;}
 case 5:{ /* validate variable transfer input and worst case response */
   int off=3,out=3;
   for(int i=0;i<p[2];i++){
    if(off>=64)return false;
    uint8_t rq=p[off++];
    if(!(rq&2)||(rq&0x10))off+=4;
    if((rq&2)&&!(rq&0x10))out+=4;
    if(rq&0x80)out+=4; /* conservative timestamp bound */
    if(off>64||out>64)return false;
   }return true;
 }
 case 6:{int count=get16(p+2);return count<=15 && ((p[4]&2)||5+count*4<=64);}
 default:return false; /* no unbounded ExecuteCommands or unsupported SWO/JTAG */
 }
}
size_t wd_process(int owner,const uint8_t *in,size_t n,uint8_t *out){
 if(n<10||in[0]!='W'||in[1]!='D'||in[2]!=1||get16(in+8)!=n-10||n>WD_MAX)return 0;
 memcpy(out,in,10);out[3]|=128;out[10]=0;size_t len=1;const uint8_t*p=in+10;size_t size=n-10;
 xSemaphoreTake(mutex,portMAX_DELAY);
 if(current_owner && esp_timer_get_time()-last_seen>60000000)release_locked();
 if(in[3]==1){
   size_t tlen=strlen(CONFIG_WD_TOKEN);unsigned diff=size^tlen;
   for(size_t i=0;i<tlen;i++)diff|=(i<size?p[i]:0)^CONFIG_WD_TOKEN[i];
   if(maintenance)out[10]=2;
   else if(diff)out[10]=1;
   else if(current_owner && current_owner!=owner)out[10]=2;
   else {sequence=in[4]|(uint32_t)in[5]<<8|(uint32_t)in[6]<<16|(uint32_t)in[7]<<24;current_owner=owner;last_seen=esp_timer_get_time();DAP_Setup();}
 }else if(current_owner!=owner){out[10]=1;}
 else {
  uint32_t seq=in[4]|(uint32_t)in[5]<<8|(uint32_t)in[6]<<16|(uint32_t)in[7]<<24;
  if(seq<=sequence){out[10]=3;goto done;}sequence=seq;
  last_seen=esp_timer_get_time();
  if(in[3]>=16&&in[3]<=22){size_t count=0;out[10]=interfaces_request(in[3],p,size,out+11,&count);len+=count;}
  else if(in[3]==24){
   if(size>1||(size==1&&p[0]>1))out[10]=3;
   else {if(size){nvs_handle_t h;if(nvs_open("wd",NVS_READWRITE,&h)!=ESP_OK)out[10]=5;else{esp_err_t e=nvs_set_u8(h,"hid_power",p[0]);if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);if(e==ESP_OK)hid_target_auto=p[0];else out[10]=5;}}out[11]=hid_target_auto;len++;}
  }
  else if(in[3]==23){if(size)out[10]=3;else release_locked();}
  else if(in[3]==2){
   if(size!=64||!dap_valid(p)){out[10]=3;}
   else {
    int mv=board_vref_mv();
    bool io=p[0]!=0 && p[0]!=1 && p[0]!=3 && p[0]!=4 && p[0]!=0x11 && p[0]!=0x13;
    if(io && (mv<2700||mv>3600||board_fault())){board_safe();out[10]=4;}
    else {
     uint8_t req[2048]={0},resp[2048]={0};memcpy(req,p,64);
     /* Bound bit-bang clock to <=100 kHz, slow path has >=5 us half-cycle. */
     if(req[0]==0x11){uint32_t hz=req[1]|req[2]<<8|req[3]<<16|req[4]<<24;if(hz<10000||hz>100000){out[10]=3;goto done;}}
     uint32_t r=DAP_ExecuteCommand(req,resp)&0xffff;
     DAP_Data.fast_clock=0;if(DAP_Data.clock_delay<5)DAP_Data.clock_delay=5;
     if(r>64){out[10]=3;}else{memcpy(out+11,resp,r);len+=r;}
    }
   }
  }else if(in[3]==3){
   len+=snprintf((char*)out+11,200,"{\"vref_mv\":%d,\"fault\":%d,\"target_power\":%d,\"reset_pin\":false,\"max_swd_hz\":100000,\"executor_core\":%d,\"stack_free_bytes\":%u}",board_vref_mv(),board_fault(),board_power_enabled(),(int)xPortGetCoreID(),(unsigned)uxTaskGetStackHighWaterMark(NULL));
  }else if(in[3]==4){if(size!=1||p[0]>1)out[10]=3;else if(!p[0]){interfaces_close();board_power(false);}else if(board_power(p[0])!=0)out[10]=4;}
  else out[10]=3;
 }
 done:
 out[8]=len;out[9]=len>>8;xSemaphoreGive(mutex);return 10+len;
}
static bool io_all(int fd,uint8_t*b,size_t n,bool send_data){
 while(n){int r=send_data?send(fd,b,n,0):recv(fd,b,n,0);if(r<=0)return false;b+=r;n-=r;}return true;
}
// Keep command workers on one core for predictable peripheral interrupt ownership.
static void tcp_task(void *unused){
 int s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);int reuse=1;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
 struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(6123),.sin_addr.s_addr=htonl(INADDR_ANY)};
 if(bind(s,(void*)&a,sizeof(a))||listen(s,1)){close(s);vTaskDelete(NULL);return;}
 while(1){
  int fd=accept(s,NULL,NULL);if(fd<0){vTaskDelay(pdMS_TO_TICKS(100));continue;}
  struct timeval tv={.tv_sec=15};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
  uint8_t in[WD_MAX],out[WD_MAX];
  while(io_all(fd,in,10,false)){
   size_t n=get16(in+8);if(n>256||!io_all(fd,in+10,n,false))break;
   size_t r=wd_process(1,in,n+10,out);if(!r||!io_all(fd,out,r,true))break;
  }
  wd_release(1);shutdown(fd,SHUT_RDWR);close(fd);
 }
}
static void guard_task(void *unused){while(1){vTaskDelay(pdMS_TO_TICKS(100));xSemaphoreTake(mutex,portMAX_DELAY);interfaces_guard();if(current_owner&&(board_fault()||esp_timer_get_time()-last_seen>60000000))release_locked();xSemaphoreGive(mutex);}}
void server_init(void){nvs_handle_t h;uint8_t automatic=0;if(nvs_open("wd",NVS_READONLY,&h)==ESP_OK){nvs_get_u8(h,"hid_power",&automatic);nvs_close(h);}hid_target_auto=automatic==1;mutex=xSemaphoreCreateMutex();interfaces_close();DAP_Setup();xTaskCreatePinnedToCore(tcp_task,"dap_tcp",16384,NULL,5,NULL,1);xTaskCreatePinnedToCore(guard_task,"power_guard",4096,NULL,6,NULL,1);}

void wd_usb_dap(const uint8_t*request,uint8_t*response){
 static uint32_t seq=0;uint8_t in[WD_MAX]={ 'W','D',1,2 },out[WD_MAX];
 uint8_t p[64];memcpy(p,request,64);
 if(p[0]==0x11){uint32_t hz=p[1]|(uint32_t)p[2]<<8|(uint32_t)p[3]<<16|(uint32_t)p[4]<<24;if(hz>100000||hz<10000){p[1]=0xa0;p[2]=0x86;p[3]=1;p[4]=0;}}
 if(p[0]==4){p[2]=p[4]=80;p[3]=p[5]=0;}
 for(int attempt=0;attempt<2;attempt++){
  ++seq;memcpy(in+4,&seq,4);in[3]=2;in[8]=64;in[9]=0;memcpy(in+10,p,64);
  if(p[0]==2&&hid_target_auto&&board_vref_mv()<400){
   in[3]=4;in[8]=1;in[10]=1;size_t powered=wd_process(4,in,11,out);
   if(powered>=11&&out[10]==0){++seq;memcpy(in+4,&seq,4);}
   in[3]=2;in[8]=64;memcpy(in+10,p,64);
  }
  size_t n=wd_process(4,in,74,out);
  if(n>=11&&out[10]==0){memcpy(response,out+11,n-11);if(p[0]==3)wd_release(4);return;}
  if(attempt==0&&n>=11&&out[10]==1){size_t t=strlen(CONFIG_WD_TOKEN);in[3]=1;in[8]=t;in[9]=0;++seq;memcpy(in+4,&seq,4);memcpy(in+10,CONFIG_WD_TOKEN,t);wd_process(4,in,10+t,out);continue;}
  break;
 }
 response[0]=request[0];response[1]=0xff;
}
