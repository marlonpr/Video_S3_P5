//  (VIDEO_TEST)
#include <stdio.h>
#include "driver/gpio.h"
#include "hub75.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_panel.h"
#include "sd_card_test.h"

static const char* TAG = "MAIN";

// =============================== HUB75 GLOBAL OBJECTS ===============================

static Hub75Config config = make_config();
static Hub75Driver driver(config);

#define VIDEO_WIDTH       128
#define VIDEO_HEIGHT      64
#define VIDEO_FPS         10
#define VIDEO_FRAME_SIZE  (VIDEO_WIDTH * VIDEO_HEIGHT * 3)

// Change this GPIO to your desired video button
#define VIDEO_BUTTON_GPIO GPIO_NUM_42

#define VIDEO_FILE_PATH "/sdcard/video_128x64.rgb888"

static const char *TAG_VIDEO = "VIDEO";
static const char *TAG_VIDEO_BUTTON = "VIDEO_BUTTON";

static TaskHandle_t s_video_task_handle = nullptr;

 //
static volatile bool s_video_mode_active = false;

//
// * RGB888 frame:
// * 128 × 64 × 3 = 24,576 bytes
 //
static uint8_t s_video_frame[VIDEO_FRAME_SIZE];

static void video_button_init()
{
    gpio_config_t config = {};

    config.pin_bit_mask = 1ULL << VIDEO_BUTTON_GPIO;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;

    ESP_ERROR_CHECK(gpio_config(&config));

    ESP_LOGI(
        TAG_VIDEO_BUTTON,
        "Video button initialized on GPIO%d",
        VIDEO_BUTTON_GPIO
    );
}


static void video_button_task(void *arg)
{
    bool previous_level = true;
    TickType_t last_press_tick = 0;

    constexpr TickType_t poll_period =
        pdMS_TO_TICKS(20);

    constexpr TickType_t debounce_period =
        pdMS_TO_TICKS(200);

    while (true) {
        const bool current_level =
            gpio_get_level(VIDEO_BUTTON_GPIO);

        //
       //  * Active-low button:
        // * previous HIGH, current LOW = new press
         //
        if (previous_level && !current_level) {
            const TickType_t now = xTaskGetTickCount();

            if ((now - last_press_tick) >= debounce_period) {
                last_press_tick = now;

                if (s_video_task_handle) {
                    //
                   //  * Each notification toggles play/stop.
                     //
                    xTaskNotifyGive(s_video_task_handle);
                }
            }
        }

        previous_level = current_level;

        vTaskDelay(poll_period);
    }
}




static void video_task(void *arg)
{
    auto *driver = static_cast<Hub75Driver *>(arg);

    if (!driver) {
        ESP_LOGE(TAG_VIDEO, "Invalid HUB75 driver");
        vTaskDelete(nullptr);
        return;
    }

    FILE *file = nullptr;
    bool playing = false;

    const TickType_t frame_period =
        pdMS_TO_TICKS(1000 / VIDEO_FPS);

    TickType_t last_frame_tick =
        xTaskGetTickCount();

    ESP_LOGI(TAG_VIDEO, "Video task ready");

    while (true) {
        if (!playing) {
            //
            // * Wait indefinitely until the button task sends a notification.
             //
            ulTaskNotifyTake(
                pdTRUE,
                portMAX_DELAY
            );

            file = fopen(VIDEO_FILE_PATH, "rb");

            if (!file) {
                ESP_LOGE(
                    TAG_VIDEO,
                    "Cannot open %s",
                    VIDEO_FILE_PATH
                );

                playing = false;
                s_video_mode_active = false;
                continue;
            }

            playing = true;
            s_video_mode_active = true;
            last_frame_tick = xTaskGetTickCount();

            ESP_LOGI(
                TAG_VIDEO,
                "Playback started: %s",
                VIDEO_FILE_PATH
            );

            continue;
        }

        //
        // * Check whether the button was pressed while playing.
       //  * Timeout zero means do not block.
         //
        if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
            playing = false;
            s_video_mode_active = false;

            if (file) {
                fclose(file);
                file = nullptr;
            }

            ESP_LOGI(TAG_VIDEO, "Playback stopped");

            //
             //* Optional: clear the last video frame.
             //* Remove this if your clock task immediately redraws.
             //
            driver->clear();
            driver->flip_buffer();

            continue;
        }

        const size_t bytes_read =
            fread(
                s_video_frame,
                1,
                VIDEO_FRAME_SIZE,
                file
            );

        if (bytes_read != VIDEO_FRAME_SIZE) {
            //
            // * End of file: loop from the beginning.
             //
            clearerr(file);

            if (fseek(file, 0, SEEK_SET) != 0) {
                ESP_LOGE(TAG_VIDEO, "Failed to rewind video");

                playing = false;
                s_video_mode_active = false;

                fclose(file);
                file = nullptr;

                continue;
            }

            last_frame_tick = xTaskGetTickCount();
            continue;
        }

        driver->draw_pixels(
            0,
            0,
            VIDEO_WIDTH,
            VIDEO_HEIGHT,
            s_video_frame,
            Hub75PixelFormat::RGB888,
            Hub75ColorOrder::RGB,
            false
        );

        driver->flip_buffer();

        vTaskDelayUntil(
            &last_frame_tick,
            frame_period
        );
    }
}





