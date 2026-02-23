// #include <stdio.h>
// #include <string.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "nvs_flash.h"
// #include "esp_now.h"
// #include "esp_ieee802154.h" // Wi-Fiの代わりにこれを使う
// #include "esp_mac.h"

// 受信コールバック
// void data_receive_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
//     printf("H2で受信成功！ データ: %.*s\n", len, data);
// }

// void app_main(void) {
//     // 1. NVS初期化
//     esp_err_t ret = nvs_flash_init();
//     if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
//         nvs_flash_erase();
//         nvs_flash_init();
//     }

//     // 2. IEEE 802.15.4 の初期化 (H2ではWi-Fiの代わり)
//     // 注意: idf.py menuconfig で Component config -> ESP-NOW -> ESP-NOW over IEEE802.15.4 を有効にする必要があります
    
//     // ESP-NOWの初期化
//     ESP_ERROR_CHECK(esp_now_init());

//     // 3. コールバック登録
//     ESP_ERROR_CHECK(esp_now_register_recv_cb(data_receive_cb));

//     printf("ESP32-H2 受信待機中 (802.15.4モード)...\n");

//     while (1) {
//         vTaskDelay(pdMS_TO_TICKS(1000));
//     }
// }


// #include <stdio.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "driver/gpio.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_now.h"

// 1. データを受信したときに実行される関数（コールバック）
void data_receive_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    // 送信元のMACアドレスを表示
    printf("受信成功！ 送信元: ");
    for (int i = 0; i < 6; i++) {
        printf("%02X%s", recv_info->src_addr[i], (i == 5) ? "" : ":");
    }

    // 届いたデータを文字列として表示
    printf(" | データ内容: %.*s\n", len, data);
}

void app_main(void)
{
    // 2. NVSの初期化（必須）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // 3. Wi-Fiの初期化
    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA); 
    esp_wifi_start();

    // 4. ESP-NOWの初期化
    if (esp_now_init() != ESP_OK) {
        printf("ESP-NOWの初期化に失敗しました\n");
        return;
    }

    // 5. 受信コールバック関数を登録
    esp_now_register_recv_cb(data_receive_cb);

    printf("受信待機中...\n");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// 使用するGPIOピンの番号（ボードに合わせて変更してください）
// #define BLINK_GPIO 2

// void app_main(void)
// {
// // gpio_reset_pin(BLINK_GPIO);                      // ピンをリセット
// //     gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT); // 出力モードに設定

// //     printf("Lチカを開始します（10秒間隔）\n");

//     while (1) {
//         // 2. LEDをONにする
//         // printf("LED ON\n");
//         // gpio_set_level(BLINK_GPIO, 1);
//         // vTaskDelay(10000 / portTICK_PERIOD_MS); // 10,000ミリ秒（10秒）待機

//         // // 3. LEDをOFFにする
//         // printf("LED OFF\n");
//         // gpio_set_level(BLINK_GPIO, 0);
//         // vTaskDelay(10000 / portTICK_PERIOD_MS); // 10,000ミリ秒（10秒）待機
//     }
// }
