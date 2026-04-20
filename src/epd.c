

#include "epd.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(epd, LOG_LEVEL_INF);

#define EPD_NODE DT_NODELABEL(waveshare_epd_dev)

static const struct gpio_dt_spec edp_res_pin = GPIO_DT_SPEC_GET(EPD_NODE, res_gpios);
static const struct gpio_dt_spec edp_cs_pin = GPIO_DT_SPEC_GET(EPD_NODE, cs_gpios);
static const struct gpio_dt_spec edp_dc_pin = GPIO_DT_SPEC_GET(EPD_NODE, dc_gpios);
static const struct gpio_dt_spec edp_busy_pin = GPIO_DT_SPEC_GET(EPD_NODE, busy_gpios);

static const struct spi_dt_spec spi_spec =
    SPI_DT_SPEC_GET(EPD_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER, 0);

static bool is_gpio_initialized = false;
static bool is_in_sleep = false;

static const unsigned char EPD_LUT_VCOM0[] = {
    0x00, 0x08, 0x08, 0x00, 0x00, 0x02, 0x00, 0x0F, 0x0F, 0x00, 0x00, 0x01, 0x00, 0x08, 0x08,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const unsigned char EPD_LUT_WW[] = {
    0x50, 0x08, 0x08, 0x00, 0x00, 0x02, 0x90, 0x0F, 0x0F, 0x00, 0x00, 0x01, 0xA0, 0x08,
    0x08, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const unsigned char EPD_LUT_BW[] = {
    0x50, 0x08, 0x08, 0x00, 0x00, 0x02, 0x90, 0x0F, 0x0F, 0x00, 0x00, 0x01, 0xA0, 0x08,
    0x08, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const unsigned char EPD_LUT_WB[] = {
    0xA0, 0x08, 0x08, 0x00, 0x00, 0x02, 0x90, 0x0F, 0x0F, 0x00, 0x00, 0x01, 0x50, 0x08,
    0x08, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const unsigned char EPD_LUT_BB[] = {
    0x20, 0x08, 0x08, 0x00, 0x00, 0x02, 0x90, 0x0F, 0x0F, 0x00, 0x00, 0x01, 0x10, 0x08,
    0x08, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void epd_spi_write_byte(uint8_t data)
{
    struct spi_buf tx_buf = {.buf = &data, .len = 1};
    struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};
    int err = spi_write_dt(&spi_spec, &tx);
    if (err) {
        LOG_ERR("spi_write_dt() failed, (error %d)", err);
    }
}

static void epd_send_command(uint8_t cmd)
{
    gpio_pin_set_dt(&edp_dc_pin, 0);
    gpio_pin_set_dt(&edp_cs_pin, 1);
    epd_spi_write_byte(cmd);
    gpio_pin_set_dt(&edp_cs_pin, 0);
}

static void epd_send_data(uint8_t data)
{
    gpio_pin_set_dt(&edp_dc_pin, 1);
    gpio_pin_set_dt(&edp_cs_pin, 1);
    epd_spi_write_byte(data);
    gpio_pin_set_dt(&edp_cs_pin, 0);
}

static void epd_read_busy(void)
{
    while (gpio_pin_get_dt(&edp_busy_pin)) {  // LOW: idle, HIGH: busy
        k_msleep(10);
    }
}

static void epd_turn_on_display(void)
{
    epd_send_command(0x12);
    k_msleep(100);
    epd_read_busy();
}

static void epd_setlut(void)
{
    uint8_t count;
    epd_send_command(0x20);
    for (count = 0; count < 36; count++) {
        epd_send_data(EPD_LUT_VCOM0[count]);
    }

    epd_send_command(0x21);
    for (count = 0; count < 36; count++) {
        epd_send_data(EPD_LUT_WW[count]);
    }

    epd_send_command(0x22);
    for (count = 0; count < 36; count++) {
        epd_send_data(EPD_LUT_BW[count]);
    }

    epd_send_command(0x23);
    for (count = 0; count < 36; count++) {
        epd_send_data(EPD_LUT_WB[count]);
    }

    epd_send_command(0x24);
    for (count = 0; count < 36; count++) {
        epd_send_data(EPD_LUT_BB[count]);
    }
}

static void epd_reset(void)
{
    gpio_pin_set_dt(&edp_res_pin, 1);
    k_msleep(10);
    gpio_pin_set_dt(&edp_res_pin, 0);
    k_msleep(10);

    gpio_pin_set_dt(&edp_res_pin, 1);
    k_msleep(10);
    gpio_pin_set_dt(&edp_res_pin, 0);
    k_msleep(10);

    gpio_pin_set_dt(&edp_res_pin, 1);
    k_msleep(10);
    gpio_pin_set_dt(&edp_res_pin, 0);
    k_msleep(10);
}

void epd_clear(void)
{
    uint32_t width, height;
    width = (EPD_WIDTH % 8 == 0) ? (EPD_WIDTH / 8) : (EPD_WIDTH / 8 + 1);
    height = EPD_HEIGHT;

    epd_send_command(0x10);
    for (uint32_t j = 0; j < height; j++) {
        for (uint32_t i = 0; i < width; i++) {
            epd_send_data(0xFF);
        }
    }

    epd_send_command(0x13);
    for (uint32_t j = 0; j < height; j++) {
        for (uint32_t i = 0; i < width; i++) {
            epd_send_data(0xFF);
        }
    }

    epd_send_command(0x12);  // DISPLAY REFRESH
    k_msleep(100);
    epd_turn_on_display();
}

void epd_display(uint8_t* buffer)
{
    uint32_t width, height;
    width = (EPD_WIDTH % 8 == 0) ? (EPD_WIDTH / 8) : (EPD_WIDTH / 8 + 1);
    height = EPD_HEIGHT;

    epd_send_command(0x10);
    for (uint32_t j = 0; j < height; j++) {
        for (uint32_t i = 0; i < width; i++) {
            epd_send_data(0x00);
        }
    }

    epd_send_command(0x13);
    for (uint32_t j = 0; j < height; j++) {
        for (uint32_t i = 0; i < width; i++) {
            epd_send_data(buffer[i + j * width]);
        }
    }

    epd_send_command(0x12);  // DISPLAY REFRESH
    k_msleep(10);
    epd_turn_on_display();
}

static int epd_gpio_init(void)
{
    int err;

    if (!spi_is_ready_dt(&spi_spec)) {
        LOG_ERR("SPI bus is not ready");
        return 0;
    }

    if (!gpio_is_ready_dt(&edp_cs_pin)) {
        LOG_ERR("CS gpio device not ready");
        return -EIO;
    }

    if (!gpio_is_ready_dt(&edp_dc_pin)) {
        LOG_ERR("DC gpio device not ready");
        return -EIO;
    }

    if (!gpio_is_ready_dt(&edp_res_pin)) {
        LOG_ERR("RES gpio device not ready");
        return -EIO;
    }

    if (!gpio_is_ready_dt(&edp_busy_pin)) {
        LOG_ERR("BUSY gpio device not ready");
        return -EIO;
    }

    err = gpio_pin_configure_dt(&edp_cs_pin, GPIO_OUTPUT_INACTIVE);
    if (err) {
        LOG_ERR("Failed to configure CS pin (error %d)", err);
        return err;
    }

    err = gpio_pin_configure_dt(&edp_res_pin, GPIO_OUTPUT_INACTIVE);
    if (err) {
        LOG_ERR("Failed to configure RES pin (error %d)", err);
        return err;
    }

    err = gpio_pin_configure_dt(&edp_dc_pin, GPIO_OUTPUT_ACTIVE);
    if (err) {
        LOG_ERR("Failed to configure DC pin (error %d)", err);
        return err;
    }

    err = gpio_pin_configure_dt(&edp_busy_pin, GPIO_INPUT | GPIO_PULL_UP);
    if (err) {
        LOG_ERR("Failed to configure BUSY pin (error %d)", err);
        return err;
    }

    return 0;
}

int epd_init(void)
{
    int err;

    if (!is_gpio_initialized) {
        err = epd_gpio_init();
        if (err) {
            return err;
        }
        is_gpio_initialized = true;
    } else if (!is_in_sleep) {
        return 0;
    }

    epd_reset();
    epd_send_command(0x01);  // POWER SETTING
    epd_send_data(0x03);
    epd_send_data(0x00);
    epd_send_data(0x2b);
    epd_send_data(0x2b);

    epd_send_command(0x06);  // boost soft start
    epd_send_data(0x17);     // A
    epd_send_data(0x17);     // B
    epd_send_data(0x17);     // C

    epd_send_command(0x04);
    epd_read_busy();

    epd_send_command(0x00);  // panel setting
    epd_send_data(0xbf);     // KW-bf   KWR-2F	BWROTP 0f	BWOTP 1f

    epd_send_command(0x30);
    epd_send_data(0x3c);  // 3A 100HZ   29 150Hz 39 200HZ	31 171HZ

    epd_send_command(0x61);  // resolution setting
    epd_send_data(0x01);
    epd_send_data(0x90);  // 400
    epd_send_data(0x01);  // 300
    epd_send_data(0x2c);

    epd_send_command(0x82);  // vcom_DC setting
    epd_send_data(0x12);

    epd_send_command(0X50);
    epd_send_data(0x97);

    epd_setlut();

    is_in_sleep = false;

    return 0;
}

void epd_sleep(void)
{
    epd_send_command(0x50);  // DEEP_SLEEP
    epd_send_data(0XF7);

    epd_send_command(0x02);  // POWER_OFF
    epd_read_busy();

    epd_send_command(0x07);  // DEEP_SLEEP
    epd_send_data(0XA5);

    is_in_sleep = true;
}

// ========================================= Drawing functions =========================================

void epd_fill(uint8_t* buffer, epd_color_t color)
{
    if (color == EPD_COLOR_BLACK)
        memset(buffer, 0x00, (EPD_WIDTH * EPD_HEIGHT) / 8);
    else
        memset(buffer, 0xFF, (EPD_WIDTH * EPD_HEIGHT) / 8);
}

void epd_draw_pixel(uint8_t* buffer, int16_t x, int16_t y, epd_color_t color)
{
    uint16_t byte_offset;
    uint8_t mask;

    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) {
        return;
    }
    byte_offset = (x + y * EPD_WIDTH) / 8;
    mask = 0x80 >> (x % 8);
    if (color == EPD_COLOR_BLACK) {
        buffer[byte_offset] &= ~mask;
    } else {
        buffer[byte_offset] |= mask;
    }
}

void epd_draw_line(uint8_t* buffer, int16_t from_x, int16_t from_y, int16_t to_x, int16_t to_y, epd_color_t color)
{
    int16_t x = from_x;
    int16_t y = from_y;
    int16_t dx = to_x - from_x >= 0 ? to_x - from_x : from_x - to_x;
    int16_t dy = to_y - from_y <= 0 ? to_y - from_y : from_y - to_y;

    int16_t x_add = from_x < to_x ? 1 : -1;
    int16_t y_add = from_y < to_y ? 1 : -1;
    int16_t eps = dx + dy;

    for (;;) {
        epd_draw_pixel(buffer, x, y, color);
        if (2 * eps >= dy) {
            if (x == to_x)
                break;
            eps += dy;
            x += x_add;
        }
        if (2 * eps <= dx) {
            if (y == to_y)
                break;
            eps += dx;
            y += y_add;
        }
    }
}

void epd_draw_rectangle(uint8_t* buffer, int16_t from_x, int16_t from_y, int16_t to_x, int16_t to_y, epd_color_t color,
                        bool fill)
{
    int16_t y;

    if (fill) {
        for (y = from_y; y < to_y; y++) {
            epd_draw_line(buffer, from_x, y, to_x, y, color);
        }
    } else {
        epd_draw_line(buffer, from_x, from_y, to_x, from_y, color);
        epd_draw_line(buffer, from_x, from_y, from_x, to_y, color);
        epd_draw_line(buffer, to_x, to_y, to_x, from_y, color);
        epd_draw_line(buffer, to_x, to_y, from_x, to_y, color);
    }
}

void epd_draw_circle(uint8_t* buffer, int16_t x, int16_t y, int16_t radius, epd_color_t color, bool fill)
{
    int16_t curr_x, curr_y;
    curr_x = 0;
    curr_y = radius;
    int16_t eps = 3 - (radius << 1);
    int16_t cont_y;

    if (fill) {
        while (curr_x <= curr_y) {  // Realistic circles
            for (cont_y = curr_x; cont_y <= curr_y; cont_y++) {
                epd_draw_pixel(buffer, x + curr_x, y + cont_y, color);  // 1
                epd_draw_pixel(buffer, x - curr_x, y + cont_y, color);  // 2
                epd_draw_pixel(buffer, x - cont_y, y + curr_x, color);  // 3
                epd_draw_pixel(buffer, x - cont_y, y - curr_x, color);  // 4
                epd_draw_pixel(buffer, x - curr_x, y - cont_y, color);  // 5
                epd_draw_pixel(buffer, x + curr_x, y - cont_y, color);  // 6
                epd_draw_pixel(buffer, x + cont_y, y - curr_x, color);  // 7
                epd_draw_pixel(buffer, x + cont_y, y + curr_x, color);
            }
            if (eps < 0)
                eps += 4 * curr_x + 6;
            else {
                eps += 10 + 4 * (curr_x - curr_y);
                curr_y--;
            }
            curr_x++;
        }
    } else {  // Draw a hollow circle
        while (curr_x <= curr_y) {
            epd_draw_pixel(buffer, x + curr_x, y + curr_y, color);  // 1
            epd_draw_pixel(buffer, x - curr_x, y + curr_y, color);  // 2
            epd_draw_pixel(buffer, x - curr_y, y + curr_x, color);  // 3
            epd_draw_pixel(buffer, x - curr_y, y - curr_x, color);  // 4
            epd_draw_pixel(buffer, x - curr_x, y - curr_y, color);  // 5
            epd_draw_pixel(buffer, x + curr_x, y - curr_y, color);  // 6
            epd_draw_pixel(buffer, x + curr_y, y - curr_x, color);  // 7
            epd_draw_pixel(buffer, x + curr_y, y + curr_x, color);  // 0

            if (eps < 0)
                eps += 4 * curr_x + 6;
            else {
                eps += 10 + 4 * (curr_x - curr_y);
                curr_y--;
            }
            curr_x++;
        }
    }
}

void epd_draw_image(const uint8_t* image, uint8_t* buffer, uint16_t width, uint16_t height, int16_t x, int16_t y,
                    epd_color_t color, bool transparent)
{
    int16_t img_byte_x, img_y, pixel_x, byte_offset;
    uint8_t img_bytes_width = ((width - 1) / 8 + 1);
    uint8_t mask[2];
    uint8_t mask_bkg[2];
    uint8_t bkg;

    for (img_y = 0; img_y < height; img_y++) {
        for (img_byte_x = 0; img_byte_x < img_bytes_width; img_byte_x++) {
            pixel_x = x + img_byte_x * 8;
            byte_offset = (pixel_x + (y + img_y) * EPD_WIDTH) / 8;
            if (byte_offset >= EPD_FRAME_BUFFER_SIZE || byte_offset < 0) {
                continue;
            }
            *(uint16_t*)mask = image[img_byte_x + img_y * img_bytes_width] << (8 - (x % 8));
            if (!transparent) {
                bkg = (img_byte_x + 1) * 8 > width ? (0xFF << (8 - (width % 8))) : 0xFF;
                *(uint16_t*)mask_bkg = bkg << (8 - (x % 8));
            }
            if (pixel_x < EPD_WIDTH || pixel_x >= 0) {
                if (color == EPD_COLOR_BLACK) {
                    if (!transparent)
                        buffer[byte_offset] |= mask_bkg[1];
                    buffer[byte_offset] &= ~mask[1];
                } else {
                    if (!transparent)
                        buffer[byte_offset] &= ~mask_bkg[1];
                    buffer[byte_offset] |= mask[1];
                }
            }
            pixel_x += 8;
            byte_offset += 1;
            if (pixel_x < EPD_WIDTH || pixel_x >= 0) {
                if (color == EPD_COLOR_BLACK) {
                    if (!transparent)
                        buffer[byte_offset] |= mask_bkg[0];
                    buffer[byte_offset] &= ~mask[0];
                } else {
                    if (!transparent)
                        buffer[byte_offset] &= ~mask_bkg[0];
                    buffer[byte_offset] |= mask[0];
                }
            }
        }
    }
}

void epd_draw_icon(const sICON* icon, uint8_t* buffer, int16_t x, int16_t y, epd_color_t color, bool transparent)
{
    epd_draw_image(icon->data, buffer, icon->width, icon->height, x, y, color, transparent);
}

void epd_draw_char(const char ch, const sFONT* font, uint8_t* buffer, int16_t x, int16_t y, epd_color_t color,
                   bool transparent)
{
    int16_t char_offset;
    uint8_t font_bytes_width = ((font->width - 1) / 8 + 1);

    if (ch >= 32) {
        char_offset = (ch - 32) * font->height * font_bytes_width;
        epd_draw_image(&font->table[char_offset], buffer, font->width, font->height, x, y, color, transparent);
    }
}

void epd_draw_string(const char* str, const sFONT* font, uint8_t* buffer, int16_t x, int16_t y, epd_color_t color,
                     bool transparent)
{
    uint32_t i = 0;

    while (str[i] != '\0') {
        epd_draw_char(str[i], font, buffer, x, y, color, transparent);
        x += font->width;
        i++;
    }
}