static uint8_t brightness_level_to_hub75(int level)
{
    if (level < 1) {
        level = 1;
    }

    if (level > 10) {
        level = 10;
    }

    return (uint8_t)((level * 255) / 10);
}



// =============================== APP MAIN ===============================

extern "C" void app_main(void)
{	
	ESP_LOGI(TAG, "Starting HUB75");

    if (!driver.begin()) {
        ESP_LOGE(TAG, "Driver start failed");
        return;
    }	
	driver.set_brightness(brightness_level_to_hub75(5));	
	
	sd_card_test();

	video_button_init();

	xTaskCreatePinnedToCore(
	    video_task,
	    "video_task",
	    8192,
	    &driver,
	    5,
	    &s_video_task_handle,
	    1
	);

	xTaskCreatePinnedToCore(
	    video_button_task,
	    "video_button_task",
	    2048,
	    nullptr,
	    4,
	    nullptr,
	    0
	);	

}

















//=============================================================== ETHERNET SECTION ===========================================================//	
/*
ESP_LOGI(TAG, "Starting Ethernet");
ESP_ERROR_CHECK(clock_ethernet_init_static());

clock_protocol_context_t protocol_ctx = {
    .brightness_level = &brightness_level,
    .eth_brightness_pending = &g_eth_brightness_pending,
    .eth_brightness_level = &g_eth_brightness_level,
    .eth_format_pending = &g_eth_format_pending,
    .eth_format = &g_eth_format,
    .eth_time_pending = &g_eth_time_pending,
    .eth_time = &g_eth_time,
    .factory_reset_pending = &g_eth_factory_reset_pending,
    .now = &g_now,
    .rtc_valid = &g_rtc_valid,
    .clock_format = &clock_format,
    .data_mux = &g_data_mux,
};

clock_protocol_init(&protocol_ctx);

ESP_ERROR_CHECK(clock_ethernet_start_tcp_server(clock_protocol_rx_callback));
*/
//====================================================================================================================================================//







/*
ESP_ERROR_CHECK(clock_settings_init());	

ESP_ERROR_CHECK(clock_alarm_init(ALARM_GPIO));		

ESP_ERROR_CHECK(clock_alarm_load());	


uint8_t saved_format = clock_settings_load_format((uint8_t)FORMAT_12H);
if (saved_format > FORMAT_24H) {
    saved_format = FORMAT_12H;
}
clock_format = (hour_format_t)saved_format;

uint8_t saved_mode = clock_settings_load_mode((uint8_t)MODE_1);
if (saved_mode < MODE_1 || saved_mode > MODE_ROTATION) {
    saved_mode = MODE_1;
}
display_mode = (display_mode_t)saved_mode;

brightness_level = clock_settings_load_brightness(5);

if (brightness_level < 1) {
    brightness_level = 1;
}

if (brightness_level > 10) {
    brightness_level = 10;
}

temporal_brightness = brightness_level;

//driver.set_brightness(brightness_level_to_hub75(brightness_level));






start_logo_screen(3000);	

static ds18b20_t ambient_sensor;
static ds3231_dev_t rtc;

ESP_ERROR_CHECK(ds18b20_init(&ambient_sensor, DS18B20_GPIO));
ESP_ERROR_CHECK(init_ds3231(&rtc));

check_or_set_default_rtc(&rtc);


clock_menu_context_t menu_ctx = {
    .driver = &driver,
    .rtc = &rtc,

    .brightness_level = &brightness_level,
    .temporal_brightness = &temporal_brightness,

    .data_mux = &g_data_mux,
    .g_now = &g_now,
    .g_rtc_valid = &g_rtc_valid,

    .show_message = show_temp_message,
};

clock_menu_init(&menu_ctx);

ESP_ERROR_CHECK(clock_buttons_init(PIN_MENU, PIN_UP, PIN_DOWN));




*/









	
		
