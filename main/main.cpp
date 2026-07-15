#include <stdio.h>
#include <string.h>
#include "hub75.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ds3231.h"
#include "led_panel.h"
#include "ds18b20.h"

#include "clock_display.h"
#include "clock_settings.h"
#include "clock_buttons.h"
#include "clock_menu.h"
#include "clock_ethernet.h"
#include "clock_network.h"
#include "clock_alarm.h"
#include "clock_protocol.h"
#include "clock_modes.h"
#include "clock_sd_card.h"
#include "clock_logo_manager.h"
#include "clock_palette.h"
#include "clock_ota.h"

#include "clock_mdns.h"

#define ALARM_GPIO GPIO_NUM_48
#define DS18B20_GPIO GPIO_NUM_39
#define PIN_MENU GPIO_NUM_40
#define PIN_UP   GPIO_NUM_41
#define PIN_DOWN GPIO_NUM_42

#define BUTTON_HOLD_MS     1000
#define NETWORK_MODE_HOLD_MS 10000
#define BUTTON_DEBOUNCE_MS 500
#define BUTTON_REPEAT_DELAY_MS 500
#define BUTTON_REPEAT_RATE_MS  500

//Assing a diferrent STATIC IP for each device in clock_ethernet.cpp
//ip_info.ip.addr      = ESP_IP4TOADDR(192, 168, 10, 50); 50 for device 1, 51 for device 2 and so on

static const char* TAG = "MAIN";


// =============================== HUB75 GLOBAL OBJECTS ===============================

static Hub75Config config = make_config();
static Hub75Driver driver(config);

//================================ GLOBALS =======================================
static bool g_startup_screen_active = false;
static int64_t g_startup_screen_until_us = 0;

static bool g_logo_screen_active = true;
static int64_t g_logo_screen_until_us = 0;

// =============================== SHARED DATA ===============================

static char g_message[32] = {0};
static bool g_message_active = false;
static int64_t g_message_until_us = 0;
static char g_next_message[32] = {0};
static bool g_next_message_pending = false;
static uint32_t g_next_message_duration_ms = 0;
static clock_network_result_gate_t g_network_result_gate = {};


static int brightness_level = 5;        // 1 to 10
static int temporal_brightness = 5;     // temporary menu value while editing

// =============================== ETHERNET DATA ===============================

static int g_eth_brightness_level = 5;
static bool g_eth_brightness_pending = false;

static bool g_eth_format_pending = false;
static hour_format_t g_eth_format = FORMAT_12H;

static bool g_eth_time_pending = false;
static ds3231_time_t g_eth_time = {};

#define DEFAULT_BRIGHTNESS_LEVEL  5
#define DEFAULT_CLOCK_FORMAT      FORMAT_12H
#define DEFAULT_DISPLAY_MODE      MODE_ROTATION

static bool g_eth_factory_reset_pending = false;


// ===========================================================================================================================


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



// =============================== DEFAULT RTC TIME ===============================

static const ds3231_time_t default_time = {
    .second = 0,
    .minute = 52,
    .hour = 13,
    .day_of_week = 2,
    .day = 18,
    .month = 8,
    .year = 2025,
};



static ds3231_time_t g_now = {
    .second = 0,
    .minute = 0,
    .hour = 0,
    .day_of_week = 1,
    .day = 1,
    .month = 1,
    .year = 2000,
};
static float g_temp_c = 0.0f;
static bool g_rtc_valid = false;
static bool g_temp_valid = false;

static portMUX_TYPE g_data_mux = portMUX_INITIALIZER_UNLOCKED;

// =============================== CLOCK / TEMP HELPERS ===============================
static hour_format_t clock_format = FORMAT_12H;



// =============================== DISPLAY MODES ===============================

static display_mode_t display_mode = MODE_1;
static clock_network_mode_t g_saved_network_mode = CLOCK_NETWORK_MODE_AUTO;


static void set_temp_message_locked(const char *msg, uint32_t duration_ms)
{
    snprintf(g_message, sizeof(g_message), "%s", msg);
    g_message_active = true;
    g_message_until_us = esp_timer_get_time() + ((int64_t)duration_ms * 1000);
    g_next_message_pending = false;
}

static void show_temp_message(const char *msg, uint32_t duration_ms)
{
    if (!msg) {
        return;
    }

    portENTER_CRITICAL(&g_data_mux);
    set_temp_message_locked(msg, duration_ms);
    portEXIT_CRITICAL(&g_data_mux);
}

