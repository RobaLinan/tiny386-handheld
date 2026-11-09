#ifdef BOOT_MENU

#include <stdio.h>
#include <stdlib.h>
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
#include "esp_timer.h"

#include "common.h"
#include "../../ini.h"

extern const uint8_t vgafont16[256 * 16];
extern esp_err_t bsp_display_brightness_set(uint8_t percent);
int lcd_get_brightness(void);
void i2s_set_volume(int vol);
int i2s_get_volume(void);

/* Games come from /sdcard/menu.ini:
 *
 *   [game1]
 *   title = PAL
 *   cmd = D:\PAL.BAT
 *   thumb = /sdcard/menu/pal_thumb.raw
 *
 * `cmd` is typed once DOS shows its prompt (Enter is appended). Missing
 * file = the two built-in entries below. */
#define MAX_GAMES 16

struct game_entry {
	char title[32];
	char cmd[96];
	char thumb[96];
};

static struct game_entry games[MAX_GAMES];
static int ngames;

/* Selected entry; read by pc_main(). */
int boot_choice = 0;

const char *boot_menu_cmd(int idx)
{
	return (idx >= 0 && idx < ngames) ? games[idx].cmd : NULL;
}

static const char *TAG = "bootmenu";
static esp_lcd_touch_handle_t tp;
static esp_lcd_panel_handle_t menu_panel;
static uint16_t *menu_fb;    /* full menu screen, transposed */
static uint16_t *strip;      /* pre-rendered card strip, transposed */
static uint16_t *staging;    /* aligned copy of the visible strip window */
static uint16_t *thumbs[MAX_GAMES];
static int strip_w;          /* strip width in logical x */
static int max_scroll;
static int scroll;

#define MENU_W LCD_WIDTH    /* 800 */
#define MENU_H LCD_HEIGHT   /* 480 */
#define CARD_W 320
#define CARD_H 250
#define CARD_GAP 16
#define STRIP_Y (MENU_H - 12 - CARD_H)
#define STRIP_H CARD_H
#define TITLE_H 40
#define THUMB_W 380
#define THUMB_H 214
#define THUMB_BYTES ((size_t) THUMB_W * THUMB_H * 2)
#define WINDOW_PX ((size_t) MENU_W * STRIP_H)

static void add_cmd_enter(struct game_entry *g)
{
	size_t n = strlen(g->cmd);

	if (n && n + 1 < sizeof(g->cmd)) {
		g->cmd[n] = '\r';
		g->cmd[n + 1] = 0;
	}
}

static int menu_ini_handler(void *user, const char *section, const char *name,
			    const char *value)
{
	int idx = -1;

	(void) user;
	if (sscanf(section, "game%d", &idx) != 1 || idx < 1 ||
	    idx > MAX_GAMES)
		return 1;
	struct game_entry *g = &games[idx - 1];
	if (!strcmp(name, "title"))
		snprintf(g->title, sizeof(g->title), "%s", value);
	else if (!strcmp(name, "cmd"))
		snprintf(g->cmd, sizeof(g->cmd), "%s", value);
	else if (!strcmp(name, "thumb"))
		snprintf(g->thumb, sizeof(g->thumb), "%s", value);
	return 1;
}

static void add_default(int *n, const char *title, const char *cmd,
			const char *thumb)
{
	struct game_entry *g = &games[(*n)++];

	snprintf(g->title, sizeof(g->title), "%s", title);
	snprintf(g->cmd, sizeof(g->cmd), "%s", cmd);
	snprintf(g->thumb, sizeof(g->thumb), "%s", thumb);
}

