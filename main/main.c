#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include "esp_wifi_he.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

// REPLACE THESE WITH YOUR WI-FI CREDENTIALS


static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define MQTT_CONNECTED_BIT BIT1
#define MQTT_PUBLISHED_BIT BIT2

static const char *TAG = "WIFI6_TWT_LOGGER";

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected. Retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        // Signal that we are connected!
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ====================================================================
// Power Management Initialization
// ====================================================================
void enable_automatic_light_sleep(void) {
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160, 
        .min_freq_mhz = 40,  // Drop CPU to 40MHz or XTAL when idle
        .light_sleep_enable = true // The magic switch
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    ESP_LOGI(TAG, "Automatic Light Sleep Enabled.");
}

// ====================================================================
// Wi-Fi 6 TWT Setup (Called AFTER successful Wi-Fi connection)
// ====================================================================
void negotiate_twt_with_router(void) {
    wifi_phy_mode_t phymode;
    esp_wifi_sta_get_negotiated_phymode(&phymode);
    
    if (phymode == WIFI_PHY_MODE_HE20) {
        ESP_LOGI(TAG, "Router supports Wi-Fi 6 (802.11ax). Negotiating TWT...");
        
        // Configure Individual TWT (iTWT)
        wifi_twt_setup_config_t twt_config = {
            .setup_cmd = TWT_REQUEST,
            .trigger = 1,       
            .flow_id = 0,
            .twt_id = 0,
            .wake_invl_expn = 10,
            .wake_invl_mant = 5000,
            .min_wake_dura = 255,
            .timeout_time_ms = 1000
        };
        
        esp_err_t err = esp_wifi_sta_itwt_setup(&twt_config);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "TWT Negotiated Successfully!");
        } else {
            ESP_LOGE(TAG, "TWT Setup Failed. Error Code: %d", err);
        }
    } else {
        ESP_LOGW(TAG, "Router does not support Wi-Fi 6. Falling back to legacy Wi-Fi Power Save.");
    }
}

// ====================================================================
// The MQTT Publisher Task
// ====================================================================
void mqtt_sensor_task(void *pvParameters) {
    esp_mqtt_client_handle_t client = (esp_mqtt_client_handle_t)pvParameters;
    int dummy_humidity = 40;

    for(;;) {
        dummy_humidity++;
        char payload[32];
        sprintf(payload, "{\"humidity\": %d}", dummy_humidity);

        ESP_LOGI(TAG, "Publishing to HiveMQ: %s", payload);
        
        // Clear the old flag before publishing
        xEventGroupClearBits(s_wifi_event_group, MQTT_PUBLISHED_BIT); 
        
        esp_mqtt_client_publish(client, "portfolio/c6/sensor", payload, 0, 1, 0);

        // --- THE FIX: Wait for the delivery receipt before sleeping! ---
        xEventGroupWaitBits(s_wifi_event_group, MQTT_PUBLISHED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        ESP_LOGI(TAG, "Task sleeping for 15 seconds. Yielding to Light Sleep...");
        
        // NOW it is safe to sleep!
        vTaskDelay(pdMS_TO_TICKS(15000)); 
    }
}

// ====================================================================
// MQTT Event Handler
// ====================================================================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "=== MQTT CONNECTED TO HIVEMQ ===");
            xEventGroupSetBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "=== MQTT DISCONNECTED ===");
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "=== MQTT MESSAGE SUCCESSFULLY PUBLISHED ===");
            xEventGroupSetBits(s_wifi_event_group, MQTT_PUBLISHED_BIT);
            break;
        default:
            break;
    }
}

// ====================================================================
// Main Application
// ====================================================================
void app_main(void) {
    // 1. Initialize NVS (Required for Wi-Fi to store calibration data)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Enable Automatic Light Sleep
    enable_automatic_light_sleep();

    // 3. INITIALIZE THE TCP/IP STACK (This fixes your crash!)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    s_wifi_event_group = xEventGroupCreate();

    // 4. Configure Wi-Fi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    // Force the Wi-Fi driver to allow 802.11ax (Wi-Fi 6) connections
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX);
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_ERROR_CHECK(esp_wifi_start());

    // 2. Wait for the router to assign an IP address
    ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "IP received! Starting MQTT Client...");

    // 3. Start MQTT Client (Still at full power!)
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://broker.hivemq.com",
        .session.keepalive = 600, 
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    // 4. WAIT for HiveMQ to accept the connection
    ESP_LOGI(TAG, "Waiting for MQTT to connect...");
    xEventGroupWaitBits(s_wifi_event_group, MQTT_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    // 5. THE HANDSHAKES ARE DONE. NOW turn on the aggressive power saving!
    ESP_LOGI(TAG, "Network secure. Enabling Max Modem Power Save & TWT.");
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    negotiate_twt_with_router();

    // 6. Spin up our sensor task
    xTaskCreate(mqtt_sensor_task, "mqtt_task", 4096, (void*)client, 5, NULL);
}