#include "ethernet_file_server.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_eth.h"
#include "esp_mac.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "ETH_FILE";

// -----------------------------------------------------------------------------
// W5500 configuration
// -----------------------------------------------------------------------------

/*
 * Important:
 *
 * SD card uses SPI2_HOST.
 * W5500 uses SPI3_HOST.
 */
#define ETH_SPI_HOST       SPI3_HOST

#define ETH_MOSI_GPIO      GPIO_NUM_11
#define ETH_MISO_GPIO      GPIO_NUM_12
#define ETH_SCLK_GPIO      GPIO_NUM_13
#define ETH_CS_GPIO        GPIO_NUM_14
#define ETH_INT_GPIO       GPIO_NUM_10
#define ETH_RST_GPIO       GPIO_NUM_9

#define ETH_SPI_CLOCK_HZ   (20 * 1000 * 1000)

// -----------------------------------------------------------------------------
// Network configuration
// -----------------------------------------------------------------------------

#define TCP_SERVER_PORT    5000

#define STATIC_IP_A        192
#define STATIC_IP_B        168
#define STATIC_IP_C        10
#define STATIC_IP_D        51

#define STATIC_GW_A        192
#define STATIC_GW_B        168
#define STATIC_GW_C        10
#define STATIC_GW_D        1

// -----------------------------------------------------------------------------
// File-transfer configuration
// -----------------------------------------------------------------------------

#define SD_MOUNT_POINT          "/sdcard"
#define MAX_FILENAME_LENGTH     64
#define MAX_FILE_SIZE           (100U * 1024U * 1024U)
#define FILE_BUFFER_SIZE        4096

static esp_eth_handle_t s_eth_handle = nullptr;
static esp_netif_t *s_eth_netif = nullptr;

static uint8_t s_file_buffer[FILE_BUFFER_SIZE];

// -----------------------------------------------------------------------------
// Utility functions
// -----------------------------------------------------------------------------