/*
int x = 0;
int frame_counter = 0;

while (true) {
    if ((frame_counter % 3) == 0) {
        x++;

        if (x > 110) {
            x = 0;
        }
    }

    driver.clear();
    driver.fill(x, 18, 18, 13, 255, 255, 255);
    driver.flip_buffer();

    frame_counter++;

    vTaskDelay(pdMS_TO_TICKS(100));
}
*/
	
	
	

/*

xTaskCreatePinnedToCore(
    button_task,
    "ButtonTask",
    4096,
    &rtc,
    2,
    NULL,
    0
);

xTaskCreatePinnedToCore(
    display_update_task,
    "DisplayTask",
    8192,
    &driver,
    2,
    NULL,
    1
);

xTaskCreatePinnedToCore(
    rtc_task,
    "RtcTask",
    4096,
    &rtc,
    1,
    NULL,
    0
);

xTaskCreatePinnedToCore(
    ds18b20_task,
    "DS18B20Task",
    4096,
    &ambient_sensor,
    1,
    NULL,
    0
);
*/	










/*



#define VIDEO_WIDTH       128
#define VIDEO_HEIGHT      64
#define VIDEO_FPS         10
//#define VIDEO_FRAME_SIZE  (VIDEO_WIDTH * VIDEO_HEIGHT * 2) 565

#define VIDEO_FRAME_SIZE (128 * 64 * 3) //888


static uint8_t video_frame_buffer[VIDEO_FRAME_SIZE];

void play_rgb565_video(Hub75Driver *driver, const char *path)
{
    if (!driver || !path) {
        return;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE("VIDEO", "Failed to open video file: %s", path);
        return;
    }

    ESP_LOGI("VIDEO", "Playing RGB565 video: %s", path);

    const TickType_t frame_delay = pdMS_TO_TICKS(1000 / VIDEO_FPS);

	TickType_t last_wake = xTaskGetTickCount();

	while (true) {
	    size_t read_bytes = fread(video_frame_buffer, 1, VIDEO_FRAME_SIZE, f);

	    if (read_bytes != VIDEO_FRAME_SIZE) {
	        //ESP_LOGI("VIDEO", "End of video, looping");
	        fseek(f, 0, SEEK_SET);
	        last_wake = xTaskGetTickCount();
	        continue;
	    }

	    driver->draw_pixels(0,
	                        0,
	                        VIDEO_WIDTH,
	                        VIDEO_HEIGHT,
	                        video_frame_buffer,
	                        Hub75PixelFormat::RGB565,
	                        Hub75ColorOrder::RGB,
	                        false);

	    driver->flip_buffer();

	    vTaskDelayUntil(&last_wake, frame_delay);
	}

    fclose(f);
}














void play_rgb888_video(Hub75Driver *driver, const char *path)
{
    if (!driver || !path) {
        return;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE("VIDEO", "Failed to open video file: %s", path);
        return;
    }

    ESP_LOGI("VIDEO", "Playing RGB565 video: %s", path);

    const TickType_t frame_delay = pdMS_TO_TICKS(1000 / VIDEO_FPS);

	TickType_t last_wake = xTaskGetTickCount();

	while (true) {
	    size_t read_bytes = fread(video_frame_buffer, 1, VIDEO_FRAME_SIZE, f);

	    if (read_bytes != VIDEO_FRAME_SIZE) {
	        //ESP_LOGI("VIDEO", "End of video, looping");
	        fseek(f, 0, SEEK_SET);
	        last_wake = xTaskGetTickCount();
	        continue;
	    }

		driver->draw_pixels(
		    0,
		    0,
		    128,
		    64,
		    video_frame_buffer,
		    Hub75PixelFormat::RGB888,
		    Hub75ColorOrder::RGB,
		    false
		);

	    driver->flip_buffer();

	    vTaskDelayUntil(&last_wake, frame_delay);
	}

    fclose(f);
}




*/













