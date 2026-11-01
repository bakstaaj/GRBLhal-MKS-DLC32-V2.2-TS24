/*
 * MKS TS24-R V2.1 UI port scaffold.
 *
 * The reference MKS TS35 UI uses LVGL 6.1.1 and TFT_eSPI under Arduino.
 * This grblHAL-side port replaces the Arduino display/touch glue with
 * ESP-IDF SPI/GPIO. Layouts and actions are adapted from MKS's TS35 UI for
 * the 320 x 240 TS24-R panel.
 */

#include "ts24_ui.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "driver.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "grbl/grbllib.h"
#include "grbl/gcode.h"
#include "grbl/protocol.h"
#include "grbl/state_machine.h"
#include "grbl/system.h"
#include "grbl/stream_file.h"
#include "grbl/vfs.h"

#include "lvgl.h"

extern lv_font_t lv_font_roboto_12_subpx;

#if !defined(BOARD_MKS_DLC32_V2P0)
#error "MKS_TS24_UI currently targets BOARD_MKS_DLC32_V2P0 only."
#endif

#define TS24_WIDTH              320
#define TS24_HEIGHT             240
#define TS24_SPI_HOST           SPI3_HOST
#define TS24_PIN_SCLK           GPIO_NUM_18
#define TS24_PIN_MISO           GPIO_NUM_19
#define TS24_PIN_MOSI           GPIO_NUM_23
#define TS24_PIN_LCD_CS         GPIO_NUM_25
#define TS24_PIN_TOUCH_CS       GPIO_NUM_26
#define TS24_PIN_LCD_RESET      GPIO_NUM_27
#define TS24_PIN_BACKLIGHT      GPIO_NUM_5
#define TS24_PIN_LCD_DC         GPIO_NUM_33

#define TS24_BUFFER_LINES       8
#define TS24_JOG_FEED_SLOW      500
#define TS24_JOG_FEED_MEDIUM    1000
#define TS24_JOG_FEED_FAST      2000
#define TS24_FILE_COUNT         5
#define TS24_PATH_LEN           128
#define TS24_NAME_LEN           64

typedef struct {
    char name[TS24_NAME_LEN];
    char path[TS24_PATH_LEN];
    bool directory;
} ts24_file_entry_t;

static const char *TAG = "mks_ts24_ui";

static spi_device_handle_t lcd_device;
static spi_device_handle_t touch_device;
static lv_disp_buf_t display_buffer;
static DMA_ATTR lv_color_t display_pixels[TS24_WIDTH * TS24_BUFFER_LINES];
static lv_obj_t *state_label;
static lv_obj_t *position_label;
static lv_obj_t *work_position_label;
static lv_obj_t *active_screen;
static lv_obj_t *control_buttons[6];
static bool control_layout_valid, control_layout_alarm;
static lv_obj_t *jog_step_button;
static lv_obj_t *jog_feed_button;
static lv_obj_t *alarm_lock_button;
static lv_obj_t *alarm_lock_label;
static lv_obj_t *main_pause_button, *main_pause_label, *main_stop_button;
static lv_obj_t *main_ready_label, *main_setup_button;
static lv_obj_t *mode_status_label, *mode_toggle_label;
static char file_browser_path[TS24_PATH_LEN] = "/";
static char selected_filename[TS24_PATH_LEN] = "";
static ts24_file_entry_t file_entries[TS24_FILE_COUNT];
static uint8_t file_entry_count;
static uint16_t file_page_offset;
static bool file_has_more;
static int jog_step_index = 1;
static const float jog_steps[] = {0.1f, 1.0f, 10.0f};
static int jog_feed_index = 1;
static const int jog_feeds[] = {TS24_JOG_FEED_SLOW, TS24_JOG_FEED_MEDIUM, TS24_JOG_FEED_FAST};
static lv_style_t style_screen, style_button, style_button_pressed, style_home_button, style_home_button_pressed;
static lv_style_t style_panel, style_label, style_label_small, style_label_alarm, style_dro, style_accent;
static bool ui_started;

static void set_hidden_if_changed(lv_obj_t *obj, bool hidden)
{
    if(obj && lv_obj_get_hidden(obj) != hidden)
        lv_obj_set_hidden(obj, hidden);
}

static void lcd_transfer(const void *bytes, size_t length, bool data)
{
    spi_transaction_t transaction = {0};
    transaction.length = length * 8;
    transaction.tx_buffer = bytes;
    gpio_set_level(TS24_PIN_LCD_DC, data ? 1 : 0);
    if(spi_device_transmit(lcd_device, &transaction) != ESP_OK)
        ESP_LOGE(TAG, "LCD SPI transfer failed");
}

static void lcd_command(uint8_t command)
{
    lcd_transfer(&command, sizeof(command), false);
}

static void lcd_command_data(uint8_t command, const uint8_t *data, size_t length)
{
    gpio_set_level(TS24_PIN_LCD_CS, 0);
    lcd_command(command);
    if(length)
        lcd_transfer(data, length, true);
    gpio_set_level(TS24_PIN_LCD_CS, 1);
}

static void lcd_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t range[4];

    range[0] = (uint8_t)(x1 >> 8);
    range[1] = (uint8_t)x1;
    range[2] = (uint8_t)(x2 >> 8);
    range[3] = (uint8_t)x2;
    lcd_command(0x2A); /* CASET */
    lcd_transfer(range, sizeof(range), true);

    range[0] = (uint8_t)(y1 >> 8);
    range[1] = (uint8_t)y1;
    range[2] = (uint8_t)(y2 >> 8);
    range[3] = (uint8_t)y2;
    lcd_command(0x2B); /* RASET */
    lcd_transfer(range, sizeof(range), true);
    lcd_command(0x2C); /* RAMWR */
}

