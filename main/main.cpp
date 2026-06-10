//  (VIDEO_TEST)
#include <stdio.h>
#include "driver/gpio.h"
#include "ethernet_file_server.h"
#include "hub75.h"
#include "led_panel.h"

static const char* TAG = "MAIN";

// =============================== HUB75 GLOBAL OBJECTS ===============================

static Hub75Config config = make_config();
static Hub75Driver driver(config);

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#define VIDEO_STREAM_PORT 5001

#define VIDEO_WIDTH       192
#define VIDEO_HEIGHT      96
#define VIDEO_FRAME_SIZE  (VIDEO_WIDTH * VIDEO_HEIGHT * 3)  // *3 for RGB888, 2 for RGB565

static uint8_t video_stream_frame[VIDEO_FRAME_SIZE];

static const char *TAG_VIDEO_STREAM = "VIDEO_STREAM";





static bool receive_exact(
    int socket_fd,
    uint8_t *buffer,
    size_t size)
{
    if (!buffer || size == 0) {
        return false;
    }

    size_t received = 0;

    while (received < size) {
        const int result = recv(
            socket_fd,
            buffer + received,
            size - received,
            0
        );

        if (result > 0) {
            received += static_cast<size_t>(result);
            continue;
        }

        if (result == 0) {
            ESP_LOGI(
                TAG_VIDEO_STREAM,
                "Client closed the connection"
            );

            return false;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            ESP_LOGW(
                TAG_VIDEO_STREAM,
                "Timed out waiting for video data"
            );

            return false;
        }

        ESP_LOGE(
            TAG_VIDEO_STREAM,
            "recv failed: errno=%d (%s)",
            errno,
            strerror(errno)
        );

        return false;
    }

    return true;
}




static void video_stream_task(void *argument)
{
    auto *hub75 = static_cast<Hub75Driver *>(argument);

    if (!hub75) {
        ESP_LOGE(TAG_VIDEO_STREAM, "Invalid HUB75 driver");
        vTaskDelete(nullptr);
        return;
    }

    while (true) {
        int listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

        if (listen_socket < 0) {
            ESP_LOGE(
                TAG_VIDEO_STREAM,
                "Failed to create listening socket: errno=%d",
                errno
            );

            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int reuse = 1;

        if (setsockopt(
                listen_socket,
                SOL_SOCKET,
                SO_REUSEADDR,
                &reuse,
                sizeof(reuse)) < 0) {
            ESP_LOGW(
                TAG_VIDEO_STREAM,
                "SO_REUSEADDR failed: errno=%d",
                errno
            );
        }

        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_port = htons(VIDEO_STREAM_PORT);
        address.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(
                listen_socket,
                reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) < 0) {
            ESP_LOGE(
                TAG_VIDEO_STREAM,
                "bind failed: errno=%d",
                errno
            );

            close(listen_socket);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (listen(listen_socket, 1) < 0) {
            ESP_LOGE(
                TAG_VIDEO_STREAM,
                "listen failed: errno=%d",
                errno
            );

            close(listen_socket);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(
            TAG_VIDEO_STREAM,
            "Waiting for RGB888 stream on TCP port %d",
            VIDEO_STREAM_PORT
        );

        while (true) {
            sockaddr_in client_address = {};
            socklen_t client_length = sizeof(client_address);

            int client_socket = accept(
                listen_socket,
                reinterpret_cast<sockaddr *>(&client_address),
                &client_length
            );

            if (client_socket < 0) {
                ESP_LOGE(
                    TAG_VIDEO_STREAM,
                    "accept failed: errno=%d",
                    errno
                );

                break;
            }

            char client_ip[INET_ADDRSTRLEN] = {};

            inet_ntop(
                AF_INET,
                &client_address.sin_addr,
                client_ip,
                sizeof(client_ip)
            );

            ESP_LOGI(
                TAG_VIDEO_STREAM,
                "Video client connected from %s:%u",
                client_ip,
                ntohs(client_address.sin_port)
            );

            int no_delay = 1;

            setsockopt(
                client_socket,
                IPPROTO_TCP,
                TCP_NODELAY,
                &no_delay,
                sizeof(no_delay)
            );

            int keepalive = 1;

            setsockopt(
                client_socket,
                SOL_SOCKET,
                SO_KEEPALIVE,
                &keepalive,
                sizeof(keepalive)
            );

            /*
             * Prevent recv() from blocking forever if the sender disappears.
             */
            timeval receive_timeout = {};
            receive_timeout.tv_sec = 5;
            receive_timeout.tv_usec = 0;

            setsockopt(
                client_socket,
                SOL_SOCKET,
                SO_RCVTIMEO,
                &receive_timeout,
                sizeof(receive_timeout)
            );

            uint32_t frame_number = 0;

            while (receive_exact(
                client_socket,
                video_stream_frame,
                VIDEO_FRAME_SIZE)) {

			
					
			/*
			
			//======================== RGB565 ========================================
			
			hub75->draw_pixels(0, 0, VIDEO_WIDTH, VIDEO_HEIGHT,
			                   video_stream_frame,
			                   Hub75PixelFormat::RGB565,
			                   Hub75ColorOrder::RGB,
			                   false);
							   
		    //===================================================================================================
		   
			*/
			
			//======================== RGB888 ========================================
			
			hub75->draw_pixels(
			    0,
			    0,
			    VIDEO_WIDTH,
			    VIDEO_HEIGHT,
			    video_stream_frame,
			    Hub75PixelFormat::RGB888,
			    Hub75ColorOrder::RGB,
			    false
			);
			
			//===================================================================================================
			
			

							   

					//hub75->flip_buffer();     // =========== COMMENT FOR SINGLE BUFFER =========== 

                frame_number++;

                if ((frame_number % 100) == 0) {
                    ESP_LOGI(
                        TAG_VIDEO_STREAM,
                        "Displayed %lu frames",
                        static_cast<unsigned long>(frame_number)
                    );
                }
            }

            shutdown(client_socket, SHUT_RDWR);
            close(client_socket);

            ESP_LOGI(
                TAG_VIDEO_STREAM,
                "Video client disconnected after %lu frames",
                static_cast<unsigned long>(frame_number)
            );

            /*
             * Optional: clear the last streamed frame.
             * Remove these lines if you want the last frame to remain visible.
             */
            hub75->clear();
            //hub75->flip_buffer();   				// =========== COMMENT FOR SINGLE BUFFER ===========

            ESP_LOGI(
                TAG_VIDEO_STREAM,
                "Waiting for next video client"
            );
        }

        close(listen_socket);

        ESP_LOGW(
            TAG_VIDEO_STREAM,
            "Restarting video listening socket"
        );

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}




// =============================== APP MAIN ===============================
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting HUB75");

    if (!driver.begin()) {
        ESP_LOGE(TAG, "Driver start failed");
        return;
    }

    driver.set_brightness(25);

    esp_err_t ret = ethernet_file_server_start();

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Ethernet initialization failed: %s",
            esp_err_to_name(ret)
        );
        return;
    }

    BaseType_t task_result = xTaskCreatePinnedToCore(
        video_stream_task,
        "video_stream",
        8192,
        &driver,
        5,
        nullptr,
        1
    );

    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create video stream task");
        return;
    }

    ESP_LOGI(
        TAG,
        "RGB888 video stream ready on port %d",
        VIDEO_STREAM_PORT
    );
}

















/*
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

#define VIDEO_FILE_PATH "/sdcard/video_128x64.RGB888"

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
             // Optional: clear the last video frame.
             // Remove this if your clock task immediately redraws.
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

*/



















