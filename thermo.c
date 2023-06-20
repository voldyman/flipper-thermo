
#include <gui/gui.h>
#include <gui/view_port.h>

#include <core/thread.h>
#include <core/kernel.h>

#include <locale/locale.h>

#include <one_wire/maxim_crc.h>

#include <furi_hal_power.h>

#define UPDATE_PERIOD_MS 1000UL
#define TEXT_STORE_SIZE 64U

#define TAG "VoldyThermo"

/* Possible GPIO pin choices:
 - gpio_ext_pc0
 - gpio_ext_pc1
 - gpio_ext_pc3
 - gpio_ext_pb2
 - gpio_ext_pb3
 - gpio_ext_pa4
 - gpio_ext_pa6
 - gpio_ext_pa7
 - gpio_ibutton
*/
#define THERMO_GPIO_PIN (gpio_ibutton)

typedef union {
    struct {
        uint8_t hum_msb;
        uint8_t hum_lsb;
        uint8_t temp_msb;
        uint8_t temp_lsb;
        uint8_t crc; /* CRC checksum for error detection */
    } fields;
    uint8_t bytes[5];
} AM2301ScratchPad;

/* Flags which the reader thread responds to */
typedef enum {
    ReaderThreadFlagExit = 1,
} ReaderThreadFlag;

/* Application context structure */
typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* event_queue;
    FuriThread* reader_thread;
    float temp_celsius;
    float humidity;
    bool has_device;
} ThermoContext;

static void thermo_draw_callback(Canvas* canvas, void* ctx) {
    ThermoContext* context = ctx;

    char text_store[TEXT_STORE_SIZE];
    const size_t middle_x = canvas_width(canvas) / 2U;
    const uint8_t title_bottom = 16;

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, middle_x, 12, AlignCenter, AlignBottom, "Thermometer");
    canvas_draw_line(canvas, 0, title_bottom, canvas_width(canvas), title_bottom);

    canvas_draw_rframe(canvas, 0, 0, canvas_width(canvas), canvas_height(canvas) - 1, 7);
    canvas_draw_rframe(canvas, 0, 0, canvas_width(canvas), canvas_height(canvas), 7);

    if(!context->has_device) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(
            canvas, middle_x, 30, AlignCenter, AlignBottom, "Connnect thermometer");

        snprintf(
            text_store,
            TEXT_STORE_SIZE,
            "to GPIO pin %ld",
            furi_hal_resources_get_ext_pin_number(&THERMO_GPIO_PIN));
        canvas_draw_str_aligned(canvas, middle_x, 42, AlignCenter, AlignBottom, text_store);
        strncpy(text_store, "-- No data --", TEXT_STORE_SIZE);
        canvas_draw_str_aligned(
            canvas, middle_x, title_bottom + 2, AlignCenter, AlignBottom, text_store);
        return;
    }

    // if(context->has_device)
    canvas_set_font(canvas, FontKeyboard);
    float temp;
    char temp_units;

    // The applicaton is locale-aware.
    // Change Settings->System->Units to check it out.
    switch(locale_get_measurement_unit()) {
    case LocaleMeasurementUnitsMetric:
        temp = context->temp_celsius;
        temp_units = 'C';
        break;
    case LocaleMeasurementUnitsImperial:
        temp = locale_celsius_to_fahrenheit(context->temp_celsius);
        temp_units = 'F';
        break;
    default:
        furi_crash("Illegal measurement units");
    }
    // If a reading is available, display it
    snprintf(text_store, TEXT_STORE_SIZE, "%.1f%c", (double)temp, temp_units);

    uint8_t frame_width = 54;
    uint8_t frame_height = 20;

    uint8_t vert_pad = (canvas_height(canvas) - title_bottom - (2 * frame_height)) / 3;
    uint8_t temp_y = title_bottom + vert_pad;

    // Draw Temperature
    canvas_draw_rframe(canvas, middle_x - (frame_width / 2), temp_y, frame_width, 20, 3);

    canvas_draw_str_aligned(
        canvas, middle_x, temp_y + (frame_height / 2) + 2, AlignCenter, AlignBottom, text_store);

    // Draw Humidity
    snprintf(text_store, TEXT_STORE_SIZE, "%.1f%%", (double)context->humidity);
    uint8_t hum_y = temp_y + (frame_height) + vert_pad;
    canvas_draw_rframe(canvas, middle_x - (frame_width / 2), hum_y, frame_width, 20, 3);

    canvas_draw_str_aligned(
        canvas, middle_x, hum_y + (frame_height / 2) + 2, AlignCenter, AlignBottom, text_store);
}

static void thermo_input_callback(InputEvent* event, void* ctx) {
    ThermoContext* context = ctx;
    furi_message_queue_put(context->event_queue, event, FuriWaitForever);
}