static uint16_t decode_u16_le(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

static uint32_t decode_u32_le(const uint8_t *data)
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

static bool filename_is_valid(const char *filename)
{
    if (!filename || filename[0] == '\0') {
        return false;
    }

    if (strstr(filename, "..") != nullptr) {
        return false;
    }

    if (strchr(filename, '/') != nullptr) {
        return false;
    }

    if (strchr(filename, '\\') != nullptr) {
        return false;
    }

    if (strchr(filename, ':') != nullptr) {
        return false;
    }

    return true;
}

/**
 * TCP is a byte stream.
 *
 * One recv() call is not guaranteed to return all requested bytes.
 */
static bool socket_receive_exact(int sock,
                                 uint8_t *buffer,
                                 size_t length)
{
    size_t total_received = 0;

    while (total_received < length) {
        int result = recv(
            sock,
            buffer + total_received,
            length - total_received,
            0);

        if (result == 0) {
            ESP_LOGW(TAG, "Client disconnected");
            return false;
        }

        if (result < 0) {
            ESP_LOGE(TAG, "recv failed: errno=%d", errno);
            return false;
        }

        total_received += static_cast<size_t>(result);
    }

    return true;
}

static bool socket_send_all(int sock,
                            const uint8_t *data,
                            size_t length)
{
    size_t total_sent = 0;

    while (total_sent < length) {
        int result = send(
            sock,
            data + total_sent,
            length - total_sent,
            0);

        if (result <= 0) {
            ESP_LOGE(TAG, "send failed: errno=%d", errno);
            return false;
        }

        total_sent += static_cast<size_t>(result);
    }

    return true;
}

static bool socket_send_text(int sock, const char *text)
{
    return socket_send_all(
        sock,
        reinterpret_cast<const uint8_t *>(text),
        strlen(text));
}

// -----------------------------------------------------------------------------
// File receiver
// -----------------------------------------------------------------------------

static bool receive_file(int client_sock)
{
    /*
     * Header:
     *
     * 4 bytes: "FILE"
     * 2 bytes: filename length, uint16 little-endian
     * 4 bytes: file size, uint32 little-endian
     */
    uint8_t header[10];

    if (!socket_receive_exact(
            client_sock,
            header,
            sizeof(header))) {
        return false;
    }

    if (memcmp(header, "FILE", 4) != 0) {
        ESP_LOGE(TAG, "Invalid file command");
        socket_send_text(client_sock, "ERR COMMAND\n");
        return false;
    }

    const uint16_t filename_length = decode_u16_le(&header[4]);
    const uint32_t file_size = decode_u32_le(&header[6]);

    if (filename_length == 0 ||
        filename_length >= MAX_FILENAME_LENGTH) {
        ESP_LOGE(
            TAG,
            "Invalid filename length: %u",
            static_cast<unsigned>(filename_length));

        socket_send_text(client_sock, "ERR FILENAME\n");
        return false;
    }

    if (file_size == 0 || file_size > MAX_FILE_SIZE) {
        ESP_LOGE(
            TAG,
            "Invalid file size: %lu",
            static_cast<unsigned long>(file_size));

        socket_send_text(client_sock, "ERR SIZE\n");
        return false;
    }

    char filename[MAX_FILENAME_LENGTH] = {};

    if (!socket_receive_exact(
            client_sock,
            reinterpret_cast<uint8_t *>(filename),
            filename_length)) {
        return false;
    }

    filename[filename_length] = '\0';

    if (!filename_is_valid(filename)) {
        ESP_LOGE(TAG, "Unsafe filename: %s", filename);
        socket_send_text(client_sock, "ERR FILENAME\n");
        return false;
    }

    char final_path[128] = {};
    char temporary_path[128] = {};

    int result = snprintf(
        final_path,
        sizeof(final_path),
        SD_MOUNT_POINT "/%s",
        filename);

    if (result < 0 ||
        result >= static_cast<int>(sizeof(final_path))) {
        socket_send_text(client_sock, "ERR PATH\n");
        return false;
    }

    result = snprintf(
        temporary_path,
        sizeof(temporary_path),
        SD_MOUNT_POINT "/%s.part",
        filename);

    if (result < 0 ||
        result >= static_cast<int>(sizeof(temporary_path))) {
        socket_send_text(client_sock, "ERR PATH\n");
        return false;
    }

    ESP_LOGI(
        TAG,
        "Receiving %s, size=%lu bytes",
        filename,
        static_cast<unsigned long>(file_size));

    remove(temporary_path);

    FILE *file = fopen(temporary_path, "wb");

    if (!file) {
        ESP_LOGE(
            TAG,
            "Failed to open %s, errno=%d",
            temporary_path,
            errno);

        socket_send_text(client_sock, "ERR OPEN\n");
        return false;
    }

    if (!socket_send_text(client_sock, "READY\n")) {
        fclose(file);
        remove(temporary_path);
        return false;
    }

    uint32_t total_received = 0;
    unsigned last_progress = 101;
    bool success = true;

    while (total_received < file_size) {
        uint32_t remaining = file_size - total_received;

        size_t block_size =
            remaining > sizeof(s_file_buffer)
                ? sizeof(s_file_buffer)
                : static_cast<size_t>(remaining);

        if (!socket_receive_exact(
                client_sock,
                s_file_buffer,
                block_size)) {
            success = false;
            break;
        }

        size_t written = fwrite(
            s_file_buffer,
            1,
            block_size,
            file);

        if (written != block_size) {
            ESP_LOGE(
                TAG,
                "SD write failed: requested=%u written=%u errno=%d",
                static_cast<unsigned>(block_size),
                static_cast<unsigned>(written),
                errno);

            success = false;
            break;
        }

        total_received += static_cast<uint32_t>(written);

        unsigned progress =
            static_cast<unsigned>(
                (static_cast<uint64_t>(total_received) * 100ULL) /
                file_size);

        if (progress != last_progress) {
            ESP_LOGI(
                TAG,
                "Progress: %u%%",
                progress);

            last_progress = progress;
        }
    }

    if (fflush(file) != 0) {
        ESP_LOGE(TAG, "fflush failed: errno=%d", errno);
        success = false;
    }

    if (fclose(file) != 0) {
        ESP_LOGE(TAG, "fclose failed: errno=%d", errno);
        success = false;
    }

    if (!success || total_received != file_size) {
        remove(temporary_path);

        socket_send_text(client_sock, "ERR TRANSFER\n");

        ESP_LOGE(
            TAG,
            "Transfer failed: %lu/%lu bytes",
            static_cast<unsigned long>(total_received),
            static_cast<unsigned long>(file_size));

        return false;
    }

    /*
     * Remove existing destination before rename.
     */
    remove(final_path);

    if (rename(temporary_path, final_path) != 0) {
        ESP_LOGE(
            TAG,
            "rename failed: errno=%d",
            errno);

        remove(temporary_path);
        socket_send_text(client_sock, "ERR RENAME\n");
        return false;
    }

    ESP_LOGI(TAG, "File saved: %s", final_path);

    socket_send_text(client_sock, "OK\n");

    return true;
}

// -----------------------------------------------------------------------------
// TCP server
// -----------------------------------------------------------------------------

static void tcp_file_server_task(void *parameter)
{
    (void)parameter;

    struct sockaddr_in server_address = {};

    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(TCP_SERVER_PORT);

    int listen_socket = socket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_IP);

    if (listen_socket < 0) {
        ESP_LOGE(
            TAG,
            "Unable to create socket: errno=%d",
            errno);

        vTaskDelete(nullptr);
        return;
    }

    int reuse = 1;

    setsockopt(
        listen_socket,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse,
        sizeof(reuse));

    if (bind(
            listen_socket,
            reinterpret_cast<struct sockaddr *>(&server_address),
            sizeof(server_address)) != 0) {
        ESP_LOGE(TAG, "bind failed: errno=%d", errno);
        close(listen_socket);
        vTaskDelete(nullptr);
        return;
    }

    if (listen(listen_socket, 1) != 0) {
        ESP_LOGE(TAG, "listen failed: errno=%d", errno);
        close(listen_socket);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(
        TAG,
        "File server listening on TCP port %d",
        TCP_SERVER_PORT);

    while (true) {
        struct sockaddr_in client_address = {};
        socklen_t client_length = sizeof(client_address);

        ESP_LOGI(TAG, "Waiting for file client");

        int client_socket = accept(
            listen_socket,
            reinterpret_cast<struct sockaddr *>(&client_address),
            &client_length);

        if (client_socket < 0) {
            ESP_LOGE(
                TAG,
                "accept failed: errno=%d",
                errno);

            continue;
        }

        ESP_LOGI(
            TAG,
            "Client connected: %s",
            inet_ntoa(client_address.sin_addr));

        receive_file(client_socket);

        shutdown(client_socket, 0);
        close(client_socket);

        ESP_LOGI(TAG, "Client connection closed");
    }
}

// -----------------------------------------------------------------------------
// Ethernet events
// -----------------------------------------------------------------------------

static void ethernet_event_handler(void *arg,
                                   esp_event_base_t event_base,
                                   int32_t event_id,
                                   void *event_data)
{
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet link up");
            break;

        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Ethernet link down");
            break;

        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet started");
            break;

        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet stopped");
            break;

        default:
            break;
    }
}