static void show_network_status_message(const char *msg, uint32_t duration_ms)
{
    if (!msg) {
        return;
    }

    portENTER_CRITICAL(&g_data_mux);

    if (clock_network_result_gate_submit(&g_network_result_gate,
                                         msg,
                                         duration_ms)) {
        set_temp_message_locked(msg, duration_ms);
    }

    portEXIT_CRITICAL(&g_data_mux);
}

static void show_temp_message_sequence(const char *first,
                                       uint32_t first_duration_ms,
                                       const char *second,
                                       uint32_t second_duration_ms)
{
    if (!first || !second) {
        return;
    }

    portENTER_CRITICAL(&g_data_mux);

    snprintf(g_message, sizeof(g_message), "%s", first);
    g_message_active = true;
    g_message_until_us =
        esp_timer_get_time() + ((int64_t)first_duration_ms * 1000);
    snprintf(g_next_message, sizeof(g_next_message), "%s", second);
    g_next_message_pending = true;
    g_next_message_duration_ms = second_duration_ms;

    portEXIT_CRITICAL(&g_data_mux);
}

static void draw_temp_message(Hub75Driver *driver, const char *message)
{
    if (driver == nullptr || message == nullptr) {
        return;
    }

    const char *line1 = nullptr;
    const char *line2 = nullptr;

    if (strcmp(message, "ETH CONNECTED") == 0) {
        line1 = "ETH";
        line2 = "CONNECTED";
    } else if (strcmp(message, "WIFI CONNECTED") == 0) {
        line1 = "WIFI";
        line2 = "CONNECTED";
    } else if (strcmp(message, "NET NOT CONNECTED") == 0) {
        line1 = "NET NOT";
        line2 = "CONNECTED";
    } else if (strcmp(message, "REBOOT TO APPLY") == 0) {
        line1 = "REBOOT TO";
        line2 = "APPLY";
    }

    if (line1 != nullptr && line2 != nullptr) {
        draw_string_5x7(*driver,
                        clock_display_center_x_5x7(line1),
                        7,
                        line1,
                        255,
                        0,
                        0);
        draw_string_5x7(*driver,
                        clock_display_center_x_5x7(line2),
                        18,
                        line2,
                        255,
                        255,
                        0);
        return;
    }

    draw_string(*driver,
                clock_display_center_x_6x9(message),
                8,
                message,
                255,
                0,
                0);
}





static void start_logo_screen(uint32_t duration_ms)
{
    g_logo_screen_active = true;
    g_logo_screen_until_us = esp_timer_get_time() + ((int64_t)duration_ms * 1000);
}





// =============================== DISPLAY TASK ===============================

