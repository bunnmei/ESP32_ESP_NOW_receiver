#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "driver/gpio.h"

static const char *TAG = "INDOOR_NODE";

#define LED_PIN 2
static uint8_t s_led_state = 0; 

// ================= 【設定エリア】 =================
#define WIFI_SSID      "TP-LINK_49D4" // ★あなたの家のWiFiのSSID
#define WIFI_PASS      "38383274"
#define WIFI_CHANNEL   6 // ★送信側と合わせたチャンネル

#define NTRIP_SERVER   "85.131.253.14"
#define NTRIP_PORT     2101
#define NTRIP_MOUNT    "/TAMBA_NOSAKA_BASE"   // 例: "Kobe_Base_F9P"
#define NTRIP_PW       "nosaka01"      // これがパスワードになります
// ==================================================

// Wi-Fi接続イベント用
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

// ESP-NOWからNTRIPへ渡すデータの構造体とキュー
typedef struct {
    int len;
    uint8_t data[250];
} rtk_chunk_t;

QueueHandle_t rtk_queue;

// --- Wi-Fiイベントハンドラ ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi disconnected. Reconnecting...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi Connected!");
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// --- ESP-NOW 受信コールバック ---
// ★重要: ここでは極力軽い処理だけ行い、キューに突っ込んで終わる
static void espnow_recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len) {
    // ★Wi-Fiが繋がっていない時はデータを捨てて、キューが溢れるのを防ぐ
    if ((xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) == 0) {
        return; 
    }

    s_led_state = !s_led_state;
    gpio_set_level(LED_PIN, s_led_state);

    rtk_chunk_t chunk;
    chunk.len = data_len;
    memcpy(chunk.data, data, data_len);
    ESP_LOGI(TAG, "Received ESP-NOW packet of length %d", data_len);

    if (xQueueSend(rtk_queue, &chunk, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Queue is full, dropping ESP-NOW packet");
    }
}
// --- NTRIP 送信タスク (TCPソケット) ---
static void ntrip_client_task(void *pvParameters) {
    char rx_buffer[128];
    char request[256];
    
    while (1) {
        // Wi-Fiが繋がるまで待つ
        xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "Connecting to RTK2GO...");

        // // IPアドレスの解決
        // struct hostent *hp = gethostbyname(NTRIP_SERVER);
        // if (!hp) {
        //     ESP_LOGE(TAG, "DNS lookup failed");
        //     vTaskDelay(5000 / portTICK_PERIOD_MS);
        //     continue;
        // }

        // struct sockaddr_in dest_addr;
        // dest_addr.sin_family = AF_INET;
        // dest_addr.sin_port = htons(NTRIP_PORT);
        // dest_addr.sin_addr.s_addr = *(uint32_t *)(hp->h_addr);

        vTaskDelay(2000 / portTICK_PERIOD_MS);

        struct sockaddr_in dest_addr;
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(NTRIP_PORT);

        // ★IPアドレス（123.45...等）直打ちでも、ドメイン名（rtk2go.com等）でも両方対応できる最強の変換処理
        if (inet_pton(AF_INET, NTRIP_SERVER, &dest_addr.sin_addr) <= 0) {
            // 数字のIPじゃなかった場合は、DNSで名前解決を試みる
            struct hostent *hp = gethostbyname(NTRIP_SERVER);
            if (!hp) {
                ESP_LOGE(TAG, "DNS lookup failed (Still resolving...)");
                vTaskDelay(5000 / portTICK_PERIOD_MS);
                continue; // 失敗しても5秒待って再挑戦する
            }
            dest_addr.sin_addr.s_addr = *(uint32_t *)(hp->h_addr);
        }

        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket");
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }

        // TCP接続
        if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
            ESP_LOGE(TAG, "Socket connect failed");
            close(sock);
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "Successfully connected to RTK2GO server!");

        // NTRIPの「合言葉」を送信
        snprintf(request, sizeof(request), 
                 "SOURCE %s %s\r\nSource-Agent: NTRIP ESP32\r\n\r\n", 
                 NTRIP_PW, NTRIP_MOUNT);
        send(sock, request, strlen(request), 0);

        // サーバーからの返事 (ICY 200 OK) を待つ
        int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
        if (len > 0) {
            rx_buffer[len] = 0;
            ESP_LOGI(TAG, "Server replied: %s", rx_buffer);
            if (strstr(rx_buffer, "ICY 200 OK") == NULL) {
                ESP_LOGE(TAG, "Password or Mountpoint rejected! Disconnecting.");
                close(sock);
                vTaskDelay(10000 / portTICK_PERIOD_MS);
                continue;
            }
        } else {
            close(sock);
            continue;
        }

        ESP_LOGI(TAG, "🚀 NTRIP Stream Started! Sending data...");
        rtk_chunk_t chunk;

        // 無限ループ：キューからRTCMデータを取り出してTCPで送る
        while (1) {
            if (xQueueReceive(rtk_queue, &chunk, portMAX_DELAY) == pdTRUE) {
                int err = send(sock, chunk.data, chunk.len, 0);
                if (err < 0) {
                    ESP_LOGE(TAG, "Error occurred during sending. Reconnecting...");
                    break; // ループを抜けて再接続へ
                }
                // ESP_LOGI(TAG, "Sent %d bytes to RTK2GO", chunk.len); // 成功ログ（うるさい場合はコメントアウト）
            }
        }

        close(sock);
    }
}



void app_main(void)
{
    // NVSの初期化
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ★LEDピンの初期設定と、最初は消灯しておく処理
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0);

    // xTaskCreate(led_blink_task, "led_blink_task", 2048, NULL, 5, &blink_task_handle);

    // キューの作成 (250バイトの塊を最大20個までストック可能)
    rtk_queue = xQueueCreate(20, sizeof(rtk_chunk_t));

    // イベントグループ作成
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // Wi-Fi初期化
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .channel = WIFI_CHANNEL,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_wifi_set_ps(WIFI_PS_NONE);

    // // ★チャンネルの強制固定
    // esp_wifi_set_promiscuous(true);
    // esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    // esp_wifi_set_promiscuous(false);

    // ESP-NOW初期化
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    // NTRIP送信タスクの起動 (スタックサイズは少し大きめに)
    xTaskCreate(ntrip_client_task, "ntrip_client_task", 8192, NULL, 5, NULL);

    
}