// REF: https://www.haoyuelectronics.com/Attachment/AM2301/AM2301.pdf
static int32_t thermo_reader_thread_callback(void* ctx) {
    ThermoContext* context = ctx;
    UNUSED(context);
    furi_hal_gpio_write(&THERMO_GPIO_PIN, true);
    furi_hal_gpio_init(&THERMO_GPIO_PIN, GpioModeOutputOpenDrain, GpioPullUp, GpioSpeedVeryHigh);

    for(;;) {
        const uint32_t flags =
            furi_thread_flags_wait(ReaderThreadFlagExit, FuriFlagWaitAny, UPDATE_PERIOD_MS);

        if(flags != (unsigned)FuriFlagErrorTimeout) {
            // exit from the thread when exit signal is received
            break;
        }

        // Request
        furi_hal_gpio_write(&THERMO_GPIO_PIN, false);
        furi_delay_ms(19);

        // line up
        furi_hal_gpio_write(&THERMO_GPIO_PIN, true);
        uint16_t retries = 500;

        do {
            if(--retries == 0) break;
        } while(!furi_hal_gpio_read(&THERMO_GPIO_PIN));

        retries = 500;
        do {
            if(--retries == 0) break;
        } while(furi_hal_gpio_read(&THERMO_GPIO_PIN));

        retries = 500;
        do {
            if(--retries == 0) break;
        } while(!furi_hal_gpio_read(&THERMO_GPIO_PIN));

        retries = 500;
        do {
            if(--retries == 0) break;
        } while(furi_hal_gpio_read(&THERMO_GPIO_PIN));

        context->has_device = true;

        // we need to read 5 bytes from the port
        AM2301ScratchPad buf = {0};
        for(uint8_t a = 0; a < sizeof(buf.bytes); a++) {
            for(uint8_t b = 7; b != 255; b--) {
                uint16_t hT = 0, lT = 0;
                // read the low signal
                while(!furi_hal_gpio_read(&THERMO_GPIO_PIN) && lT != 65535) lT++;
                // read the high signal
                while(furi_hal_gpio_read(&THERMO_GPIO_PIN) && hT != 65535) hT++;
                // high signal was longer than low, then bit is 1
                if(hT > lT) {
                    buf.bytes[a] |= (1 << b);
                }
            }
        }

        // verify checksum
        if((uint8_t)(buf.fields.hum_msb + buf.fields.hum_lsb + buf.fields.temp_msb +
                     buf.fields.temp_lsb) != buf.fields.crc) {
            furi_log_print_format(FuriLogLevelDebug, TAG, "fields checksum match failure");
            continue;
        }

        uint16_t temp = ((uint16_t)buf.fields.temp_msb << 8) | buf.fields.temp_lsb;
        uint16_t hum = ((uint16_t)buf.fields.hum_msb << 8) | buf.fields.hum_lsb;

        context->humidity = (float)(hum / 10);
        if(READ_BIT(temp, 1 << 15)) {
            CLEAR_BIT(temp, 1 << 15);
            context->temp_celsius = (float)(temp / -10);
        } else {
            context->temp_celsius = (float)(temp / 10);
        }
        furi_log_print_format(
            FuriLogLevelTrace,
            TAG,
            "Temp: %f & Humidity: %f",
            (double)context->temp_celsius,
            (double)context->humidity);

        furi_delay_us(10000);
    }
    // cleanup before thread exit
    context->has_device = false;
    furi_hal_gpio_write(&THERMO_GPIO_PIN, false);
    furi_hal_gpio_init(&THERMO_GPIO_PIN, GpioModeAnalog, GpioPullNo, GpioSpeedLow);

    return 0;
}

static ThermoContext* thermo_context_alloc() {
    ThermoContext* context = malloc(sizeof(ThermoContext));
    furi_log_print_format(FuriLogLevelTrace, TAG, "allocated context");

    context->view_port = view_port_alloc();
    view_port_draw_callback_set(context->view_port, thermo_draw_callback, context);
    view_port_input_callback_set(context->view_port, thermo_input_callback, context);
    furi_log_print_format(FuriLogLevelTrace, TAG, "setup view port");

    context->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    furi_log_print_format(FuriLogLevelTrace, TAG, "allocated event queue");

    context->reader_thread = furi_thread_alloc();
    furi_thread_set_stack_size(context->reader_thread, 1024U);
    furi_thread_set_context(context->reader_thread, context);
    furi_thread_set_callback(context->reader_thread, thermo_reader_thread_callback);
    furi_log_print_format(FuriLogLevelTrace, TAG, "setup reader thread");

    context->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(context->gui, context->view_port, GuiLayerFullscreen);

    return context;
}

static void thermo_context_free(ThermoContext* context) {
    view_port_enabled_set(context->view_port, false);
    gui_remove_view_port(context->gui, context->view_port);

    furi_thread_free(context->reader_thread);
    furi_message_queue_free(context->event_queue);
    view_port_free(context->view_port);

    furi_record_close(RECORD_GUI);
}

static void thermo_run(ThermoContext* context) {
    furi_hal_power_enable_otg();

    furi_thread_start(context->reader_thread);

    for(bool is_running = true; is_running;) {
        InputEvent event;
        const FuriStatus status =
            furi_message_queue_get(context->event_queue, &event, FuriWaitForever);
        if((status != FuriStatusOk) || (event.type != InputTypeShort)) {
            continue;
        }
        if(event.key == InputKeyBack) {
            is_running = false;
        }
    }
    furi_thread_flags_set(furi_thread_get_id(context->reader_thread), ReaderThreadFlagExit);
    furi_thread_join(context->reader_thread);
    furi_hal_power_disable_otg();
}

int32_t thermo_main(void* p) {
    UNUSED(p);
    furi_log_print_format(FuriLogLevelTrace, TAG, "Starting main");

    ThermoContext* context = thermo_context_alloc();
    furi_log_print_format(FuriLogLevelTrace, TAG, "context allocated, running main loop");

    thermo_run(context);

    furi_log_print_format(FuriLogLevelTrace, TAG, "freeing before exit");
    thermo_context_free(context);

    furi_log_print_format(FuriLogLevelTrace, TAG, "el fin");
    return 0;
}