void display_update_task(void* pvParameters)
{
    Hub75Driver* driver = (Hub75Driver*)pvParameters;

    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(33); // ~30 FPS
	
	display_mode_t mode_copy;
	char message_copy[32];
	bool message_active_copy;
	
	bool menu_active_copy;
	
	bool startup_screen_active_copy;
	
	bool logo_screen_active_copy;
	clock_network_mode_t saved_network_mode_copy;

    while (true) {
		clock_menu_check_timeout();		
		
		bool factory_reset_pending_copy = false;

		portENTER_CRITICAL(&g_data_mux);

		if (g_eth_factory_reset_pending) {
		    factory_reset_pending_copy = true;
		    g_eth_factory_reset_pending = false;
		}

		portEXIT_CRITICAL(&g_data_mux);

		if (factory_reset_pending_copy) {
		    ESP_LOGW(TAG, "Applying factory reset from Ethernet");

			portENTER_CRITICAL(&g_data_mux);

			brightness_level = DEFAULT_BRIGHTNESS_LEVEL;
			temporal_brightness = DEFAULT_BRIGHTNESS_LEVEL;
			clock_format = DEFAULT_CLOCK_FORMAT;
			display_mode = DEFAULT_DISPLAY_MODE;

			g_eth_brightness_pending = false;
			g_eth_format_pending = false;
			g_eth_time_pending = false;

			portEXIT_CRITICAL(&g_data_mux);

			driver->set_brightness(
			    brightness_level_to_hub75(DEFAULT_BRIGHTNESS_LEVEL)
			);

			clock_settings_save_brightness(DEFAULT_BRIGHTNESS_LEVEL);
			clock_settings_save_format((uint8_t)DEFAULT_CLOCK_FORMAT);
			clock_settings_save_mode((uint8_t)DEFAULT_DISPLAY_MODE);

			esp_err_t alarm_save_ret = clock_alarm_clear_all_and_save();

		    if (alarm_save_ret != ESP_OK) {
		        ESP_LOGE(TAG,
		                 "Failed to save cleared alarms after factory reset: %s",
		                 esp_err_to_name(alarm_save_ret));
		    }

		    esp_err_t palette_reset_ret =
		        clock_palette_restore_all_defaults();

		    if (palette_reset_ret != ESP_OK) {
		        ESP_LOGE(TAG,
		                 "Failed to clear palette overrides after factory reset: %s",
		                 esp_err_to_name(palette_reset_ret));
		    }

		    ESP_LOGW(TAG, "Factory reset from Ethernet applied");

		    show_temp_message("RESET", 1000);
		}
		

		clock_alarm_process_deferred_save();
		
		bool eth_brightness_pending_copy = false;
		int eth_brightness_level_copy = 5;

		bool eth_format_pending_copy = false;
		hour_format_t eth_format_copy = FORMAT_12H;

		portENTER_CRITICAL(&g_data_mux);

		if (g_eth_brightness_pending) {
		    eth_brightness_pending_copy = true;
		    eth_brightness_level_copy = g_eth_brightness_level;
		    g_eth_brightness_pending = false;
		}

		if (g_eth_format_pending) {
		    eth_format_pending_copy = true;
		    eth_format_copy = g_eth_format;
		    g_eth_format_pending = false;
		}

		portEXIT_CRITICAL(&g_data_mux);

		if (eth_brightness_pending_copy) {
		    uint8_t hub75_brightness =
		        brightness_level_to_hub75(eth_brightness_level_copy);

		    driver->set_brightness(hub75_brightness);

		    portENTER_CRITICAL(&g_data_mux);
		    brightness_level = eth_brightness_level_copy;
		    temporal_brightness = eth_brightness_level_copy;
		    portEXIT_CRITICAL(&g_data_mux);

		    clock_settings_save_brightness(eth_brightness_level_copy);

		    ESP_LOGI(TAG,
		             "Brightness applied from Ethernet: level=%d hub75=%u",
		             eth_brightness_level_copy,
		             hub75_brightness);
		}

		if (eth_format_pending_copy) {
		    portENTER_CRITICAL(&g_data_mux);
		    clock_format = eth_format_copy;
		    portEXIT_CRITICAL(&g_data_mux);

		    clock_settings_save_format((uint8_t)eth_format_copy);

		    ESP_LOGI(TAG,
		             "Clock format applied from Ethernet: %s",
		             eth_format_copy == FORMAT_24H ? "24H" : "12H");
		}

		
		ds3231_time_t now_copy;
		float temp_copy;
		bool rtc_valid_copy;
		bool temp_valid_copy;
		hour_format_t format_copy;

		portENTER_CRITICAL(&g_data_mux);

		now_copy = g_now;
		temp_copy = g_temp_c;
		rtc_valid_copy = g_rtc_valid;
		temp_valid_copy = g_temp_valid;
		format_copy = clock_format;
		mode_copy = display_mode;

		menu_active_copy = clock_menu_is_active();

		startup_screen_active_copy = g_startup_screen_active;

		if (g_startup_screen_active && esp_timer_get_time() > g_startup_screen_until_us) {
		    g_startup_screen_active = false;
		    startup_screen_active_copy = false;

		    char pending_network_result[CLOCK_NETWORK_RESULT_MESSAGE_CAPACITY];
		    uint32_t pending_network_result_duration_ms = 0;
		    if (clock_network_result_gate_finish_startup(
		            &g_network_result_gate,
		            pending_network_result,
		            sizeof(pending_network_result),
		            &pending_network_result_duration_ms)) {
		        set_temp_message_locked(pending_network_result,
		                                pending_network_result_duration_ms);
		    }
		}

		if (g_message_active && esp_timer_get_time() > g_message_until_us) {
		    if (g_next_message_pending) {
		        snprintf(g_message, sizeof(g_message), "%s", g_next_message);
		        g_message_until_us = esp_timer_get_time() +
		            ((int64_t)g_next_message_duration_ms * 1000);
		        g_next_message_pending = false;
		    } else {
		        g_message_active = false;
		    }
		}

		message_active_copy = g_message_active;
		snprintf(message_copy, sizeof(message_copy), "%s", g_message);
		saved_network_mode_copy = g_saved_network_mode;
		
		logo_screen_active_copy = g_logo_screen_active;

		if (g_logo_screen_active && esp_timer_get_time() > g_logo_screen_until_us) {
		    g_logo_screen_active = false;
		    logo_screen_active_copy = false;

		    /*
		     * Start the settings screen after logo finishes.
		     */
		    g_startup_screen_active = true;
		    g_startup_screen_until_us = esp_timer_get_time() + (3000 * 1000);
		}

		portEXIT_CRITICAL(&g_data_mux);
		
		
		
		if (rtc_valid_copy) {
		    clock_alarm_check_trigger(&now_copy);
		}

		clock_alarm_runtime_update();

		clock_alarm_display_state_t alarm_state = {};

		bool alarm_active_copy =
		    clock_alarm_get_display_state(&alarm_state);

		alarm_effect_t alarm_effect_copy = alarm_state.effect;
		int alarm_id_copy = alarm_state.alarm_id;

		driver->clear();
		
	


		if (alarm_active_copy) {
		    bool show_alarm = true;

			if (alarm_effect_copy == ALARM_EFFECT_INTERMITENTE ||
			    alarm_effect_copy == ALARM_EFFECT_PARPADEO_CONTINUO ||
			    alarm_effect_copy == ALARM_EFFECT_PARPADEO_INTERMITENTE) {
			    show_alarm = ((esp_timer_get_time() / 300000) % 2) == 0;
			}

		    if (show_alarm) {
		        char alarm_msg[16];
		        snprintf(alarm_msg, sizeof(alarm_msg), "AL %02d", alarm_id_copy);

		        draw_string(*driver,
		                    clock_display_center_x_6x9(alarm_msg),
		                    8,
		                    alarm_msg,
		                    255,
		                    0,
		                    0);
		    }

		    driver->flip_buffer();

		    vTaskDelayUntil(&xLastWakeTime, xFrequency);
		    continue;
		}		
		
		if (message_active_copy) {
		    scroll_stop();
		    draw_temp_message(driver, message_copy);
		    driver->flip_buffer();

		    vTaskDelayUntil(&xLastWakeTime, xFrequency);
		    continue;
		}

		if (logo_screen_active_copy) {
		    scroll_stop();

		    clock_display_draw_logo(driver);

		    driver->flip_buffer();

		    vTaskDelayUntil(&xLastWakeTime, xFrequency);
		    continue;
		}
		
		if (startup_screen_active_copy) {
		    scroll_stop();

			clock_display_draw_startup(driver,
			                           mode_copy,
			                           brightness_level,
			                           format_copy,
			                           clock_network_mode_panel_label(
			                               saved_network_mode_copy));

		    driver->flip_buffer();

		    vTaskDelayUntil(&xLastWakeTime, xFrequency);
		    continue;
		}

		if (menu_active_copy) {
		    scroll_stop();
		    clock_menu_draw(driver);
		    driver->flip_buffer();

		    vTaskDelayUntil(&xLastWakeTime, xFrequency);
		    continue;
		}
		
		
		
		if (rtc_valid_copy) {
		    switch (mode_copy) {
				case MODE_1:
				{
				    fixed_item_t active_fixed_item = clock_modes_get_fixed_item();

				    if (active_fixed_item == FIXED_ITEM_LOGO) {
				        scroll_stop();
				        clock_display_draw_logo(driver);
				    } else {
						clock_display_draw_mode_1(driver,
						                          &now_copy,
						                          temp_copy,
						                          temp_valid_copy,
						                          format_copy);
				    }

				    break;
				}

				case MODE_2:
				{
				    fixed_item_t active_fixed_item = clock_modes_get_fixed_item();

				    if (active_fixed_item == FIXED_ITEM_LOGO) {
				        scroll_stop();
				        clock_display_draw_logo(driver);
				    } else {
				        clock_display_draw_mode_2(driver,
				                                  &now_copy,
				                                  temp_copy,
				                                  temp_valid_copy,
				                                  format_copy);
				    }

				    break;
				}

				case MODE_3:
				{
				    fixed_item_t active_fixed_item = clock_modes_get_fixed_item();

				    if (active_fixed_item == FIXED_ITEM_LOGO) {
				        scroll_stop();
				        clock_display_draw_logo(driver);
				    } else {
				        scroll_stop();
						clock_display_draw_mode_3(driver,
						                          &now_copy,
						                          temp_copy,
						                          temp_valid_copy,
						                          format_copy);
				    }

				    break;
				}

					case MODE_ROTATION:
					{
					    rotation_item_t active_rotation_item = clock_modes_get_rotation_item();

					    switch (active_rotation_item)
					    {
					        case ROT_ITEM_LOGO:
					            scroll_stop();
					            clock_display_draw_logo(driver);
					            break;

					        case ROT_ITEM_MODE_1:
							clock_display_draw_mode_1(driver,
							                          &now_copy,
							                          temp_copy,
							                          temp_valid_copy,
							                          format_copy);
					            break;

							case ROT_ITEM_MODE_2:
							    clock_display_draw_mode_2(driver,
							                              &now_copy,
							                              temp_copy,
							                              temp_valid_copy,
							                              format_copy);
							    break;

					        case ROT_ITEM_MODE_3:
					        default:
					            scroll_stop();
								clock_display_draw_mode_3(driver,
								                          &now_copy,
								                          temp_copy,
								                          temp_valid_copy,
								                          format_copy);
					            break;
					    }

					    break;
					}

					default:
					clock_display_draw_mode_1(driver,
					                          &now_copy,
					                          temp_copy,
					                          temp_valid_copy,
					                          format_copy);
					    break;
		    }
		} else {
		    scroll_stop();
		    draw_string(*driver, 2, 2, "NO RTC", 255, 0, 0);
		}

		driver->flip_buffer();

		vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

// =============================== RTC TASK ===============================

void rtc_task(void* pvParameters)
{
    ds3231_dev_t* rtc = (ds3231_dev_t*)pvParameters;

    while (true) {
		
		
		bool eth_time_pending_copy = false;
		ds3231_time_t eth_time_copy = {};

		portENTER_CRITICAL(&g_data_mux);

		if (g_eth_time_pending) {
		    eth_time_pending_copy = true;
		    eth_time_copy = g_eth_time;
		    g_eth_time_pending = false;
		}

		portEXIT_CRITICAL(&g_data_mux);

		if (eth_time_pending_copy) {
		    esp_err_t set_ret = ds3231_set_time(rtc, &eth_time_copy);

		    if (set_ret == ESP_OK) {
		        portENTER_CRITICAL(&g_data_mux);
		        g_now = eth_time_copy;
		        g_rtc_valid = true;
		        portEXIT_CRITICAL(&g_data_mux);

		        ESP_LOGI(TAG,
		                 "RTC updated from Ethernet: %04d-%02d-%02d %02d:%02d:%02d DOW=%d",
		                 eth_time_copy.year,
		                 eth_time_copy.month,
		                 eth_time_copy.day,
		                 eth_time_copy.hour,
		                 eth_time_copy.minute,
		                 eth_time_copy.second,
		                 eth_time_copy.day_of_week);
		    } else {
		        ESP_LOGE(TAG,
		                 "Failed to update RTC from Ethernet: %s",
		                 esp_err_to_name(set_ret));
		    }
		}
		
		
        ds3231_time_t now;

        if (ds3231_get_time(rtc, &now) == ESP_OK) {
            portENTER_CRITICAL(&g_data_mux);
            g_now = now;
            g_rtc_valid = true;
            portEXIT_CRITICAL(&g_data_mux);
        } else {
            ESP_LOGE(TAG, "Failed to read DS3231");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// =============================== DS18B20 TASK ===============================

void ds18b20_task(void* pvParameters)
{
    ds18b20_t* sensor = (ds18b20_t*)pvParameters;
	
	bool sensor_desconnected_init = true;

    while (true) {
        float temp_c = 0.0f;

        esp_err_t ret = ds18b20_read_temperature(sensor, &temp_c);

        if (ret == ESP_OK) {
            portENTER_CRITICAL(&g_data_mux);
            g_temp_c = temp_c;
            g_temp_valid = true;
            portEXIT_CRITICAL(&g_data_mux);			
        } else if (ret == ESP_ERR_NOT_FOUND) {
            portENTER_CRITICAL(&g_data_mux);
            g_temp_valid = false;
            portEXIT_CRITICAL(&g_data_mux);

            if(sensor_desconnected_init)
				ESP_LOGW(TAG, "DS18B20 not connected");
			sensor_desconnected_init = false;
        } else {
            ESP_LOGE(TAG, "DS18B20 read failed: %s", esp_err_to_name(ret));
        }

        /*
         * ds18b20_read_temperature() already waits 750 ms.
         * 4250 ms + 750 ms = about 5000 ms total period.
         */
        vTaskDelay(pdMS_TO_TICKS(4250));
    }
}

uint8_t clock_modes_advance_mode()
{
    display_mode_t new_mode;

    portENTER_CRITICAL(&g_data_mux);

    if (display_mode >= MODE_ROTATION) {
        display_mode = MODE_1;
    } else {
        display_mode =
            static_cast<display_mode_t>(display_mode + 1);
    }

    new_mode = display_mode;

    clock_modes_reset_sequences();

    portEXIT_CRITICAL(&g_data_mux);

    clock_settings_save_mode(static_cast<uint8_t>(new_mode));

    scroll_stop();

    char msg[16];
    snprintf(
        msg,
        sizeof(msg),
        "MODO:%u",
        static_cast<unsigned>(new_mode)
    );

    show_temp_message(msg, 1000);

    ESP_LOGI(
        TAG,
        "Display mode changed to %u",
        static_cast<unsigned>(new_mode)
    );

    return static_cast<uint8_t>(new_mode);
}

esp_err_t clock_modes_set_mode(uint8_t mode)
{
    if (mode < MODE_1 || mode > MODE_ROTATION) {
        return ESP_ERR_INVALID_ARG;
    }

    const display_mode_t new_mode = static_cast<display_mode_t>(mode);

    portENTER_CRITICAL(&g_data_mux);

    display_mode = new_mode;
    clock_modes_reset_sequences();

    portEXIT_CRITICAL(&g_data_mux);

    const esp_err_t save_result =
        clock_settings_save_mode(static_cast<uint8_t>(new_mode));

    scroll_stop();

    char msg[16];
    snprintf(
        msg,
        sizeof(msg),
        "MODO:%u",
        static_cast<unsigned>(new_mode)
    );

    show_temp_message(msg, 1000);

    ESP_LOGI(
        TAG,
        "Display mode set to %u",
        static_cast<unsigned>(new_mode)
    );

    return save_result;
}

uint8_t clock_modes_get_mode()
{
    display_mode_t current_mode;

    portENTER_CRITICAL(&g_data_mux);
    current_mode = display_mode;
    portEXIT_CRITICAL(&g_data_mux);

    return static_cast<uint8_t>(current_mode);
}

static esp_err_t save_network_mode_callback(uint8_t mode, void *)
{
    return clock_settings_save_network_mode(mode);
}

static void handle_network_mode_long_hold(void)
{
    clock_network_mode_t current_mode;

    portENTER_CRITICAL(&g_data_mux);
    current_mode = g_saved_network_mode;
    portEXIT_CRITICAL(&g_data_mux);

    clock_network_mode_t new_mode = current_mode;
    const esp_err_t result = clock_network_cycle_saved_mode(
        current_mode,
        save_network_mode_callback,
        nullptr,
        &new_mode);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save network mode: %s",
                 esp_err_to_name(result));
        show_temp_message("NET SAVE FAIL", 2000);
        return;
    }

    portENTER_CRITICAL(&g_data_mux);
    g_saved_network_mode = new_mode;
    portEXIT_CRITICAL(&g_data_mux);

    show_temp_message_sequence(clock_network_mode_panel_label(new_mode),
                               1800,
                               "REBOOT TO APPLY",
                               2500);

    ESP_LOGI(TAG,
             "Saved network boot mode changed to %s; live interfaces unchanged",
             clock_network_mode_label(new_mode));
}

static void handle_normal_button(button_t btn, ds3231_dev_t *rtc)
{
    switch (btn)
    {
        case BTN_MENU:
            clock_menu_enter();
            break;

			case BTN_UP:
			{
			    clock_modes_advance_mode();
			    break;
			}

        case BTN_DOWN:
        {
            hour_format_t new_format;

            portENTER_CRITICAL(&g_data_mux);

            if (clock_format == FORMAT_12H) {
                clock_format = FORMAT_24H;
            } else {
                clock_format = FORMAT_12H;
            }

            new_format = clock_format;

            portEXIT_CRITICAL(&g_data_mux);

            clock_settings_save_format((uint8_t)new_format);

            show_temp_message(new_format == FORMAT_24H ? "24HRS:ON" : "24HRS:OFF",
                              1000);

            ESP_LOGI(TAG,
                     "Clock format changed to %s",
                     new_format == FORMAT_24H ? "24H" : "12H");

            break;
        }

        default:
            break;
    }
}

void button_task(void *arg)
{
    ds3231_dev_t *rtc = (ds3231_dev_t *)arg;
	QueueHandle_t button_queue = clock_buttons_get_queue();


    TickType_t last_press_time[3] = {
        0,
        0,
        0
    };

    button_t pending_hold_btn = BTN_NONE;
    TickType_t pending_hold_start = 0;
    clock_network_long_hold_t up_network_hold = {};

    bool ignore_until_release = false;
	
	button_t menu_repeat_btn = BTN_NONE;
	TickType_t menu_repeat_start = 0;
	TickType_t menu_last_repeat = 0;

    while (true)
    {
        button_t btn;
        TickType_t now = xTaskGetTickCount();

        /*
         * Use short timeout instead of portMAX_DELAY so the task can check
         * whether a pending button has been held long enough.
         */
		 if (xQueueReceive(button_queue, &btn, pdMS_TO_TICKS(10)))
		 {
		     if (btn < BTN_MENU || btn > BTN_DOWN) {
		         continue;
		     }

		     /*
		      * Important:
		      * After a hold action outside the menu, ignore any queued/bounce events
		      * until all buttons are released.
		      */
		     if (ignore_until_release) {
		         continue;
		     }

		     if (pending_hold_btn == btn && clock_button_is_pressed(btn)) {
		         continue;
		     }

		     if (last_press_time[btn] != 0 &&
		         (now - last_press_time[btn]) < pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) {
		         continue;
		     }

		     last_press_time[btn] = now;

		     /*
		      * Inside menu:
		      * Buttons act immediately.
		      */
			  if (clock_menu_is_active()) 
			  {			  
				clock_menu_handle_button(btn);

			      /*
			       * Inside menu, only UP and DOWN repeat.
			       * MENU should not repeat, because it changes fields.
			       */
			      if (btn == BTN_UP || btn == BTN_DOWN) {
			          menu_repeat_btn = btn;
			          menu_repeat_start = now;
			          menu_last_repeat = now;
			      }

			      continue;
			  }

		     /*
		      * Outside menu:
		      * Do not execute immediately.
		      * Start hold detection.
		      */
		     pending_hold_btn = btn;
		     pending_hold_start = now;

		     ESP_LOGI(TAG, "Button %d pressed, waiting for hold", btn);
		 }	 		 
		 
		 /*
		  * Inside menu:
		  * Auto-repeat UP/DOWN while held.
		  */
		  if (clock_menu_is_active() &&
		      menu_repeat_btn != BTN_NONE)
		 {
		     if (clock_button_is_pressed(menu_repeat_btn))
		     {
		         now = xTaskGetTickCount();

		         if ((now - menu_repeat_start) >= pdMS_TO_TICKS(BUTTON_REPEAT_DELAY_MS))
		         {
		             if ((now - menu_last_repeat) >= pdMS_TO_TICKS(BUTTON_REPEAT_RATE_MS))
		             {
						clock_menu_handle_button(menu_repeat_btn);
		                 menu_last_repeat = now;
		             }
		         }
		     }
		     else
		     {
		         menu_repeat_btn = BTN_NONE;
		     }
		 } 		 

		/*
		 * Outside the menu, UP is resolved on release for its normal action so
		 * a continuous 10-second hold can select network mode without also
		 * changing display mode. MENU and DOWN retain their existing timing.
		 */
		 if (!clock_menu_is_active() &&
		     pending_hold_btn != BTN_NONE &&
		     !ignore_until_release)
        {
            if (clock_button_is_pressed(pending_hold_btn))
            {
                now = xTaskGetTickCount();
				const TickType_t held_ticks = now - pending_hold_start;

				if (pending_hold_btn == BTN_UP) {
				    const uint32_t held_ms =
				        static_cast<uint32_t>(held_ticks * portTICK_PERIOD_MS);

				    if (held_ms >= NETWORK_MODE_HOLD_MS &&
				        clock_network_long_hold_update(&up_network_hold,
				                                       true,
				                                       held_ms)) {
				        ESP_LOGI(TAG, "UP 10-second network mode hold accepted");
				        handle_network_mode_long_hold();
				        xQueueReset(button_queue);
				        pending_hold_btn = BTN_NONE;
				        ignore_until_release = true;
				    }
				} else if (held_ticks >= pdMS_TO_TICKS(BUTTON_HOLD_MS)) {
				    ESP_LOGI(TAG, "Button %d hold accepted", pending_hold_btn);

				    handle_normal_button(pending_hold_btn, rtc);
				    xQueueReset(button_queue);
				    pending_hold_btn = BTN_NONE;
				    ignore_until_release = true;
				}
            }
            else
            {
				now = xTaskGetTickCount();
				const TickType_t held_ticks = now - pending_hold_start;

				if (pending_hold_btn == BTN_UP &&
				    held_ticks >= pdMS_TO_TICKS(BUTTON_HOLD_MS)) {
				    ESP_LOGI(TAG, "UP normal hold accepted on release");
				    handle_normal_button(BTN_UP, rtc);
				    xQueueReset(button_queue);
				} else {
				    ESP_LOGI(TAG, "Button hold cancelled");
				}

				clock_network_long_hold_update(&up_network_hold, false, 0);
                pending_hold_btn = BTN_NONE;
            }
        }

        /*
         * Re-arm buttons only after all are released.
         */
		 if (clock_buttons_all_released())
		 {
		     pending_hold_btn = BTN_NONE;
		     menu_repeat_btn = BTN_NONE;
		     ignore_until_release = false;
		     clock_network_long_hold_update(&up_network_hold, false, 0);
		 }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void check_or_set_default_rtc(ds3231_dev_t *rtc)
{
    if (!rtc) {
        return;
    }

    ds3231_time_t now;

    esp_err_t ret = ds3231_get_time(rtc, &now);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read RTC during startup check: %s",
                 esp_err_to_name(ret));
        return;
    }

    if (!rtc_time_is_valid(&now)) {
        ESP_LOGW(TAG,
                 "RTC time invalid: %04d-%02d-%02d %02d:%02d:%02d DOW=%d",
                 now.year,
                 now.month,
                 now.day,
                 now.hour,
                 now.minute,
                 now.second,
                 now.day_of_week);

        ds3231_time_t fixed_time = default_time;

        fixed_time.day_of_week = clock_menu_calculate_weekday(
            fixed_time.day,
            fixed_time.month,
            fixed_time.year
        );

        ret = ds3231_set_time(rtc, &fixed_time);

        if (ret == ESP_OK) {
            ESP_LOGW(TAG,
                     "RTC set to default: %04d-%02d-%02d %02d:%02d:%02d DOW=%d",
                     fixed_time.year,
                     fixed_time.month,
                     fixed_time.day,
                     fixed_time.hour,
                     fixed_time.minute,
                     fixed_time.second,
                     fixed_time.day_of_week);

            portENTER_CRITICAL(&g_data_mux);
            g_now = fixed_time;
            g_rtc_valid = true;
            portEXIT_CRITICAL(&g_data_mux);
        } else {
            ESP_LOGE(TAG, "Failed to set default RTC time: %s",
                     esp_err_to_name(ret));
        }
    } else {
        ESP_LOGI(TAG,
                 "RTC startup time valid: %04d-%02d-%02d %02d:%02d:%02d DOW=%d",
                 now.year,
                 now.month,
                 now.day,
                 now.hour,
                 now.minute,
                 now.second,
                 now.day_of_week);
    }
}

// =============================== APP MAIN ===============================

extern "C" void app_main(void)
{
	clock_network_result_gate_init(&g_network_result_gate);

	clock_ota_print_app_info();
	clock_ota_confirm_app_if_needed();

	clock_logo_init();

	esp_err_t sd_err = clock_sd_card_init();
	if (sd_err != ESP_OK) {
	    ESP_LOGW(TAG, "SD card init failed, using compiled logo fallback: %s", esp_err_to_name(sd_err));
	} else {
	    esp_err_t logo_err = clock_logo_reload_from_sd();
	    if (logo_err != ESP_OK) {
	        ESP_LOGW(TAG, "Logo reload at boot failed, keeping fallback: %s", esp_err_to_name(logo_err));
	    }
	}

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
		
	ESP_LOGI(TAG, "Starting HUB75");

    if (!driver.begin()) {
        ESP_LOGE(TAG, "Driver start failed");
        return;
    }
	
	ESP_ERROR_CHECK(clock_settings_init());	

	esp_err_t palette_init_ret = clock_palette_init();
	if (palette_init_ret != ESP_OK) {
	    ESP_LOGW(TAG,
	             "Palette NVS load failed, compiled defaults remain active: %s",
	             esp_err_to_name(palette_init_ret));
	}
	
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

	g_saved_network_mode = static_cast<clock_network_mode_t>(
	    clock_settings_load_network_mode());
	ESP_LOGI(TAG,
	         "Boot-selected network mode: %s",
	         clock_network_mode_label(g_saved_network_mode));

	brightness_level = clock_settings_load_brightness(5);

	if (brightness_level < 1) {
	    brightness_level = 1;
	}

	if (brightness_level > 10) {
	    brightness_level = 10;
	}

	temporal_brightness = brightness_level;

	driver.set_brightness(brightness_level_to_hub75(brightness_level));	
	
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

	const esp_err_t network_start_result = clock_network_start(
	    g_saved_network_mode,
	    clock_protocol_rx_callback,
	    show_network_status_message);
	if (network_start_result != ESP_OK) {
	    ESP_LOGE(TAG,
	             "Failed to start network boot coordinator: %s",
	             esp_err_to_name(network_start_result));
	    show_network_status_message("NET NOT CONNECTED", 3000);
	}
}
