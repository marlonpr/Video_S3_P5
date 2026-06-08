// sd_card_test.cpp

#include "sd_card_test.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"

#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

static const char *TAG = "SD_TEST";

#define SD_MOUNT_POINT "/sdcard"

#define PIN_SD_CS    GPIO_NUM_4
#define PIN_SD_MOSI  GPIO_NUM_6
#define PIN_SD_MISO  GPIO_NUM_5
#define PIN_SD_CLK   GPIO_NUM_7

void sd_card_test(void)
{
    ESP_LOGI(TAG, "Initializing SD card over SPI");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card = nullptr;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000;   // Start safe: 10 MHz

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = PIN_SD_MOSI;
    bus_cfg.miso_io_num = PIN_SD_MISO;
    bus_cfg.sclk_io_num = PIN_SD_CLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 16 * 1024;

    esp_err_t ret = spi_bus_initialize((spi_host_device_t)host.slot,
                                       &bus_cfg,
                                       SDSPI_DEFAULT_DMA);

    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = (spi_host_device_t)host.slot;

    ESP_LOGI(TAG, "Mounting filesystem at %s", SD_MOUNT_POINT);

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT,
                                  &host,
                                  &slot_config,
                                  &mount_config,
                                  &card);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));

        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Filesystem mount failed. Check FAT32 format.");
        } else if (ret == ESP_ERR_INVALID_RESPONSE) {
            ESP_LOGE(TAG, "Invalid SD response. Check wiring/pins/card insertion.");
        }

        return;
    }

    ESP_LOGI(TAG, "SD card mounted successfully");

    sdmmc_card_print_info(stdout, card);

    // Create/write test file
    const char *file_path = SD_MOUNT_POINT "/waveshare_test.txt";

    FILE *f = fopen(file_path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing");
    } else {
        fprintf(f, "Hello from ESP32-S3-ETH SD card test\n");
        fclose(f);
        ESP_LOGI(TAG, "File written: %s", file_path);
    }

    // Read test file
    f = fopen(file_path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for reading");
    } else {
        char line[128];

        if (fgets(line, sizeof(line), f)) {
            ESP_LOGI(TAG, "Read from file: %s", line);
        }

        fclose(f);
    }

    // List root directory
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open root directory");
    } else {
        ESP_LOGI(TAG, "Files in %s:", SD_MOUNT_POINT);

        struct dirent *entry;
        while ((entry = readdir(dir)) != nullptr) {
            ESP_LOGI(TAG, "  %s", entry->d_name);
        }

        closedir(dir);
    }

    // Keep mounted if you want to use it later for video.
    // Do not unmount here if your video player will use /sdcard.
}