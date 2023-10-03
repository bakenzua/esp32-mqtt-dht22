#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp32/rom/ets_sys.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "mqtt_client.h"

#include "DHT22.h"
#define DHT22_PIN CONFIG_DHT22_PIN

static const char *TAG = "esp-dht-station";

#define WIFI_SSID CONFIG_WIFI_SSID
#define WIFI_PASS CONFIG_WIFI_PASS

#define MQTT_BROKER_URL CONFIG_MQTT_BROKER_URL
#define MQTT_USERNAME CONFIG_MQTT_USERNAME
#define MQTT_PASSWORD CONFIG_MQTT_PASSWORD
#define MQTT_PUB_TEMP CONFIG_MQTT_PUB_TEMP
#define MQTT_PUB_HUM  CONFIG_MQTT_PUB_HUM

static esp_mqtt_client_handle_t mqtt_client;

/* esp netif object representing the WIFI station */
static esp_netif_t *sta_netif = NULL;

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t event_group_bits;
#define WIFI_CONNECTED_BIT BIT0
#define MQTT_CONNECTED_BIT BIT1

/**
 * WIFI and IP event handler using esp_event API.
 */
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "wifi disconnected.");
        esp_wifi_connect();
        xEventGroupClearBits(event_group_bits, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "wifi connected.");
        xEventGroupSetBits(event_group_bits, WIFI_CONNECTED_BIT);
    }
}

/**
 * MQTT event handler using esp_event API.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            xEventGroupSetBits(event_group_bits, MQTT_CONNECTED_BIT);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            xEventGroupClearBits(event_group_bits, MQTT_CONNECTED_BIT);
            xEventGroupWaitBits( // Wait for WIFI to reconnect
                event_group_bits,
                WIFI_CONNECTED_BIT,
                pdFALSE,
                pdFALSE,
                portMAX_DELAY
            );
            vTaskDelay( 5000 / portTICK_PERIOD_MS);
            ESP_ERROR_CHECK(esp_mqtt_client_reconnect(client));
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
            }
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
}

/**
 * WIFI initialization
 */
void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL) );
    ESP_ERROR_CHECK( esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );
    ESP_LOGI(TAG, "wifi_init_sta finished.");
}

/**
 * MQTT initialization
 */
static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URL,

       .credentials.username = MQTT_USERNAME,
       .credentials.authentication.password = MQTT_PASSWORD
    };
    // mqtt_cfg.credentials.username = MQTT_USERNAME;
    // mqtt_cfg.credentials.authentication.password = MQTT_PASSWORD;
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, mqtt_client);
    esp_mqtt_client_start(mqtt_client);
}

/**
 * FreeRTOS task for getting DHT22 sensor readings and
 * sending them to MQTT broker
 */
void DHT_task(void *pvParameter)
{
    setDHTgpio(DHT22_PIN);
    ESP_LOGI(TAG, "Starting DHT Task");

    char hum[10];
    char temp[10];

    while(1) {  
        xEventGroupWaitBits( // Wait for MQTT connection
            event_group_bits,
            MQTT_CONNECTED_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY
        );
        
        ESP_LOGI(TAG, "Reading DHT");
        int ret = readDHT();
        
        errorHandler(ret);

        sprintf(hum, "%.1f", getHumidity());
        sprintf(temp, "%.1f", getTemperature());
        ESP_LOGI(TAG, "Hum %s\n", hum);
        ESP_LOGI(TAG, "Tmp %s\n", temp);

        int msg_id = esp_mqtt_client_publish(mqtt_client, MQTT_PUB_HUM, hum, 0, 1, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        msg_id = esp_mqtt_client_publish(mqtt_client, MQTT_PUB_TEMP, temp, 0, 1, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        
        // -- wait at least 2 sec before reading again ------------
        // The interval of whole process must be beyond 2 seconds !! 

        // 60 seconds
        vTaskDelay( 60000 / portTICK_PERIOD_MS );
    }
}

/**
 * Main function
 */
void app_main()
{
    nvs_flash_init();
    vTaskDelay( 1000 / portTICK_PERIOD_MS );
    event_group_bits = xEventGroupCreate();
    wifi_init();
    mqtt_app_start();
    xTaskCreate( &DHT_task, "DHT_task", 2048, NULL, 5, NULL );
}