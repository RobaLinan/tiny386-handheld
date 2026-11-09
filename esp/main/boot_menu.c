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
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "common.h"

extern const uint8_t vgafont16[256 * 16];

/* Add a game here plus a 800x480 background and a 380x214 thumbnail on
 * the SD card (scripts/menu_bg.py writes both). `cmd` is typed once DOS
 * shows its prompt. */
struct game_entry {
	const char *title;
	const char *cmd;
	const char *bg;
	const char *thumb;
};

static const struct game_entry games[] = {
	{ "PAL", "D:\\PAL.BAT\r", "/sdcard/menu/pal.raw",
	  "/sdcard/menu/pal_thumb.raw" },
	{ "FD2", "D:\\FD2\r", "/sdcard/menu/fd2.raw",
	  "/sdcard/menu/fd2_thumb.raw" },
};

#define NGAMES ((int) (sizeof(games) / sizeof(games[0])))
#define MENU_COLS 3
#define THUMB_W 380
#define THUMB_H 214
#define CARD_H 250
#define TITLE_H 40

/* Selected entry; read by pc_main(). */
int boot_choice = 0;

const char *boot_menu_cmd(int idx)
{
	return (idx >= 0 && idx < NGAMES) ? games[idx].cmd : NULL;
}

static const char *TAG = "bootmenu";
static esp_lcd_touch_handle_t tp;
static esp_lcd_panel_handle_t menu_panel;
static uint16_t *menu_fb;
static uint16_t *thumbs[NGAMES];

#define MENU_W LCD_WIDTH    /* 800, logical x */
#define MENU_H LCD_HEIGHT   /* 480, logical y */
#define BG_BYTES ((size_t) MENU_W * MENU_H * 2)
#define THUMB_BYTES ((size_t) THUMB_W * THUMB_H * 2)

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

/* Halve the brightness of a rectangle, leaving translucent-looking bars. */
static void dim_rect(int x0, int y0, int x1, int y1)
{
	for (int x = x0; x < x1; x++)
		for (int y = y0; y < y1; y++) {
			uint16_t c = menu_fb[x * MENU_H + y];
			menu_fb[x * MENU_H + y] = (c >> 1) & 0x7BEF;
		}
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

static void draw_text_shadow(int x, int y, const char *s, int scale,
			     uint16_t c)
{
	draw_text(x + 2, y + 2, s, scale, 0x0000);
	draw_text(x, y, s, scale, c);
}

static void flip(void)
{
	esp_lcd_panel_draw_bitmap(menu_panel, 0, 0, MENU_H, MENU_W, menu_fb);
}

static void load_bg(int idx)
{
	FILE *f = fopen(games[idx].bg, "rb");
	size_t got = 0;

	if (f) {
		got = fread(menu_fb, 1, BG_BYTES, f);
		fclose(f);
	}
	if (got != BG_BYTES) {
		ESP_LOGW(TAG, "%s missing, solid background", games[idx].bg);
		fill_rect(0, 0, MENU_W, MENU_H, 0x0841);
	}
}

static void load_thumbs(void)
{
	for (int i = 0; i < NGAMES; i++) {
		thumbs[i] = heap_caps_malloc(THUMB_BYTES,
					     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (!thumbs[i])
			continue;
		memset(thumbs[i], 0, THUMB_BYTES);
		FILE *f = fopen(games[i].thumb, "rb");
		if (f) {
			fread(thumbs[i], 1, THUMB_BYTES, f);
			fclose(f);
		} else {
			ESP_LOGW(TAG, "%s missing", games[i].thumb);
		}
	}
}

/* Scale the cached thumbnail (transposed layout) into a card. */
static void draw_thumb(int idx, int x0, int y0, int w, int h)
{
	const uint16_t *src = thumbs[idx];

	if (!src)
		return;
	for (int dy = 0; dy < h; dy++) {
		int sy = dy * THUMB_H / h;
		for (int dx = 0; dx < w; dx++) {
			int sx = dx * THUMB_W / w;
			put_px(x0 + dx, y0 + dy, src[sx * THUMB_H + sy]);
		}
	}
}

static void card_rect(int i, int *x0, int *y0, int *x1, int *y1)
{
	int cols = NGAMES < MENU_COLS ? NGAMES : MENU_COLS;
	int col = i % cols, row = i / cols;
	int margin = 12, gap = 12;
	int bw = (MENU_W - margin * 2 - gap * (cols - 1)) / cols;
	int total = cols * bw + gap * (cols - 1);
	int left = (MENU_W - total) / 2;

	*x0 = left + col * (bw + gap);
	*x1 = *x0 + bw;
	*y0 = MENU_H - margin - CARD_H + row * (CARD_H + gap);
	*y1 = *y0 + CARD_H;
}

static void draw_card(int i, int selected)
{
	int x0, y0, x1, y1;

	card_rect(i, &x0, &y0, &x1, &y1);
	uint16_t border = selected ? 0x07E0 : 0x7BEF;
	uint16_t fg = selected ? 0x07E0 : 0xFFFF;
	int tw = (int) strlen(games[i].title) * 8 * 2;

	fill_rect(x0 + 2, y0 + 2, x1 - 2, y1 - 2, 0x10A2);
	fill_rect(x0, y0, x1, y0 + 3, border);
	fill_rect(x0, y1 - 3, x1, y1, border);
	fill_rect(x0, y0, x0 + 3, y1, border);
	fill_rect(x1 - 3, y0, x1, y1, border);
	draw_text_shadow(x0 + (x1 - x0 - tw) / 2, y0 + 8, games[i].title, 2,
			 fg);
	draw_thumb(i, x0 + 4, y0 + TITLE_H, (x1 - x0) - 8,
		   (y1 - y0) - TITLE_H - 4);
}

static void draw_menu(int selected)
{
	load_bg(selected);
	dim_rect(0, 0, MENU_W, 40);
	dim_rect(0, MENU_H - CARD_H - 24, MENU_W, MENU_H);
	draw_text_shadow(16, 10, "TINY386", 2, 0xFFFF);
	for (int i = 0; i < NGAMES; i++)
		draw_card(i, i == selected);
	flip();
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
		return 0;
	if (touch_init() != ESP_OK) {
		ESP_LOGE(TAG, "GT911 init failed, booting the first game");
		return 0;
	}
	load_thumbs();

	ESP_LOGI(TAG, "menu ready");
	draw_menu(0);
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
			for (int i = 0; i < NGAMES; i++) {
				int x0, y0, x1, y1;
				card_rect(i, &x0, &y0, &x1, &y1);
				if (x >= x0 && x < x1 && y >= y0 && y < y1) {
					draw_menu(i);
					vTaskDelay(pdMS_TO_TICKS(250));
					return i;
				}
			}
		}
		vTaskDelay(pdMS_TO_TICKS(20));
	}
}

#endif