static void load_games(void)
{
	int parsed = ini_parse("/sdcard/menu.ini", menu_ini_handler, NULL);
	int n = 0;

	if (parsed == 0) {
		for (int i = 0; i < MAX_GAMES; i++) {
			if (!games[i].title[0])
				continue;
			if (n != i)
				games[n] = games[i];
			add_cmd_enter(&games[n]);
			n++;
		}
	}
	if (n == 0) {
		ESP_LOGW(TAG, "/sdcard/menu.ini missing or empty, defaults");
		add_default(&n, "PAL", "D:\\PAL.BAT",
			    "/sdcard/menu/pal_thumb.raw");
		add_default(&n, "FD2", "D:\\FD2",
			    "/sdcard/menu/fd2_thumb.raw");
		for (int i = 0; i < n; i++)
			add_cmd_enter(&games[i]);
	}
	ngames = n;
	for (int i = 0; i < ngames; i++)
		ESP_LOGI(TAG, "game %d: %s -> %s", i + 1, games[i].title,
			 games[i].cmd);
}

/* Logical (x, y) lives at x * height + y in a transposed buffer. */
static inline void put_px(uint16_t *buf, int h, int x, int y, uint16_t c)
{
	buf[x * h + y] = c;
}

static void fill_buf_rect(uint16_t *buf, int h, int x0, int y0, int x1,
			  int y1, uint16_t c)
{
	for (int x = x0; x < x1; x++)
		for (int y = y0; y < y1; y++)
			put_px(buf, h, x, y, c);
}

static void draw_text(uint16_t *buf, int h, int x, int y, const char *s,
		      int scale, uint16_t c)
{
	for (; *s; s++) {
		const uint8_t *glyph = vgafont16 + (uint8_t) *s * 16;
		for (int row = 0; row < 16; row++) {
			uint8_t bits = glyph[row];
			for (int col = 0; col < 8; col++)
				if (bits & (0x80 >> col))
					fill_buf_rect(buf, h, x + col * scale,
						      y + row * scale,
						      x + (col + 1) * scale,
						      y + (row + 1) * scale, c);
		}
		x += 8 * scale;
	}
}

static void draw_text_shadow(uint16_t *buf, int h, int x, int y,
			     const char *s, int scale, uint16_t c)
{
	draw_text(buf, h, x + 2, y + 2, s, scale, 0x0000);
	draw_text(buf, h, x, y, s, scale, c);
}

/* Halve the brightness of a rectangle, leaving a translucent-looking bar. */
static void dim_rect(int x0, int y0, int x1, int y1)
{
	for (int x = x0; x < x1; x++)
		for (int y = y0; y < y1; y++) {
			uint16_t c = menu_fb[x * MENU_H + y];
			menu_fb[x * MENU_H + y] = (c >> 1) & 0x7BEF;
		}
}

static int card_x(int i)
{
	int content = ngames * CARD_W + (ngames - 1) * CARD_GAP;
	int margin = content >= MENU_W ? 24 : (MENU_W - content) / 2;
	return margin + i * (CARD_W + CARD_GAP);
}