static void ethernet_got_ip_handler(void *arg,
                                    esp_event_base_t event_base,
                                    int32_t event_id,
                                    void *event_data)
{
    ip_event_got_ip_t *event =
        static_cast<ip_event_got_ip_t *>(event_data);

    ESP_LOGI(
        TAG,
        "Ethernet IP: " IPSTR,
        IP2STR(&event->ip_info.ip));
}

// -----------------------------------------------------------------------------
// Public initialization
// -----------------------------------------------------------------------------

esp_err_t ethernet_file_server_start(void)
{
    ESP_LOGI(TAG, "Initializing W5500 Ethernet");

    esp_err_t ret = gpio_install_isr_service(0);

    if (ret != ESP_OK &&
        ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(
            TAG,
            "gpio_install_isr_service failed: %s",
            esp_err_to_name(ret));

        return ret;
    }

    ret = esp_netif_init();

    if (ret != ESP_OK &&
        ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_loop_create_default();

    if (ret != ESP_OK &&
        ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();

    s_eth_netif = esp_netif_new(&netif_config);

    if (!s_eth_netif) {
        return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t bus_config = {};

    bus_config.mosi_io_num = ETH_MOSI_GPIO;
    bus_config.miso_io_num = ETH_MISO_GPIO;
    bus_config.sclk_io_num = ETH_SCLK_GPIO;
    bus_config.quadwp_io_num = -1;
    bus_config.quadhd_io_num = -1;

    ret = spi_bus_initialize(
        ETH_SPI_HOST,
        &bus_config,
        SPI_DMA_CH_AUTO);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "W5500 SPI init failed: %s",
            esp_err_to_name(ret));

        return ret;
    }

    spi_device_interface_config_t device_config = {};

    device_config.command_bits = 16;
    device_config.address_bits = 8;
    device_config.mode = 0;
    device_config.clock_speed_hz = ETH_SPI_CLOCK_HZ;
    device_config.spics_io_num = ETH_CS_GPIO;
    device_config.queue_size = 20;

    eth_w5500_config_t w5500_config =
        ETH_W5500_DEFAULT_CONFIG(
            ETH_SPI_HOST,
            &device_config);

    w5500_config.int_gpio_num = ETH_INT_GPIO;

    eth_mac_config_t mac_config =
        ETH_MAC_DEFAULT_CONFIG();

    eth_phy_config_t phy_config =
        ETH_PHY_DEFAULT_CONFIG();

    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = ETH_RST_GPIO;

    esp_eth_mac_t *mac =
        esp_eth_mac_new_w5500(
            &w5500_config,
            &mac_config);

    esp_eth_phy_t *phy =
        esp_eth_phy_new_w5500(
            &phy_config);

    if (!mac || !phy) {
        ESP_LOGE(TAG, "Failed to create W5500 MAC/PHY");
        return ESP_ERR_NO_MEM;
    }

    esp_eth_config_t ethernet_config =
        ETH_DEFAULT_CONFIG(mac, phy);

    ret = esp_eth_driver_install(
        &ethernet_config,
        &s_eth_handle);

    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t mac_address[6] = {};

    ESP_ERROR_CHECK(
        esp_read_mac(
            mac_address,
            ESP_MAC_WIFI_STA));

    mac_address[0] =
        (mac_address[0] & 0xFE) | 0x02;

    ESP_ERROR_CHECK(
        esp_eth_ioctl(
            s_eth_handle,
            ETH_CMD_S_MAC_ADDR,
            mac_address));

    ESP_ERROR_CHECK(
        esp_netif_attach(
            s_eth_netif,
            esp_eth_new_netif_glue(s_eth_handle)));

    ESP_ERROR_CHECK(
        esp_netif_dhcpc_stop(s_eth_netif));

    esp_netif_ip_info_t ip_info = {};

    IP4_ADDR(
        &ip_info.ip,
        STATIC_IP_A,
        STATIC_IP_B,
        STATIC_IP_C,
        STATIC_IP_D);

    IP4_ADDR(
        &ip_info.netmask,
        255,
        255,
        255,
        0);

    IP4_ADDR(
        &ip_info.gw,
        STATIC_GW_A,
        STATIC_GW_B,
        STATIC_GW_C,
        STATIC_GW_D);

    ESP_ERROR_CHECK(
        esp_netif_set_ip_info(
            s_eth_netif,
            &ip_info));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            ETH_EVENT,
            ESP_EVENT_ANY_ID,
            ethernet_event_handler,
            nullptr));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_ETH_GOT_IP,
            ethernet_got_ip_handler,
            nullptr));

    ESP_ERROR_CHECK(
        esp_eth_start(s_eth_handle));

    BaseType_t task_result = xTaskCreate(
        tcp_file_server_task,
        "tcp_file_server",
        8192,
        nullptr,
        5,
        nullptr);

    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TCP task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Ethernet file server started at "
        "192.168.10.51:%d",
        TCP_SERVER_PORT);

    return ESP_OK;
}