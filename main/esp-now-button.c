#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_now.h"
#include "esp_crc.h"
#include "esp_system.h"
#include <stdio.h>
#include "driver/gpio.h"
#include "../../esp-now-gw/main/esp_msg_types.h"

#define QUEUE_SIZE     20
#define GPIO_BUTTON    13
#define GPIO_BUTTON_SEL (1ULL<<GPIO_BUTTON)
#define ESP_INTR_FLAG_DEFAULT 0

const uint8_t my_mac[ESP_NOW_ETH_ALEN] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x00 };
const uint8_t gw_mac[ESP_NOW_ETH_ALEN] = { 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x00 };
const char* PROG = "button";
static xQueueHandle button_queue;
int64_t last_button_press = 0;
// esp_timer_get_time returns usec, not millis
const uint64_t debounce_delay = 100 * 1000;

#define TAG "button-00"

uint8_t read_mac[ESP_NOW_ETH_ALEN];

static void my_espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS ) {
        ESP_LOGI(PROG, "ESP_NOW_SEND callback, status SUCCESS ");
        ESP_LOGI(PROG, "Mac addr sent %x:%x:%x %x:%x:%x",
                 mac_addr[0], mac_addr[1], mac_addr[2],
                 mac_addr[3], mac_addr[4], mac_addr[5]);
    } else {
        ESP_LOGI(PROG, "ESP_NOW_SEND callback, status FAILURE");
    }
}

static void my_espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len) {
    esp_now_message_t *recv_msg;
    recv_msg = (esp_now_message_t *) data;
    switch(recv_msg->type) {
    case CONFIRM:
        ESP_LOGI(PROG, "Received confirm message");
        confirm_message_t confirm_msg;
        memcpy(&confirm_msg, recv_msg->message, sizeof(confirm_message_t));
        ESP_LOGI(PROG, "Confirm status %u", confirm_msg.confirmed);
        break;
    default:
        ESP_LOGI(PROG, "Received unknown message");
        break;
    }
}

static void esp_init(void) {
    esp_now_message_t *msg;
    const TickType_t delay = 10000 / portTICK_PERIOD_MS;

    esp_now_init();
    esp_now_register_send_cb(my_espnow_send_cb);
    esp_now_register_recv_cb(my_espnow_recv_cb);

    // Add gw-peer
    esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = 10;
    peer->ifidx = WIFI_IF_STA;
    peer->encrypt = false;
    memcpy(peer->peer_addr, gw_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_add_peer(peer));

    msg = malloc((size_t) sizeof(esp_now_message_t));
    msg->type=REGISTER;
    register_message_t my_reg_msg;
    my_reg_msg.type = BUTTON;
    strcpy((char *) my_reg_msg.tag, TAG);
    memcpy((void *) my_reg_msg.mac, my_mac, ESP_NOW_ETH_ALEN);
    msg->len=sizeof(register_message_t);
    memcpy(msg->message, &my_reg_msg, msg->len);
    ESP_LOGI(PROG,"Sending message of type %u and size %u to gw", msg->type, msg->len);
    esp_now_send(gw_mac, (const uint8_t *) msg, sizeof(esp_now_message_t));
    vTaskDelay(delay);
    free(peer);
}


static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    static uint8_t u = 1;

    uint64_t now = esp_timer_get_time();
    if ( now - last_button_press > debounce_delay) {
        xQueueSendFromISR(button_queue, &u , NULL);
        last_button_press = now;
    }
}

static void send_task(void *foo) {

    esp_now_message_t smsg;
    button_press_message_t bmsg;
    uint8_t asdf;
    ESP_LOGI(PROG, "Pulling");
    for(;;) {
        if (xQueueReceive( button_queue, &(asdf), 10000 / portTICK_PERIOD_MS) == pdPASS) {
            ESP_LOGI(PROG, "Pulled message");
            smsg.len = sizeof(button_press_message_t);
            smsg.type = BUTTON_PRESSED;
            strcpy((char *) bmsg.sender_tag, TAG);
            bmsg.buttons_pressed[0] = 1;
            for (int i=1 ; i < MAX_BUTTONS ; i++) {
                bmsg.buttons_pressed[i] = 0;
            }
            memcpy(smsg.message, &bmsg, sizeof(button_press_message_t));
            esp_now_send(gw_mac, (const uint8_t *) &smsg, sizeof(esp_now_message_t));
        } else {
            ESP_LOGI(PROG, "Failed to pul message");
        }
    }
}

void gpio_init() {
    gpio_config_t io_conf;

    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.pin_bit_mask = GPIO_BUTTON_SEL;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 1;
    io_conf.pull_down_en = 0;
    gpio_config(&io_conf);
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(GPIO_BUTTON, gpio_isr_handler, (void*) NULL);
}

void app_main(void)
{

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );
    ESP_ERROR_CHECK(esp_base_mac_addr_set(my_mac));
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_start());
    ESP_ERROR_CHECK( esp_wifi_set_channel(10,1));
    ESP_ERROR_CHECK(esp_read_mac(read_mac, WIFI_IF_STA));
    ESP_LOGI(PROG, "read mac-address %x:%x:%x %x:%x:%x", read_mac[0], read_mac[1],read_mac[2],read_mac[3],read_mac[4],read_mac[5]);
    button_queue =xQueueCreate(10, sizeof(uint8_t));
    esp_init();
    gpio_init();
    xTaskCreate(send_task, "esp_now_send_task", 64*1024, NULL, 1, NULL);
}