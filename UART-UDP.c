#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "sdkconfig.h"


#define WIFI_SSID      "UCB"
#define WIFI_PASS      ""
#define HOST_IP_ADDR   "172.18.16.3" // <--- PON LA IP DE TU LAPTOP AQUI
//#define HOST_IP_ADDR   "172.18.21.125" // <--- PON LA IP DE TU LAPTOP AQUI
#define PORT           3333

/* --- 2. CONFIGURACIÓN UART (Pines 16 y 17) --- */
#define RX_PIN         16
#define TX_PIN         17
#define UART_NUM       UART_NUM_2
#define BUF_SIZE       1024

/* Variables de estado */
static const char *TAG = "BRIDGE_APP";
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

/* --- MANEJADOR DE EVENTOS WIFI (Callback) --- */
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Reintentando conectar al AP...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Conectado! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* --- INICIALIZAR WIFI (Station Mode) --- */
void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_OPEN, // Cambiar si usas contraseña
        },
    };
    
    // Si tienes contraseña, cambia el authmode arriba a WIFI_AUTH_WPA2_PSK
    if (strlen(WIFI_PASS) > 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi iniciado. Esperando conexión...");
}

/* --- INICIALIZAR UART --- */
void init_uart(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    // Configuramos UART2
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART2 Inicializado en pines %d(RX) y %d(TX)", RX_PIN, TX_PIN);
}

/* --- TAREA PRINCIPAL (BRIDGE) --- */
void udp_client_task(void *pvParameters)
{
    char rx_buffer[128];
    char addr_str[128];
    int addr_family = AF_INET;
    int ip_protocol = 0;
    struct sockaddr_in dest_addr;

    while (1) {
        // 1. Esperar a tener WiFi
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

        // 2. Configurar Socket UDP
        dest_addr.sin_addr.s_addr = inet_addr(HOST_IP_ADDR);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(PORT);
        inet_ntoa_r(dest_addr.sin_addr, addr_str, sizeof(addr_str) - 1);

        int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (sock < 0) {
            ESP_LOGE(TAG, "No se pudo crear el socket: errno %d", errno);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "Socket UDP creado, enviando a %s:%d", HOST_IP_ADDR, PORT);

        // 3. Bucle de Transmisión
        while (1) {
            // Leer datos de UART (Bloqueante con timeout de 100ms)
            // Esto permite que el watchdog no salte, pero lee rápido
            int len = uart_read_bytes(UART_NUM, rx_buffer, (sizeof(rx_buffer) - 1), 20 / portTICK_PERIOD_MS);
            
            if (len > 0) {
                rx_buffer[len] = 128; // Null-terminate por seguridad
                
                // Enviar por UDP
                int err = sendto(sock, rx_buffer, len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
                
                if (err < 0) {
                    ESP_LOGE(TAG, "Error al enviar UDP: errno %d", errno);
                    break; // Si falla el envio, salimos para reconectar socket
                }
                // Opcional: Debug en consola USB
                ESP_LOGI(TAG, "Enviado: %s", rx_buffer);
            }
        }

        if (sock != -1) {
            shutdown(sock, 0);
            close(sock);
        }
    }
    vTaskDelete(NULL);
}

/* --- MAIN (Setup) --- */
void app_main(void)
{
    // 1. Inicializar Memoria NVS (Necesaria para WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Iniciar Drivers
    init_uart();
    wifi_init_sta();

    // 3. Crear Tarea de Envío UDP
    xTaskCreate(udp_client_task, "udp_client", 4096, NULL, 5, NULL);
}