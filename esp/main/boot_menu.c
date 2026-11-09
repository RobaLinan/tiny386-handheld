#ifdef BOOT_MENU

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"

#include "common.h"

extern const uint8_t vgafont16[256 * 16];

/* 0 = PAL (PAL!), 1 = FD2, 2 = DOS prompt. Read by pc_main(). */
int boot_choice = 2;

static const char *TAG = "bootmenu";
static esp_lcd_touch_handle_t tp;
static uint16_t *menu_fb;
static esp_lcd_panel_handle_t menu_panel;

#define MENU_W LCD_WIDTH    /* 800, logical x */
#define MENU_H LCD_HEIGHT   /* 480, logical y */

/* The panel scans portrait: logical (x, y) lives at x * MENU_H + y. */
static inline void put_px(int x, int y, uint16_t c)
{
	if (x >= 0 && x < MENU_W && y >= 0 && y < MENU_H)
		menu_fb[x * MENU_H + y] = c;
}

static void fill_rect(int x0, int y0, int x1, int y1, uint16_t c)
{
	for (int x = x0; x < x1; x++)
		for (int y = y0; y < y1; y++)
			put_px(x, y, c);
}

static void draw_text(int x, int y, const char *s, int scale, uint16_t c)
{
	for (; *s; s++) {
		const uint8_t *glyph = vgafont16 + (uint8_t) *s * 16;
		for (int row = 0; row < 16; row++) {
			uint8_t bits = glyph[row];
			for (int col = 0; col < 8; col++)
				if (bits & (0x80 >> col))
					fill_rect(x + col * scale,
						  y + row * scale,
						  x + (col + 1) * scale,
						  y + (row + 1) * scale, c);
		}
		x += 8 * scale;
	}
}

struct button {
	int x0, y0, x1, y1;
	const char *title;
	const char *sub;
	uint16_t bg;
};

static struct button buttons[3] = {
	{  30, 240, 250, 380, "PAL", "Xian Jian", 0x2945 },
	{ 290, 240, 510, 380, "FD2", "Yan Long 2", 0x2945 },
	{ 550, 240, 770, 380, "DOS", "Command",   0x2945 },
};

static void draw_button(struct button *b, int pressed)
{
	uint16_t bg = pressed ? 0x0600 : b->bg;
	uint16_t border = pressed ? 0x07E0 : 0x7BEF;
	int tw = (int) strlen(b->title) * 8 * 4;
	int sw = (int) strlen(b->sub) * 8 * 2;

	fill_rect(b->x0, b->y0, b->x1, b->y1, bg);
	fill_rect(b->x0, b->y0, b->x1, b->y0 + 3, border);
	fill_rect(b->x0, b->y1 - 3, b->x1, b->y1, border);
	fill_rect(b->x0, b->y0, b->x0 + 3, b->y1, border);
	fill_rect(b->x1 - 3, b->y0, b->x1, b->y1, border);
	draw_text(b->x0 + (b->x1 - b->x0 - tw) / 2, b->y0 + 24,
		  b->title, 4, 0xFFFF);
	draw_text(b->x0 + (b->x1 - b->x0 - sw) / 2, b->y0 + 96,
		  b->sub, 2, 0xD69A);
}

static void draw_menu(int pressed)
{
	fill_rect(0, 0, MENU_W, MENU_H, 0x0841);
	draw_text((MENU_W - 11 * 8 * 4) / 2, 40, "TINY386 DOS", 4, 0xFFFF);
	draw_text((MENU_W - 14 * 8 * 2) / 2, 130, "touch to boot a game", 2,
		  0xA514);
	for (int i = 0; i < 3; i++)
		draw_button(&buttons[i], i == pressed);
	esp_lcd_panel_draw_bitmap(menu_panel, 0, 0, MENU_H, MENU_W, menu_fb);
}

static esp_err_t touch_init(void)
{
	i2c_master_bus_handle_t bus = NULL;
	esp_err_t err = i2c_master_get_bus_handle(ES8311_I2C_NUM, &bus);
	if (err != ESP_OK || !bus) {
		ESP_LOGE(TAG, "no I2C bus for touch: %s", esp_err_to_name(err));
		return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
	}
	esp_lcd_panel_io_handle_t io = NULL;
	esp_lcd_panel_io_i2c_config_t io_cfg =
		ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
	io_cfg.scl_speed_hz = 100000;
	err = esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io);
	if (err != ESP_OK)
		return err;

	esp_lcd_touch_config_t cfg = {
		.x_max = MENU_H,
		.y_max = MENU_W,
		.rst_gpio_num = -1,
		.int_gpio_num = -1,
		.levels = { .reset = 0, .interrupt = 0 },
		.flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
	};
	return esp_lcd_touch_new_i2c_gt911(io, &cfg, &tp);
}

/* Raw GT911 coordinates are portrait (0..479, 0..799); the panel is
 * mirrored so the menu's logical y runs opposite to the raw x. */
static void touch_to_menu(uint16_t rx, uint16_t ry, int *x, int *y)
{
	*x = ry;
	*y = MENU_H - 1 - rx;
}

int boot_menu(esp_lcd_panel_handle_t panel)
{
	menu_panel = panel;
	if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, (void **) &menu_fb)
	    != ESP_OK || !menu_fb)
		return 2;
	if (touch_init() != ESP_OK) {
		ESP_LOGE(TAG, "GT911 init failed, booting DOS");
		return 2;
	}

	ESP_LOGI(TAG, "menu ready");
	draw_menu(-1);
	vTaskDelay(pdMS_TO_TICKS(100));

	for (;;) {
		uint16_t rx = 0, ry = 0, strength[1];
		uint8_t cnt = 0;
		esp_lcd_touch_read_data(tp);
		if (esp_lcd_touch_get_coordinates(tp, &rx, &ry, strength, &cnt,
						  1) && cnt) {
			int x, y;
			touch_to_menu(rx, ry, &x, &y);
			fprintf(stderr, "menu: touch %d,%d -> %d,%d\n",
				rx, ry, x, y);
			for (int i = 0; i < 3; i++) {
				if (x >= buttons[i].x0 && x < buttons[i].x1 &&
				    y >= buttons[i].y0 && y < buttons[i].y1) {
					draw_button(&buttons[i], 1);
					esp_lcd_panel_draw_bitmap(menu_panel,
								  0, 0, MENU_H,
								  MENU_W, menu_fb);
					vTaskDelay(pdMS_TO_TICKS(150));
					return i;
				}
			}
		}
		vTaskDelay(pdMS_TO_TICKS(20));
	}
}

#endif