static void load_thumbs(void)
{
	for (int i = 0; i < ngames; i++) {
		thumbs[i] = heap_caps_aligned_alloc(128, THUMB_BYTES,
						    MALLOC_CAP_SPIRAM |
						    MALLOC_CAP_8BIT);
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

/* Draw a thumbnail, scaling the cached 380x214 image into the card. */
static void draw_thumb(uint16_t *buf, int h, int i, int x0, int y0, int w,
		       int hh)
{
	const uint16_t *src = thumbs[i];

	if (!src)
		return;
	for (int dy = 0; dy < hh; dy++) {
		int sy = dy * THUMB_H / hh;
		for (int dx = 0; dx < w; dx++) {
			int sx = dx * THUMB_W / w;
			put_px(buf, h, x0 + dx, y0 + dy,
			       src[sx * THUMB_H + sy]);
		}
	}
}

static void build_strip(void)
{
	int content = ngames * CARD_W + (ngames - 1) * CARD_GAP;
	int margin = content >= MENU_W ? 24 : (MENU_W - content) / 2;

	strip_w = content + margin * 2;
	if (strip_w < MENU_W)
		strip_w = MENU_W;
	max_scroll = strip_w - MENU_W;

	strip = heap_caps_aligned_alloc(128,
					(size_t) strip_w * STRIP_H * 2,
					MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	staging = heap_caps_aligned_alloc(128, WINDOW_PX * 2,
					  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	assert(strip && staging);
	fill_buf_rect(strip, STRIP_H, 0, 0, strip_w, STRIP_H, 0x0861);

	for (int i = 0; i < ngames; i++) {
		int x0 = card_x(i);
		int tw = (int) strlen(games[i].title) * 8 * 2;
		uint16_t border = 0x7BEF;

		fill_buf_rect(strip, STRIP_H, x0 + 2, 2, x0 + CARD_W - 2,
			      CARD_H - 2, 0x10A2);
		fill_buf_rect(strip, STRIP_H, x0, 0, x0 + CARD_W, 3, border);
		fill_buf_rect(strip, STRIP_H, x0, CARD_H - 3, x0 + CARD_W,
			      CARD_H, border);
		fill_buf_rect(strip, STRIP_H, x0, 0, x0 + 3, CARD_H, border);
		fill_buf_rect(strip, STRIP_H, x0 + CARD_W - 3, 0, x0 + CARD_W,
			      CARD_H, border);
		draw_text_shadow(strip, STRIP_H, x0 + (CARD_W - tw) / 2, 8,
				 games[i].title, 2, 0xFFFF);
		draw_thumb(strip, STRIP_H, i, x0 + 4, TITLE_H, CARD_W - 8,
			   CARD_H - TITLE_H - 4);
	}
}

static void stage_strip(void)
{
	int s = scroll;

	if (s < 0)
		s = 0;
	if (s > max_scroll)
		s = max_scroll;
	memcpy(staging, strip + (size_t) s * STRIP_H, WINDOW_PX * 2);
	esp_lcd_panel_draw_bitmap(menu_panel, STRIP_Y, 0, STRIP_Y + STRIP_H,
				  MENU_W, staging);
}

static void draw_background(void)
{
	/* vertical gradient from deep blue to black */
	for (int x = 0; x < MENU_W; x++) {
		for (int y = 0; y < MENU_H; y++) {
			int v = 8 + y / 24;
			uint16_t c = (uint16_t) (v << 11) | (v << 6) | (v / 2);
			menu_fb[x * MENU_H + y] = c;
		}
	}
	draw_text_shadow(menu_fb, MENU_H, 16, 10, "TINY386", 2, 0xFFFF);
	draw_text(menu_fb, MENU_H, 16, 60,
		  "swipe to browse, tap to play", 1, 0xA514);
	dim_rect(0, STRIP_Y - 8, MENU_W, STRIP_Y);
	esp_lcd_panel_draw_bitmap(menu_panel, 0, 0, MENU_H, MENU_W, menu_fb);
}

static int snap_target(int idx)
{
	int t = card_x(idx) + CARD_W / 2 - MENU_W / 2;

	if (t < 0)
		t = 0;
	if (t > max_scroll)
		t = max_scroll;
	return t;
}

static int nearest_card(void)
{
	int best = 0, bestd = 0x7fffffff;

	for (int i = 0; i < ngames; i++) {
		int d = snap_target(i) - scroll;
		if (d < 0)
			d = -d;
		if (d < bestd) {
			bestd = d;
			best = i;
		}
	}
	return best;
}

/* Ease-out animation to a scroll position. */
static void animate_to(int target)
{
	int start = scroll;
	int64_t t0 = esp_timer_get_time();

	for (;;) {
		int64_t dt = esp_timer_get_time() - t0;
		if (dt >= 220000) {
			scroll = target;
			stage_strip();
			return;
		}
		float k = (float) dt / 220000.0f;
		k = 1.0f - (1.0f - k) * (1.0f - k);
		scroll = start + (int) ((target - start) * k);
		stage_strip();
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

static int card_at(int lx)
{
	int sx = scroll + lx;

	for (int i = 0; i < ngames; i++) {
		int x0 = card_x(i);
		if (sx >= x0 && sx < x0 + CARD_W)
			return i;
	}
	return -1;
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

/* In-game gestures: swipe up/down on the left half for brightness, on the
 * right half for volume. No on-screen indicator. Polled from vga_task. */
void boot_menu_gestures(void)
{
	static int active, side, start_x, start_y, last_y, start_val, moved;
	uint16_t rx = 0, ry = 0, strength[1];
	uint8_t cnt = 0;
	int x, y;

	if (!tp)
		return;
	esp_lcd_touch_read_data(tp);
	int down = esp_lcd_touch_get_coordinates(tp, &rx, &ry, strength, &cnt,
						 1) && cnt;
	touch_to_menu(rx, ry, &x, &y);
	if (down && !active) {
		active = 1;
		moved = 0;
		start_x = x;
		start_y = last_y = y;
		side = x < MENU_W / 2 ? 0 : 1;
		start_val = side ? i2s_get_volume() : lcd_get_brightness();
	} else if (down && active) {
		int dx = x - start_x;
		int dy = start_y - y;   /* swipe up is positive */

		if (abs(dy) > 30 && abs(dy) > abs(dx))
			moved = 1;
		last_y = y;
	} else if (!down && active) {
		active = 0;
		/* One vertical swipe = one 10% step; up is louder/brighter. */
		if (moved) {
			int step = last_y < start_y ? 10 : -10;

			if (side) {
				int v = i2s_get_volume() + step;
				if (v < 0)
					v = 0;
				if (v > 100)
					v = 100;
				i2s_set_volume(v);
			} else {
				int v = lcd_get_brightness() + step;
				if (v < 10)
					v = 10;
				if (v > 100)
					v = 100;
				bsp_display_brightness_set(v);
			}
		}
	}
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
	load_games();
	load_thumbs();
	build_strip();
	draw_background();
	ESP_LOGI(TAG, "menu ready (%d games, strip %d)", ngames, strip_w);
	stage_strip();

	int active = 0, start_x = 0, start_scroll = 0, last_x = 0, last_y = 0;
	int moved = 0;
	int64_t start_t = 0;

	for (;;) {
		uint16_t rx = 0, ry = 0, strength[1];
		uint8_t cnt = 0;
		int lx, ly;

		esp_lcd_touch_read_data(tp);
		int down = esp_lcd_touch_get_coordinates(tp, &rx, &ry, strength,
							 &cnt, 1) && cnt;
		touch_to_menu(rx, ry, &lx, &ly);

		if (down && !active) {
			active = 1;
			moved = 0;
			start_x = last_x = lx;
			last_y = ly;
			start_scroll = scroll;
			start_t = esp_timer_get_time();
		} else if (down && active) {
			int dx = lx - start_x;
			if (dx > 10 || dx < -10)
				moved = 1;
			if (moved) {
				int s = start_scroll - dx;
				if (s < 0)
					s = 0;
				if (s > max_scroll)
					s = max_scroll;
				if (s != scroll) {
					scroll = s;
					stage_strip();
				}
			}
			last_x = lx;
			last_y = ly;
		} else if (!down && active) {
			active = 0;
			if (!moved && esp_timer_get_time() - start_t < 500000 &&
			    last_y >= STRIP_Y && last_y < STRIP_Y + STRIP_H) {
				int idx = card_at(last_x);
				if (idx >= 0) {
					animate_to(snap_target(idx));
					vTaskDelay(pdMS_TO_TICKS(150));
					return idx;
				}
			}
			/* fling: a fast drag skips one more card */
			int dir = last_x - start_x > 0 ? -1 : 1;
			int idx = nearest_card();
			if (moved && abs(last_x - start_x) > 80) {
				int s = scroll + dir * (CARD_W + CARD_GAP);
				int best = 0x7fffffff;
				for (int i = 0; i < ngames; i++) {
					int d = abs(snap_target(i) - s);
					if (d < best) {
						best = d;
						idx = i;
					}
				}
			}
			animate_to(snap_target(idx));
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

#endif