static void display_flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels)
{
    spi_transaction_t transaction = {0};
    const size_t width = (size_t)(area->x2 - area->x1 + 1);
    const size_t height = (size_t)(area->y2 - area->y1 + 1);

    gpio_set_level(TS24_PIN_LCD_CS, 0);
    lcd_set_window((uint16_t)area->x1, (uint16_t)area->y1,
                   (uint16_t)area->x2, (uint16_t)area->y2);
    gpio_set_level(TS24_PIN_LCD_DC, 1);
    transaction.length = width * height * sizeof(lv_color_t) * 8;
    transaction.tx_buffer = pixels;
    if(spi_device_transmit(lcd_device, &transaction) != ESP_OK)
        ESP_LOGE(TAG, "Display flush failed");
    gpio_set_level(TS24_PIN_LCD_CS, 1);
    lv_disp_flush_ready(driver);
}

static esp_err_t lcd_init(void)
{
    static const uint8_t color_mode[] = {0x55}; /* RGB565 */
    static const uint8_t memory_access[] = {0xA8}; /* BGR color order; geometry unchanged */
    static const uint8_t interface_control[] = {0x0A, 0x82};
    static const uint8_t ram_control[] = {0x00, 0xE0};
    static const uint8_t porch[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    static const uint8_t gate[] = {0x35};
    static const uint8_t vcom[] = {0x28};
    static const uint8_t lcm[] = {0x0C};
    static const uint8_t vdv_vrh_enable[] = {0x01, 0xFF};
    static const uint8_t vrh[] = {0x10};
    static const uint8_t vdv[] = {0x20};
    static const uint8_t frame_rate[] = {0x0F};
    static const uint8_t power[] = {0xA4, 0xA1};
    static const uint8_t positive_gamma[] = {
        0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32,
        0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17
    };
    static const uint8_t negative_gamma[] = {
        0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31,
        0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E
    };

    gpio_set_level(TS24_PIN_LCD_CS, 1);
    gpio_set_level(TS24_PIN_LCD_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TS24_PIN_LCD_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_command_data(0x01, NULL, 0); /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_command_data(0x11, NULL, 0); /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_command_data(0x3A, color_mode, sizeof(color_mode));
    lcd_command_data(0x36, memory_access, sizeof(memory_access));
    lcd_command_data(0xB6, interface_control, sizeof(interface_control));
    lcd_command_data(0xB0, ram_control, sizeof(ram_control));
    lcd_command_data(0xB2, porch, sizeof(porch));
    lcd_command_data(0xB7, gate, sizeof(gate));
    lcd_command_data(0xBB, vcom, sizeof(vcom));
    lcd_command_data(0xC0, lcm, sizeof(lcm));
    lcd_command_data(0xC2, vdv_vrh_enable, sizeof(vdv_vrh_enable));
    lcd_command_data(0xC3, vrh, sizeof(vrh));
    lcd_command_data(0xC4, vdv, sizeof(vdv));
    lcd_command_data(0xC6, frame_rate, sizeof(frame_rate));
    lcd_command_data(0xD0, power, sizeof(power));
    lcd_command_data(0xE0, positive_gamma, sizeof(positive_gamma));
    lcd_command_data(0xE1, negative_gamma, sizeof(negative_gamma));
    lcd_command_data(0x20, NULL, 0); /* INVOFF: preserve LVGL's requested RGB colors */
    lcd_command_data(0x13, NULL, 0); /* NORON */
    lcd_command_data(0x29, NULL, 0); /* DISPON */
    vTaskDelay(pdMS_TO_TICKS(20));

    return ESP_OK;
}

static esp_err_t display_bus_init(void)
{
    const gpio_config_t output_pins = {
        .pin_bit_mask = (1ULL << TS24_PIN_LCD_CS) |
                        (1ULL << TS24_PIN_LCD_DC) |
                        (1ULL << TS24_PIN_LCD_RESET) |
                        (1ULL << TS24_PIN_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    const spi_bus_config_t bus_config = {
        .mosi_io_num = TS24_PIN_MOSI,
        .miso_io_num = TS24_PIN_MISO,
        .sclk_io_num = TS24_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TS24_WIDTH * TS24_BUFFER_LINES * sizeof(lv_color_t)
    };
    const spi_device_interface_config_t lcd_config = {
        .clock_speed_hz = 40000000,
        .mode = 0,
        .spics_io_num = -1, /* CS held through address window and pixel data */
        .queue_size = 1
    };
    const spi_device_interface_config_t touch_config = {
        .clock_speed_hz = 2000000,
        .mode = 0,
        .spics_io_num = TS24_PIN_TOUCH_CS,
        .queue_size = 1
    };
    esp_err_t error;

    if((error = gpio_config(&output_pins)) != ESP_OK)
        return error;

    gpio_set_level(TS24_PIN_BACKLIGHT, 1); /* keep panel dark during SPI setup */
    /* grblHAL's SD-card SPI bus claims DMA channel 1 during startup. The TS24
       display has its own SPI host, so give it channel 2 instead of reusing
       the already-owned channel. */
    if((error = spi_bus_initialize(TS24_SPI_HOST, &bus_config, SPI_DMA_CH2)) != ESP_OK)
        return error;
    if((error = spi_bus_add_device(TS24_SPI_HOST, &lcd_config, &lcd_device)) != ESP_OK)
        return error;
    if((error = spi_bus_add_device(TS24_SPI_HOST, &touch_config, &touch_device)) != ESP_OK)
        return error;

    return lcd_init();
}

static uint16_t touch_sample(uint8_t command)
{
    uint8_t tx[3] = {command, 0, 0};
    uint8_t rx[3] = {0};
    spi_transaction_t transaction = {0};

    transaction.length = sizeof(tx) * 8;
    transaction.tx_buffer = tx;
    transaction.rx_buffer = rx;
    if(spi_device_transmit(touch_device, &transaction) != ESP_OK)
        return 0;

    return (uint16_t)((((uint16_t)rx[1] << 8) | rx[2]) >> 3) & 0x0FFF;
}

static bool touch_read(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    static lv_point_t last_point = {.x = TS24_WIDTH / 2, .y = TS24_HEIGHT / 2};
    uint16_t z1 = touch_sample(0xB0);
    uint16_t z2 = touch_sample(0xC0);

    (void)driver;
    if(z1 > 100 && z2 > z1) {
        /* Initial landscape mapping; calibrate raw endpoints on the real TS24-R. */
        uint16_t raw_x = touch_sample(0xD0);
        uint16_t raw_y = touch_sample(0x90);
        raw_x = raw_x < 300 ? 300 : (raw_x > 3800 ? 3800 : raw_x);
        raw_y = raw_y < 300 ? 300 : (raw_y > 3800 ? 3800 : raw_y);
        last_point.x = (lv_coord_t)(((uint32_t)(raw_y - 300) * (TS24_WIDTH - 1)) / 3500);
        last_point.y = (lv_coord_t)(TS24_HEIGHT - 1 -
                           (((uint32_t)(raw_x - 300) * (TS24_HEIGHT - 1)) / 3500));
        data->state = LV_INDEV_STATE_PR;
    } else
        data->state = LV_INDEV_STATE_REL;

    data->point = last_point;
    return false;
}

static const char *machine_state_name(sys_state_t state)
{
    if(state & STATE_ALARM)
        return "ALARM";
    if(state & STATE_HOLD)
        return "HOLD";
    if(state & STATE_JOG)
        return "JOG";
    if(state & STATE_CYCLE)
        return "RUN";
    return state == STATE_IDLE ? "IDLE" : "BUSY";
}

static void refresh_status(void)
{
    float machine_position[N_AXIS] = {0};
    char text[96], work_text[96];
    uint_fast8_t axis;
    sys_state_t state = state_get();

    if(state_label) {
        if(state & STATE_ALARM)
            snprintf(text, sizeof(text), "ALARM %u", (unsigned)sys.alarm);
        else
            snprintf(text, sizeof(text), "%s", machine_state_name(state));
        lv_label_set_text(state_label, text);
    }
    system_convert_array_steps_to_mpos(machine_position, sys.position);
    snprintf(text, sizeof(text), "X%.2f  Y%.2f  Z%.2f",
             machine_position[0], machine_position[1], machine_position[2]);
    if(position_label)
        lv_label_set_text(position_label, text);
    for(axis = 0; axis < 3; axis++)
        machine_position[axis] -= gc_get_offset(axis, true);
    snprintf(work_text, sizeof(work_text), "X%.2f  Y%.2f  Z%.2f",
             machine_position[0], machine_position[1], machine_position[2]);
    if(work_position_label)
        lv_label_set_text(work_position_label, work_text);
    if(alarm_lock_button) {
        bool alarm = (state & STATE_ALARM) != 0;
        set_hidden_if_changed(alarm_lock_button, !alarm);
        if(!control_layout_valid || alarm != control_layout_alarm) {
            lv_coord_t top = 72;
            lv_coord_t height = alarm ? 27 : 32;
            lv_coord_t gap = 1;
            uint8_t i;
            control_layout_alarm = alarm;
            control_layout_valid = true;
            for(i = 0; i < 6; i++) {
                lv_obj_set_size(control_buttons[i], 100, height);
                lv_obj_set_pos(control_buttons[i], 212, top + i * (height + gap));
            }
        }
    }
    if(main_pause_button) {
        bool job_active = (state & (STATE_CYCLE | STATE_HOLD)) != 0;
        set_hidden_if_changed(main_pause_button, !job_active);
        set_hidden_if_changed(main_stop_button, !job_active);
        set_hidden_if_changed(main_ready_label, job_active);
        set_hidden_if_changed(main_setup_button, job_active);
        lv_label_set_text(main_pause_label, (state & STATE_HOLD) ? "RESUME" : "PAUSE");
    }
    if(mode_status_label)
        lv_label_set_text(mode_status_label, settings.mode == Mode_Laser ? "Current mode: LASER" : "Current mode: CNC");
    if(mode_toggle_label)
        lv_label_set_text(mode_toggle_label, settings.mode == Mode_Laser ? "SWITCH TO CNC" : "SWITCH TO LASER");
}

static void enqueue_jog(char axis, int direction)
{
    char command[64];
    snprintf(command, sizeof(command), "$J=G91 G21 %c%+.1f F%d", axis,
             direction * jog_steps[jog_step_index], jog_feeds[jog_feed_index]);
    if(!grbl.enqueue_gcode(command))
        ESP_LOGW(TAG, "Jog rejected (controller is not idle or queue is busy)");
}

#define JOG_EVENT(name, axis_name, dir) \
    static void name(lv_obj_t *obj, lv_event_t event) \
    { (void)obj; if(event == LV_EVENT_CLICKED) enqueue_jog(axis_name, dir); }

JOG_EVENT(jog_x_minus, 'X', -1)
JOG_EVENT(jog_x_plus,  'X',  1)
JOG_EVENT(jog_y_minus, 'Y', -1)
JOG_EVENT(jog_y_plus,  'Y',  1)
JOG_EVENT(jog_z_minus, 'Z', -1)
JOG_EVENT(jog_z_plus,  'Z',  1)

static void select_jog_step(lv_obj_t *obj, lv_event_t event)
{
    char text[20];
    lv_obj_t *label;
    (void)obj;
    if(event == LV_EVENT_CLICKED) {
        jog_step_index = (jog_step_index + 1) % 3;
        snprintf(text, sizeof(text), "STEP %.1f mm", jog_steps[jog_step_index]);
        if((label = lv_obj_get_child(jog_step_button, NULL)))
            lv_label_set_text(label, text);
    }
}

static void select_jog_feed(lv_obj_t *obj, lv_event_t event)
{
    char text[24];
    lv_obj_t *label;
    (void)obj;
    if(event == LV_EVENT_CLICKED) {
        jog_feed_index = (jog_feed_index + 1) % 3;
        snprintf(text, sizeof(text), "FEED %d", jog_feeds[jog_feed_index]);
        if((label = lv_obj_get_child(jog_feed_button, NULL)))
            lv_label_set_text(label, text);
    }
}

static void home_machine(lv_obj_t *obj, lv_event_t event)
{
    (void)obj;
    if(event == LV_EVENT_CLICKED && !grbl.enqueue_gcode("$H"))
        ESP_LOGW(TAG, "Homing command rejected");
}

static void unlock_alarm(lv_obj_t *obj, lv_event_t event)
{
    char command[] = "$X";
    sys_state_t state;
    (void)obj;
    if(event != LV_EVENT_CLICKED)
        return;

    state = state_get();
    if(!(state & STATE_ALARM))
        return;

    if(!grbl.enqueue_gcode(command))
        ESP_LOGW(TAG, "TS24 UNLOCK tap: $X could not be queued (state=%u, alarm=%u)",
                 (unsigned)state, (unsigned)sys.alarm);
    else
        ESP_LOGI(TAG, "TS24 UNLOCK tap: queued $X (alarm=%u)", (unsigned)sys.alarm);
}

static void toggle_machine_mode(lv_obj_t *obj, lv_event_t event)
{
    char command[] = "$32=0";
    (void)obj;
    if(event != LV_EVENT_CLICKED)
        return;

    sys_state_t state = state_get();
    if(state != STATE_IDLE && !(state & STATE_ALARM)) {
        ESP_LOGW(TAG, "Mode change rejected while machine is active");
        return;
    }

    command[4] = settings.mode == Mode_Laser ? '0' : '1';
    if(!grbl.enqueue_gcode(command))
        ESP_LOGW(TAG, "Could not queue CNC/Laser mode change");
}

static void zero_xy(lv_obj_t *obj, lv_event_t event)
{
    (void)obj;
    if(event == LV_EVENT_CLICKED && !grbl.enqueue_gcode("G10 L20 P1 X0 Y0"))
        ESP_LOGW(TAG, "XY work zero command rejected");
}

static void zero_z(lv_obj_t *obj, lv_event_t event)
{
    (void)obj;
    if(event == LV_EVENT_CLICKED && !grbl.enqueue_gcode("G10 L20 P1 Z0"))
        ESP_LOGW(TAG, "Z work zero command rejected");
}

static void reboot_controller(lv_obj_t *obj, lv_event_t event)
{
    (void)obj;
    if(event == LV_EVENT_CLICKED) {
        sys_state_t state = state_get();
        if(state & (STATE_CYCLE | STATE_HOLD | STATE_JOG | STATE_HOMING)) {
            ESP_LOGW(TAG, "Reboot refused while motion or a job is active");
            return;
        }
        ESP_LOGW(TAG, "Controller reboot requested from TS24 control screen");
        esp_restart();
    }
}

static void feed_hold(lv_obj_t *obj, lv_event_t event)
{
    (void)obj;
    if(event == LV_EVENT_CLICKED)
        grbl.enqueue_realtime_command('!');
}

static void pause_resume_job(lv_obj_t *obj, lv_event_t event)
{
    sys_state_t state;
    (void)obj;
    if(event != LV_EVENT_CLICKED)
        return;
    state = state_get();
    if(state & STATE_HOLD)
        grbl.enqueue_realtime_command('~');
    else if(state & STATE_CYCLE)
        grbl.enqueue_realtime_command('!');
}

static void cycle_start(lv_obj_t *obj, lv_event_t event)
{
    (void)obj;
    if(event == LV_EVENT_CLICKED)
        grbl.enqueue_realtime_command('~');
}

static void start_selected_file(lv_obj_t *obj, lv_event_t event)
{
    (void)obj;
    if(event != LV_EVENT_CLICKED || selected_filename[0] == '\0')
        return;
    if(state_get() != STATE_IDLE) {
        ESP_LOGW(TAG, "File start ignored because the controller is not idle");
        return;
    }
    if(stream_redirect_read(selected_filename, NULL, NULL) == NULL) {
        ESP_LOGW(TAG, "Could not start G-code file: %s", selected_filename);
        if(state_label)
            lv_label_set_text(state_label, "FILE ERROR");
    } else if(state_label)
        lv_label_set_text(state_label, "STARTING");
}

static void stop_job(lv_obj_t *obj, lv_event_t event)
{
    (void)obj;
    if(event == LV_EVENT_CLICKED)
        grbl.enqueue_realtime_command(0x18); /* Ctrl-X: abort/reset the active stream */
}

static void show_home(lv_obj_t *obj, lv_event_t event);
static void show_control(lv_obj_t *obj, lv_event_t event);
static void show_files(lv_obj_t *obj, lv_event_t event);
static void show_calibration_info(lv_obj_t *obj, lv_event_t event);
static void create_jobs_screen(void);

static void init_styles(void)
{
    lv_style_copy(&style_screen, &lv_style_plain);
    style_screen.body.main_color = LV_COLOR_MAKE(0x13, 0x12, 0x1A);
    style_screen.body.grad_color = style_screen.body.main_color;
    style_screen.body.opa = LV_OPA_COVER;
    style_screen.text.color = LV_COLOR_WHITE;

    lv_style_copy(&style_panel, &lv_style_plain);
    style_panel.body.main_color = LV_COLOR_MAKE(0x1F, 0x23, 0x33);
    style_panel.body.grad_color = style_panel.body.main_color;
    style_panel.body.radius = 5;
    style_panel.body.border.color = LV_COLOR_MAKE(0x3B, 0x43, 0x57);
    style_panel.body.border.width = 1;
    style_panel.body.border.opa = LV_OPA_COVER;
    style_panel.body.opa = LV_OPA_COVER;

    lv_style_copy(&style_button, &lv_style_btn_rel);
    style_button.body.main_color = LV_COLOR_MAKE(0x2A, 0x36, 0x49);
    style_button.body.grad_color = LV_COLOR_MAKE(0x22, 0x2B, 0x3B);
    style_button.body.border.color = LV_COLOR_MAKE(0x49, 0x5B, 0x72);
    style_button.body.border.width = 1;
    style_button.body.radius = 5;
    style_button.text.color = LV_COLOR_WHITE;

    lv_style_copy(&style_button_pressed, &lv_style_btn_pr);
    style_button_pressed.body.main_color = LV_COLOR_MAKE(0x19, 0x8A, 0x68);
    style_button_pressed.body.grad_color = LV_COLOR_MAKE(0x16, 0x6E, 0x56);
    style_button_pressed.text.color = LV_COLOR_WHITE;

    lv_style_copy(&style_home_button, &style_button);
    style_home_button.body.radius = LV_RADIUS_CIRCLE;
    lv_style_copy(&style_home_button_pressed, &style_button_pressed);
    style_home_button_pressed.body.radius = LV_RADIUS_CIRCLE;

    lv_style_copy(&style_label, &lv_style_plain);
    style_label.body.opa = LV_OPA_TRANSP;
    style_label.text.color = LV_COLOR_WHITE;

    lv_style_copy(&style_label_small, &style_label);
    style_label_small.text.font = &lv_font_roboto_12_subpx;

    lv_style_copy(&style_label_alarm, &style_label_small);
    style_label_alarm.text.color = LV_COLOR_MAKE(0xFF, 0x35, 0x35);

    lv_style_copy(&style_dro, &style_label);
    style_dro.text.color = LV_COLOR_MAKE(0xD7, 0xF5, 0x4A);

    lv_style_copy(&style_accent, &style_label);
    style_accent.text.color = LV_COLOR_MAKE(0x45, 0xA7, 0xE8);

}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_coord_t x,
                             lv_coord_t y, lv_coord_t w, lv_coord_t h,
                             lv_event_cb_t callback)
{
    lv_obj_t *button = lv_btn_create(parent, NULL);
    lv_obj_set_size(button, w, h);
    lv_obj_set_pos(button, x, y);
    lv_btn_set_style(button, LV_BTN_STYLE_REL, &style_button);
    lv_btn_set_style(button, LV_BTN_STYLE_PR, &style_button_pressed);
    lv_btn_set_style(button, LV_BTN_STYLE_TGL_REL, &style_button);
    lv_btn_set_style(button, LV_BTN_STYLE_TGL_PR, &style_button_pressed);
    lv_obj_set_event_cb(button, callback);
    lv_obj_t *label = lv_label_create(button, NULL);
    lv_label_set_text(label, text);
    lv_label_set_style(label, LV_LABEL_STYLE_MAIN, &style_label);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_align(label, LV_LABEL_ALIGN_CENTER);
    lv_obj_set_width(label, w - 10);
    lv_obj_align(label, button, LV_ALIGN_CENTER, 0, 0);
    return button;
}

static lv_obj_t *make_small_button(lv_obj_t *parent, const char *text, lv_coord_t x,
                                   lv_coord_t y, lv_coord_t w, lv_coord_t h,
                                   lv_event_cb_t callback)
{
    lv_obj_t *button = make_button(parent, text, x, y, w, h, callback);
    lv_obj_t *label = lv_obj_get_child(button, NULL);
    lv_label_set_style(label, LV_LABEL_STYLE_MAIN, &style_label_small);
    lv_obj_align(label, button, LV_ALIGN_CENTER, 0, 0);
    return button;
}

static lv_obj_t *make_screen(const char *title)
{
    state_label = NULL;
    position_label = NULL;
    work_position_label = NULL;
    memset(control_buttons, 0, sizeof(control_buttons));
    control_layout_valid = false;
    alarm_lock_button = NULL;
    alarm_lock_label = NULL;
    main_pause_button = main_pause_label = main_stop_button = NULL;
    main_ready_label = main_setup_button = NULL;
    mode_status_label = mode_toggle_label = NULL;
    lv_obj_t *screen = lv_obj_create(NULL, NULL);
    lv_obj_t *header = lv_label_create(screen, NULL);
    lv_obj_set_style(screen, &style_screen);
    lv_label_set_style(header, LV_LABEL_STYLE_MAIN, &style_accent);
    lv_label_set_text(header, title);
    lv_obj_set_pos(header, 10, 7);
    lv_obj_set_size(header, 220, 24);
    return screen;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                            lv_coord_t w, lv_coord_t h, const lv_style_t *style)
{
    lv_obj_t *label = lv_label_create(parent, NULL);
    lv_label_set_text(label, text);
    lv_label_set_style(label, LV_LABEL_STYLE_MAIN, style ? style : &style_label);
    lv_label_set_long_mode(label, LV_LABEL_LONG_BREAK);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    return label;
}

static void load_screen(lv_obj_t *screen)
{
    lv_obj_t *previous = active_screen;
    active_screen = screen;
    lv_scr_load(screen);
    if(previous && previous != screen)
        lv_obj_del(previous);
}

static bool is_gcode_file(const char *name)
{
    const char *extension = strrchr(name, '.');
    if(extension == NULL)
        return false;
    extension++;
    return !strcasecmp(extension, "nc") || !strcasecmp(extension, "ncc") ||
           !strcasecmp(extension, "ngc") || !strcasecmp(extension, "cnc") ||
           !strcasecmp(extension, "gcode") || !strcasecmp(extension, "tap") ||
           !strcasecmp(extension, "txt");
}

static void refresh_file_entries(void)
{
    vfs_dir_t *directory;
    vfs_dirent_t *entry;

    memset(file_entries, 0, sizeof(file_entries));
    file_entry_count = 0;
    file_has_more = false;
    if((directory = vfs_opendir(file_browser_path)) == NULL)
        return;

    uint16_t filtered_index = 0;
    while((entry = vfs_readdir(directory)) != NULL) {
        size_t name_length, base_length, path_length, prefix_length;
        if(entry->name[0] == '\0' || entry->st_mode.hidden ||
           !strcmp(entry->name, ".") || !strcmp(entry->name, ".."))
            continue;
        if(!entry->st_mode.directory && !is_gcode_file(entry->name))
            continue;
        if(filtered_index++ < file_page_offset)
            continue;
        if(file_entry_count == TS24_FILE_COUNT) {
            file_has_more = true;
            break;
        }
        name_length = strnlen(entry->name, sizeof(entry->name));
        base_length = strlen(file_browser_path);
        prefix_length = !strcmp(file_browser_path, "/") ? 1 : base_length + 1;
        path_length = prefix_length + name_length;
        if(name_length >= sizeof(file_entries[file_entry_count].name) ||
           path_length >= sizeof(file_entries[file_entry_count].path))
            continue;
        memcpy(file_entries[file_entry_count].name, entry->name, name_length + 1);
        if(prefix_length == 1) {
            file_entries[file_entry_count].path[0] = '/';
            memcpy(file_entries[file_entry_count].path + 1, entry->name, name_length + 1);
        } else {
            memcpy(file_entries[file_entry_count].path, file_browser_path, base_length);
            file_entries[file_entry_count].path[base_length] = '/';
            memcpy(file_entries[file_entry_count].path + base_length + 1,
                   entry->name, name_length + 1);
        }
        file_entries[file_entry_count].directory = entry->st_mode.directory;
        file_entry_count++;
    }
    vfs_closedir(directory);
}

static void select_file_entry(lv_obj_t *obj, lv_event_t event)
{
    ts24_file_entry_t *entry;
    if(event != LV_EVENT_CLICKED || (entry = lv_obj_get_user_data(obj)) == NULL)
        return;

    if(entry->directory) {
        snprintf(file_browser_path, sizeof(file_browser_path), "%s", entry->path);
        file_page_offset = 0;
        show_files(NULL, LV_EVENT_CLICKED);
    } else {
        snprintf(selected_filename, sizeof(selected_filename), "%s", entry->path);
        create_jobs_screen();
    }
}

static void parent_directory(lv_obj_t *obj, lv_event_t event)
{
    char *separator;
    (void)obj;
    if(event != LV_EVENT_CLICKED || !strcmp(file_browser_path, "/"))
        return;
    separator = strrchr(file_browser_path, '/');
    if(separator == file_browser_path)
        file_browser_path[1] = '\0';
    else if(separator)
        *separator = '\0';
    file_page_offset = 0;
    show_files(NULL, LV_EVENT_CLICKED);
}

static void previous_file_page(lv_obj_t *obj, lv_event_t event)
{
    (void)obj;
    if(event == LV_EVENT_CLICKED && file_page_offset >= TS24_FILE_COUNT) {
        file_page_offset -= TS24_FILE_COUNT;
        show_files(NULL, LV_EVENT_CLICKED);
    }
}

static void next_file_page(lv_obj_t *obj, lv_event_t event)
{
    (void)obj;
    if(event == LV_EVENT_CLICKED && file_has_more) {
        file_page_offset += TS24_FILE_COUNT;
        show_files(NULL, LV_EVENT_CLICKED);
    }
}

static void create_file_screen(void)
{
    lv_obj_t *screen = make_screen("FILE / SD CARD");
    uint8_t index;

    refresh_file_entries();
    for(index = 0; index < file_entry_count; index++) {
        char row_text[TS24_NAME_LEN + 4];
        lv_obj_t *button;
        snprintf(row_text, sizeof(row_text), "%s%s", file_entries[index].name,
                 file_entries[index].directory ? "/" : "");
        button = make_button(screen, row_text, 10, 37 + index * 30, 300, 28,
                             select_file_entry);
        lv_obj_set_user_data(button, &file_entries[index]);
    }
    if(file_entry_count) {
        char page_text[TS24_PATH_LEN + 12];
        snprintf(page_text, sizeof(page_text), "%s  %u", file_browser_path,
                 (unsigned)(file_page_offset / TS24_FILE_COUNT + 1));
        make_label(screen, page_text, 10, 188, 300, 17, &style_label);
    } else
        make_label(screen, "No files. Check SD card / grblHAL storage.",
                   10, 188, 300, 17, &style_label);
    make_button(screen, "PREV", 8, 208, 72, 25, previous_file_page);
    make_button(screen, "NEXT", 84, 208, 72, 25, next_file_page);
    make_button(screen, "UP", 160, 208, 72, 25, parent_directory);
    make_button(screen, "BACK", 236, 208, 72, 25, show_home);
    load_screen(screen);
}

static void create_home_screen(void)
{
    lv_obj_t *screen = make_screen("MKS DLC32");
    lv_obj_t *card = lv_cont_create(screen, NULL);
    lv_obj_set_style(card, &style_panel);
    lv_obj_set_size(card, 300, 90);
    lv_obj_set_pos(card, 10, 37);
    make_label(card, "MPos", 10, 4, 50, 18, &style_accent);
    position_label = make_label(card, "X 0.00   Y 0.00   Z 0.00", 10, 22, 280, 20, &style_label);
    make_label(card, "WPos", 10, 47, 50, 18, &style_accent);
    work_position_label = make_label(card, "X 0.00   Y 0.00   Z 0.00", 10, 65, 280, 20, &style_label);

    make_button(screen, "CONTROL", 10, 132, 94, 48, show_control);
    make_button(screen, "FILES", 113, 132, 94, 48, show_files);
    main_setup_button = make_button(screen, "SETUP", 216, 132, 94, 48, show_calibration_info);

    state_label = lv_label_create(screen, NULL);
    lv_label_set_text(state_label, "STARTING");
    lv_label_set_style(state_label, LV_LABEL_STYLE_MAIN, &style_label);
    lv_obj_align(state_label, NULL, LV_ALIGN_IN_TOP_RIGHT, -10, 8);
    main_ready_label = make_label(screen, "Ready", 12, 205, 80, 20, &style_accent);
    main_pause_button = make_button(screen, "PAUSE", 10, 190, 145, 40, pause_resume_job);
    main_pause_label = lv_obj_get_child(main_pause_button, NULL);
    main_stop_button = make_button(screen, "STOP", 165, 190, 145, 40, stop_job);
    set_hidden_if_changed(main_pause_button, true);
    set_hidden_if_changed(main_stop_button, true);

    load_screen(screen);
    refresh_status();
}

static void create_control_screen(void)
{
    lv_obj_t *screen = make_screen("CONTROL / JOG");
    lv_obj_t *dro = lv_cont_create(screen, NULL);
    lv_obj_t *card = lv_cont_create(screen, NULL);
    char step_text[20], feed_text[24];
    snprintf(step_text, sizeof(step_text), "STEP %.1f mm", jog_steps[jog_step_index]);
    snprintf(feed_text, sizeof(feed_text), "FEED %d", jog_feeds[jog_feed_index]);

    lv_obj_set_style(dro, &style_panel);
    lv_obj_set_size(dro, 304, 34);
    lv_obj_set_pos(dro, 8, 36);
    make_label(dro, "M", 7, 1, 18, 15, &style_accent);
    position_label = make_label(dro, "X0.00  Y0.00  Z0.00", 27, 1, 267, 15, &style_dro);
    make_label(dro, "W", 7, 17, 18, 15, &style_accent);
    work_position_label = make_label(dro, "X0.00  Y0.00  Z0.00", 27, 17, 267, 15, &style_dro);

    lv_obj_set_style(card, &style_panel);
    lv_obj_set_size(card, 196, 163);
    lv_obj_set_pos(card, 8, 73);
    make_button(card, "Y+", 75, 2, 50, 50, jog_y_plus);
    make_button(card, "Z+", 130, 2, 50, 50, jog_z_plus);
    make_button(card, "X-", 20, 56, 50, 50, jog_x_minus);
    make_button(card, "X+", 130, 56, 50, 50, jog_x_plus);
    make_button(card, "Y-", 75, 110, 50, 50, jog_y_minus);
    make_button(card, "Z-", 130, 110, 50, 50, jog_z_minus);
    lv_obj_t *home_button = make_button(card, "H", 77, 58, 46, 46, home_machine);
    lv_btn_set_style(home_button, LV_BTN_STYLE_REL, &style_home_button);
    lv_btn_set_style(home_button, LV_BTN_STYLE_PR, &style_home_button_pressed);
    lv_btn_set_style(home_button, LV_BTN_STYLE_TGL_REL, &style_home_button);
    lv_btn_set_style(home_button, LV_BTN_STYLE_TGL_PR, &style_home_button_pressed);
    control_buttons[0] = make_small_button(screen, "REBOOT", 212, 72, 100, 27, reboot_controller);
    control_buttons[1] = jog_step_button = make_small_button(screen, step_text, 212, 100, 100, 27, select_jog_step);
    control_buttons[2] = jog_feed_button = make_small_button(screen, feed_text, 212, 128, 100, 27, select_jog_feed);
    control_buttons[3] = make_small_button(screen, "X0Y0", 212, 156, 100, 27, zero_xy);
    control_buttons[4] = make_small_button(screen, "Z0", 212, 184, 100, 27, zero_z);
    alarm_lock_button = control_buttons[5] = make_small_button(screen, "UNLOCK", 212, 212, 100, 27, unlock_alarm);
    alarm_lock_label = lv_obj_get_child(alarm_lock_button, NULL);
    lv_label_set_style(alarm_lock_label, LV_LABEL_STYLE_MAIN, &style_label_alarm);
    set_hidden_if_changed(alarm_lock_button, true);
    make_button(screen, "BACK", 238, 7, 72, 26, show_home);
    load_screen(screen);
    refresh_status();
}

static void create_jobs_screen(void)
{
    lv_obj_t *screen = make_screen("JOB CONTROL");
    state_label = make_label(screen, "IDLE", 16, 46, 120, 26, &style_accent);
    make_label(screen, selected_filename[0] ? selected_filename : "No local file selected",
               16, 82, 286, 40, &style_label);
    make_label(screen, "HOLD pauses the active stream. RESUME continues it.",
               16, 132, 286, 26, &style_label);
    make_button(screen, "START", 16, 176, 68, 44, start_selected_file);
    make_button(screen, "HOLD", 90, 176, 68, 44, feed_hold);
    make_button(screen, "RESUME", 164, 176, 68, 44, cycle_start);
    make_button(screen, "STOP", 238, 176, 66, 44, stop_job);
    make_button(screen, "BACK", 238, 7, 72, 26, show_home);
    load_screen(screen);
}

static void create_calibration_info_screen(void)
{
    lv_obj_t *screen = make_screen("GRBLHAL SETUP");
    mode_status_label = make_label(screen, "Current mode: CNC", 16, 44, 288, 22, &style_accent);
    lv_obj_t *mode_button = make_small_button(screen, "SWITCH TO LASER", 16, 72, 288, 42, toggle_machine_mode);
    mode_toggle_label = lv_obj_get_child(mode_button, NULL);
    make_label(screen, "This changes grblHAL $32 and saves the mode in controller EEPROM. Change mode while idle.",
               16, 122, 288, 38, &style_label);
    make_label(screen, "Machine calibration (steps/mm, travel, homing and limits) remains in grblHAL setup.",
               16, 166, 288, 40, &style_label);
    make_button(screen, "BACK", 238, 7, 72, 26, show_home);
    load_screen(screen);
    refresh_status();
}

static void show_home(lv_obj_t *obj, lv_event_t event)
{
    (void)obj;
    if(event == LV_EVENT_CLICKED) create_home_screen();
}

static void show_control(lv_obj_t *obj, lv_event_t event)
{
    (void)obj;
    if(event == LV_EVENT_CLICKED) create_control_screen();
}

static void show_files(lv_obj_t *obj, lv_event_t event)
{
    (void)obj;
    if(event == LV_EVENT_CLICKED) create_file_screen();
}

static void show_calibration_info(lv_obj_t *obj, lv_event_t event)
{
    (void)obj;
    if(event == LV_EVENT_CLICKED) create_calibration_info_screen();
}

static void lvgl_tick(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

static void ui_task(void *arg)
{
    lv_disp_drv_t display_driver;
    lv_indev_drv_t input_driver;
    const esp_timer_create_args_t timer_args = {
        .callback = lvgl_tick,
        .name = "ts24_lv_tick"
    };
    esp_timer_handle_t timer;
    TickType_t last_refresh = 0;

    (void)arg;
    if(display_bus_init() != ESP_OK) {
        ESP_LOGE(TAG, "Could not initialize TS24 SPI/display hardware");
        vTaskDelete(NULL);
        return;
    }

    lv_init();
    init_styles();
    lv_disp_buf_init(&display_buffer, display_pixels, NULL,
                     TS24_WIDTH * TS24_BUFFER_LINES);
    lv_disp_drv_init(&display_driver);
    display_driver.hor_res = TS24_WIDTH;
    display_driver.ver_res = TS24_HEIGHT;
    display_driver.flush_cb = display_flush;
    display_driver.buffer = &display_buffer;
    lv_disp_drv_register(&display_driver);
    lv_theme_set_current(lv_theme_night_init(210, NULL));

    lv_indev_drv_init(&input_driver);
    input_driver.type = LV_INDEV_TYPE_POINTER;
    input_driver.read_cb = touch_read;
    lv_indev_drv_register(&input_driver);

    create_home_screen();

    if(esp_timer_create(&timer_args, &timer) == ESP_OK)
        esp_timer_start_periodic(timer, 1000);

    gpio_set_level(TS24_PIN_BACKLIGHT, 0); /* MKS TS35 source uses active-low backlight */
    ui_started = true;
    ESP_LOGI(TAG, "TS24 UI started at %d x %d", TS24_WIDTH, TS24_HEIGHT);

    for(;;) {
        lv_task_handler();
        if((xTaskGetTickCount() - last_refresh) >= pdMS_TO_TICKS(250)) {
            refresh_status();
            last_refresh = xTaskGetTickCount();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void mks_ts24_ui_init(void)
{
    if(ui_started)
        return;
    if(xTaskCreatePinnedToCore(ui_task, "ts24_ui", 8192, NULL,
                               tskIDLE_PRIORITY + 1, NULL, 0) != pdPASS)
        ESP_LOGE(TAG, "Could not create TS24 UI task");
}
