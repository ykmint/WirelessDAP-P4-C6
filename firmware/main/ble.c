#include "server.h"
#include "sdkconfig.h"
#if !CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE || !CONFIG_ESP_HOSTED_NIMBLE_HCI_VHCI
#error "BLE must use ESP-Hosted VHCI over SDIO, not UART"
#endif
#include "esp_hosted.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>
#define UUID(n) BLE_UUID128_INIT(0x10,0xc6,0x7d,0xe8,0x42,0xf7,0x5e,0x9a,0x4d,0x4e,0x7e,0x0d,n,0x00,0x5a,0x96)
static const ble_uuid128_t service=UUID(1),rx_uuid=UUID(2),tx_uuid=UUID(3);
static uint8_t address_type;
static int owner=100,connected;
static uint8_t input[WD_MAX],reply[WD_MAX];static size_t used,reply_len;
static int64_t fragment_time;
static SemaphoreHandle_t lock;
typedef struct {int owner;size_t n;uint8_t frame[WD_MAX];} job_t;
static QueueHandle_t jobs;
static void advertise(void);
static int access_cb(uint16_t c,uint16_t a,struct ble_gatt_access_ctxt*ctx,void*arg){
 int rc=0;xSemaphoreTake(lock,portMAX_DELAY);
 if(ctx->op==BLE_GATT_ACCESS_OP_READ_CHR){if(reply_len&&os_mbuf_append(ctx->om,reply,reply_len))rc=BLE_ATT_ERR_INSUFFICIENT_RES;}
 else if(ctx->op==BLE_GATT_ACCESS_OP_WRITE_CHR){
  size_t count=OS_MBUF_PKTLEN(ctx->om);if(esp_timer_get_time()-fragment_time>5000000)used=0;fragment_time=esp_timer_get_time();
  if(count>WD_MAX-used){used=0;rc=BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;goto done;}
  uint16_t got;ble_hs_mbuf_to_flat(ctx->om,input+used,count,&got);used+=got;
  if(used>=10){size_t total=10+input[8]+(input[9]<<8);
   if(input[0]!='W'||input[1]!='D'||input[2]!=1||total>WD_MAX||used>total){used=0;rc=BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;goto done;}
   if(used==total){job_t j={.owner=owner,.n=used};memcpy(j.frame,input,used);used=0;if(xQueueSend(jobs,&j,0)!=pdTRUE)rc=BLE_ATT_ERR_INSUFFICIENT_RES;}
  }
 }else rc=BLE_ATT_ERR_UNLIKELY;
 done:xSemaphoreGive(lock);return rc;
}
static const struct ble_gatt_svc_def services[]={
 {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&service.u,.characteristics=(struct ble_gatt_chr_def[]){
  {.uuid=&rx_uuid.u,.access_cb=access_cb,.flags=BLE_GATT_CHR_F_WRITE},
  {.uuid=&tx_uuid.u,.access_cb=access_cb,.flags=BLE_GATT_CHR_F_READ},{0}}},{0}};
static int gap_event(struct ble_gap_event*e,void*arg){
 if(e->type==BLE_GAP_EVENT_CONNECT){if(e->connect.status==0){xSemaphoreTake(lock,portMAX_DELAY);connected=1;owner++;used=reply_len=0;xSemaphoreGive(lock);}else advertise();}
 if(e->type==BLE_GAP_EVENT_DISCONNECT){xSemaphoreTake(lock,portMAX_DELAY);int old=owner;connected=0;owner++;used=reply_len=0;xSemaphoreGive(lock);wd_release(old);advertise();}
 if(e->type==BLE_GAP_EVENT_ADV_COMPLETE){advertise();}
 return 0;
}
static void advertise(void){
 struct ble_hs_adv_fields f={0};f.flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP;f.uuids128=(ble_uuid128_t*)&service;f.num_uuids128=1;f.uuids128_is_complete=1;
 ble_gap_adv_set_fields(&f);struct ble_hs_adv_fields scan={0};scan.name=(uint8_t*)"WirelessDAP";scan.name_len=11;scan.name_is_complete=1;ble_gap_adv_rsp_set_fields(&scan);
 struct ble_gap_adv_params params={.conn_mode=BLE_GAP_CONN_MODE_UND,.disc_mode=BLE_GAP_DISC_MODE_GEN};int rc=ble_gap_adv_start(address_type,NULL,BLE_HS_FOREVER,&params,gap_event,NULL);if(rc)ESP_LOGE("ble","Advertising failed: %d",rc);else ESP_LOGI("ble","BLE_READY: WirelessDAP advertising");
}
static void sync_cb(void){int rc=ble_hs_util_ensure_addr(0);if(!rc)rc=ble_hs_id_infer_auto(0,&address_type);if(!rc)advertise();else ESP_LOGE("ble","BLE address initialization failed: %d",rc);}
static void host_task(void*p){nimble_port_run();nimble_port_freertos_deinit();}
static void worker(void*p){job_t j;while(1){xQueueReceive(jobs,&j,portMAX_DELAY);xSemaphoreTake(lock,portMAX_DELAY);bool valid=connected&&owner==j.owner;xSemaphoreGive(lock);if(!valid)continue;
 uint8_t out[WD_MAX];size_t n=wd_process(j.owner,j.frame,j.n,out);xSemaphoreTake(lock,portMAX_DELAY);valid=connected&&owner==j.owner;if(valid){memcpy(reply,out,n);reply_len=n;}xSemaphoreGive(lock);if(!valid)wd_release(j.owner);
}}
void ble_start(void){
 lock=xSemaphoreCreateMutex();jobs=xQueueCreate(2,sizeof(job_t));
 // Older ESP-Hosted coprocessors start their HCI controller at boot and do
 // not implement FeatureControl. HCI host sync below verifies that controller.
 esp_err_t controller=esp_hosted_bt_controller_init();
 if(controller==ESP_OK){
  if(esp_hosted_bt_controller_enable()!=ESP_OK){ESP_LOGE("ble","Controller enable failed");return;}
 }else{
  ESP_LOGW("ble","Controller RPC unavailable (%s); probing boot-enabled legacy HCI",esp_err_to_name(controller));
 }
 if(nimble_port_init()!=ESP_OK){ESP_LOGE("ble","NimBLE init failed");return;}
 ble_svc_gap_init();ble_svc_gatt_init();ble_hs_cfg.sync_cb=sync_cb;
 ESP_ERROR_CHECK(ble_gatts_count_cfg(services));ESP_ERROR_CHECK(ble_gatts_add_svcs(services));ble_svc_gap_device_name_set("WirelessDAP");
 xTaskCreatePinnedToCore(worker,"ble_dap",16384,NULL,5,NULL,1);nimble_port_freertos_init(host_task);
}
