/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_psram.h"
#include "esp_partition.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_idf_version.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#include "driver/usb_serial_jtag_vfs.h"
#define esp_vfs_usb_serial_jtag_use_driver usb_serial_jtag_vfs_use_driver
#else
#include "esp_vfs_dev.h"
#endif
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "esp_system.h"

#include "../../ini.h"
#include "../../pc.h"
#include "../../i8042.h"
#include "../../term.h"
#include "common.h"
#include "gamepad.h"
#include "battery.h"
#include "board_adc.h"
#include "wifi.h"
#include "sdcli.h"

//
#include "esp_private/system_internal.h"
uint32_t get_uticks()
{
	return esp_system_get_time();
}

void *psmalloc(long size);
void *fbmalloc(long size);
void *bigmalloc(size_t size)
{
	return psmalloc(size);
}

static char *pcram;
static long pcram_off;
static long pcram_len;
void *pcmalloc(long size)
{
	void *ret = pcram + pcram_off;

	size = (size + 31) / 32 * 32;
	if (pcram_off + size > pcram_len) {
		fprintf(stderr, "pcram error %ld %ld %ld\n", size, pcram_off, pcram_len);
		abort();
	}
	pcram_off += size;
	return ret;
}

void pcmalloc_init(void *ptr, long len)
{
	pcram = ptr;
	pcram_len = len;
}

extern const uint8_t _binary_fb_bios_bin_start[];
extern const uint8_t _binary_fb_bios_bin_end[];
extern const uint8_t _binary_fb_vga_bin_start[];
extern const uint8_t _binary_fb_vga_bin_end[];
#ifndef BOARD_NO_BUILTIN_DOS_IMG
extern const uint8_t _binary_fb_dos_img_start[];
extern const uint8_t _binary_fb_dos_img_end[];
#endif

int load_rom(void *phys_mem, const char *file, uword addr, int backward)
{
	if (file && file[0] == '@') {
		const uint8_t *src;
		size_t len;
		if (strcmp(file, "@bios") == 0) {
			src = _binary_fb_bios_bin_start;
			len = _binary_fb_bios_bin_end - _binary_fb_bios_bin_start;
		} else if (strcmp(file, "@vga") == 0) {
			src = _binary_fb_vga_bin_start;
			len = _binary_fb_vga_bin_end - _binary_fb_vga_bin_start;
		} else {
			return 0;
		}
		memcpy((char *)phys_mem + (backward ? addr - len : addr), src, len);
		return len;
	}
	if (file && file[0] == '/') {
		FILE *fp = fopen(file, "rb");
		if (!fp) {
			perror(file);
			return 0;
		}
		fseek(fp, 0, SEEK_END);
		int len = ftell(fp);
		rewind(fp);
		int total = 0;
		while (total < len) {
			int n = len - total;
			if (n > 16384)
				n = 16384;
			int r = fread((char *)phys_mem +
				      (backward ? addr - len : addr) + total,
				      1, n, fp);
			if (r <= 0)
				break;
			total += r;
		}
		fprintf(stderr, "%s loaded %d/%d\n", file, total, len);
		fclose(fp);
		return len;
	}
	const esp_partition_t *part =
		esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
					 ESP_PARTITION_SUBTYPE_ANY,
					 file);
	assert(part);
	int len = part->size;
	fprintf(stderr, "%s len %d\n", file, len);
	if (backward)
		esp_partition_read(part, 0, phys_mem + addr - len, len);
	else
		esp_partition_read(part, 0, phys_mem + addr, len);
	return len;
}

//
EventGroupHandle_t global_event_group;
struct Globals globals;

typedef struct {
	PC *pc;
	u8 *fb1;
	u8 *fb;
} Console;

#define NN 32
#ifdef SWAPXY
#define FB_W LCD_HEIGHT
#define FB_H LCD_WIDTH
#else
#define FB_W LCD_WIDTH
#define FB_H LCD_HEIGHT
#endif
#define BAND_MAX ((FB_H + NN - 1) / NN)

Console *console_init(int width, int height)
{
	Console *c = malloc(sizeof(Console));
	c->fb1 = fbmalloc(FB_W * BAND_MAX * 2);
	if (globals.panel_fb) {
		/* Zero-copy: VGA renders directly into the RGB DMA frame buffer */
		c->fb = globals.panel_fb;
	} else {
		c->fb = bigmalloc(LCD_WIDTH * LCD_HEIGHT * 2);
	}
	return c;
}

void lcd_draw(int x_start, int y_start, int x_end, int y_end, void *src);

#if defined(LCD_FPS_OVERLAY) || defined(BATTERY_ADC_GPIO)
extern const uint8_t vgafont16[256 * 16];

static void hud_px(Console *s, int x, int y, uint16_t c)
{
	if (x >= 0 && x < LCD_WIDTH && y >= 0 && y < LCD_HEIGHT)
#ifdef SWAPXY
		((uint16_t *) s->fb)[x * LCD_HEIGHT + y] = c;
#else
		((uint16_t *) s->fb)[y * LCD_WIDTH + x] = c;
#endif
}
#endif

#ifdef LCD_FPS_OVERLAY
/* Small "N FPS" in the top-left of the picture, like a Steam Deck overlay.
 * Drawn into the VGA buffer so the PPA scales it with the frame. */
static void draw_fps_overlay(Console *s, int cx, int cy, int fps)
{
	char buf[16];
	int x0 = cx + 4, y0 = cy + 4;

	snprintf(buf, sizeof(buf), "%d FPS", fps);
	for (char *p = buf; *p; p++) {
		const uint8_t *g = vgafont16 + (uint8_t) *p * 16;
		for (int row = 0; row < 16; row++)
			for (int col = 0; col < 8; col++)
				if (g[row] & (0x80 >> col)) {
					hud_px(s, x0 + col + 1, y0 + row + 1,
					       0x0000);
					hud_px(s, x0 + col, y0 + row, 0xFFFF);
				}
		x0 += 8;
	}
}
#endif

#ifdef BATTERY_ADC_GPIO
static void hud_rect(Console *s, int x, int y, int w, int h, uint16_t c)
{
	for (int yy = y; yy < y + h; yy++)
		for (int xx = x; xx < x + w; xx++)
			hud_px(s, xx, yy, c);
}

static void hud_text(Console *s, int x, int y, const char *t, uint16_t c)
{
	for (; *t; t++) {
		const uint8_t *g = vgafont16 + (uint8_t) *t * 16;
		for (int row = 0; row < 16; row++)
			for (int col = 0; col < 8; col++)
				if (g[row] & (0x80 >> col)) {
					hud_px(s, x + col + 1, y + row + 1,
					       0x0000);
					hud_px(s, x + col, y + row, c);
				}
		x += 8;
	}
}

/* Battery icon + percentage in the top-right of the picture, same layer as
 * the FPS counter. Green >= 50%, yellow 20..49%, red < 20%. */
static void draw_batt_overlay(Console *s, int x_right, int y_top, int pct)
{
	char buf[16];
	uint16_t color = pct >= 50 ? 0x07E0 :
			 pct >= 20 ? 0xFFE0 : 0xF800;
	int bw = 22, bh = 12;
	int x0 = x_right - 4 - bw;
	int y0 = y_top + 2;
	int inner_w = bw - 4;
	int filled = inner_w * pct / 100;

	snprintf(buf, sizeof(buf), "%d%%", pct);
	hud_text(s, x0 - 6 - (int) strlen(buf) * 8, y_top, buf, color);

	/* black drop shadow, then white shell, then the colored fill */
	hud_rect(s, x0 - 1, y0 - 1, bw + 2, bh + 2, 0x0000);
	hud_rect(s, x0 + bw, y0 + 3, 4, 6, 0x0000);
	hud_rect(s, x0, y0, bw, 1, 0xFFFF);
	hud_rect(s, x0, y0 + bh - 1, bw, 1, 0xFFFF);
	hud_rect(s, x0, y0, 1, bh, 0xFFFF);
	hud_rect(s, x0 + bw - 1, y0, 1, bh, 0xFFFF);
	hud_rect(s, x0 + bw, y0 + 4, 3, 4, 0xFFFF);
	hud_rect(s, x0 + 2, y0 + 2, inner_w, bh - 4, 0x0000);
	if (filled > 0)
		hud_rect(s, x0 + 2, y0 + 2, filled, bh - 4, color);
}
#endif

#ifdef LCD_FILL_DIRECT
void lcd_fill_frame(void *src, int cx, int cy, int cw, int ch);
#endif

#if defined(LCD_FILL_SCREEN) && !defined(LCD_FILL_DIRECT)
/* Stretch the picture's on-screen rectangle to the whole panel.
 * The framebuffer is stored transposed (SWAPXY), so one destination
 * row is a column of the logical picture. */
static void fill_band(Console *s, int cx, int cy, int cw, int ch,
		      int y0, int y1)
{
	uint16_t *dst = (uint16_t *) s->fb1;
	const uint16_t *fb = (const uint16_t *) s->fb;
	uint32_t ystep = (uint32_t) ch * 65536 / LCD_HEIGHT;

	/* SWAPXY stores logical (x, y) at x * FB_W + y: one framebuffer row
	 * is one picture column, so both loops walk memory in order. */
	for (int py = y0; py < y1; py++) {
		const uint16_t *row =
			fb + (size_t) (cx + py * cw / LCD_WIDTH) * FB_W;
		uint16_t *d = dst + (size_t) (py - y0) * FB_W;
		uint32_t yacc = (uint32_t) cy * 65536;
		for (int px = 0; px < FB_W; px++) {
			*d++ = row[yacc >> 16];
			yacc += ystep;
		}
	}
}
#endif

static void redraw(void *opaque,
		   int x, int y, int w, int h)
{
#ifdef USE_LCD_SERIAL
	(void)opaque;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	return;
#endif
	Console *s = opaque;
	int cx = 0, cy = 0, cw = 0, ch = 0, fill = 0;
#ifdef LCD_FILL_SCREEN
	vga_get_content_rect(s->pc->vga, &cx, &cy, &cw, &ch);
	fill = cw > 0 && ch > 0 &&
		(cx != 0 || cy != 0 || cw != LCD_WIDTH || ch != LCD_HEIGHT);
#endif
#ifdef LCD_FPS_OVERLAY
	static int ov_fps, ov_frames, ov_last;
	int nowv = get_uticks();
	if (!ov_last)
		ov_last = nowv;
	ov_frames++;
	if (nowv - ov_last >= 1000000) {
		ov_fps = (int) ((long long) ov_frames * 1000000 /
				(nowv - ov_last));
		ov_frames = 0;
		ov_last = nowv;
	}
	if (fill)
		draw_fps_overlay(s, cx, cy, ov_fps);
#endif
#ifdef BATTERY_ADC_GPIO
	{
		int pct = battery_get_percent();

		if (pct >= 0)
			draw_batt_overlay(s, fill ? cx + cw - 4 : LCD_WIDTH - 4,
					  fill ? cy + 4 : 4, pct);
	}
#endif
#ifdef LCD_FILL_DIRECT
	if (fill) {
		lcd_fill_frame(s->fb, cx, cy, cw, ch);
		vga_step(s->pc->vga);
	} else
#endif
#ifdef LCD_SINGLE_FRAME
	{
	lcd_draw(0, 0, FB_W, FB_H, s->fb);
	vga_step(s->pc->vga);
	}
#else
	for (int i = 0; i < NN; i++) {
		int y0 = FB_H * i / NN;
		int y1 = FB_H * (i + 1) / NN;
		int rows = y1 - y0;
		if (rows <= 0)
			continue;
#if defined(LCD_FILL_SCREEN) && !defined(LCD_FILL_DIRECT)
		if (fill) {
			fill_band(s, cx, cy, cw, ch, y0, y1);
		} else
#endif
		{
			uint16_t *src = (uint16_t *) s->fb + (size_t)FB_W * y0;
			memcpy(s->fb1, src, (size_t)FB_W * rows * 2);
		}
		lcd_draw(0, y0, FB_W, y1, s->fb1);
		vga_step(s->pc->vga);
#ifndef LCD_NO_BAND_DELAY
		usleep(900);
#endif
	}
#endif
}

#ifdef BOOT_MENU
extern int boot_choice;

extern int boot_choice;
const char *boot_menu_cmd(int idx);

static int dos_prompt_visible(PC *pc)
{
	char line[81];

	if (!pc->vga || !vga_is_text_mode(pc->vga))
		return 0;
	for (int r = 0; r < 25; r++) {
		for (int c = 0; c < 80; c++)
			line[c] = vga_mem_read(pc->vga,
					       0xB8000 + (r * 80 + c) * 2);
		line[80] = 0;
		if (strstr(line, "C:\\>"))
			return 1;
	}
	return 0;
}

/* Type the selected game's command once DOS shows its prompt. */
static void boot_autolaunch(PC *pc)
{
	static const char *cmd;
	static int started, done, pos, held, held_shift;
	static uint32_t t, next_scan;
	uint32_t now = get_uticks();

	if (done)
		return;
	if (!started) {
		if (now < next_scan)
			return;
		next_scan = now + 200000;
		if (!dos_prompt_visible(pc))
			return;
		cmd = boot_menu_cmd(boot_choice);
		if (!cmd) {
			gamepad_set_enabled(1);
			done = 1;
			return;
		}
		fprintf(stderr, "autolaunch: prompt found, typing %s\n", cmd);
		started = 1;
		pos = 0;
		t = now;
		return;
	}
	if (held) {
		if (now - t >= 60000) {
			ps2_put_keycode(pc->kbd, 0, held);
			if (held_shift)
				ps2_put_keycode(pc->kbd, 0, 0x2A);
			held = 0;
			t = now;
		}
		return;
	}
	if (now - t < 30000)
		return;
	if (!cmd[pos]) {
		fprintf(stderr, "autolaunch: done\n");
		gamepad_set_enabled(1);
		done = 1;
		return;
	}
	int shift = 0;
	int scan = term_ascii_to_scan(cmd[pos], &shift);
	pos++;
	if (!scan) {
		t = now;
		return;
	}
	if (shift)
		ps2_put_keycode(pc->kbd, 1, 0x2A);
	ps2_put_keycode(pc->kbd, 1, scan);
	held = scan;
	held_shift = shift;
	t = now;
}
#endif

static int pc_main(const char *file)
{
	PCConfig conf;
	memset(&conf, 0, sizeof(conf));
	conf.mem_size = 8 * 1024 * 1024;
	conf.vga_mem_size = 256 * 1024;
	conf.cpu_gen = 4;
	conf.fpu = 0;

	if (file) {
		int err = ini_parse(file, parse_conf_ini, &conf);
		if (err) {
			fprintf(stderr, "error %d\n", err);
			return err;
		}
	} else {
#ifdef BOARD_NO_BUILTIN_DOS_IMG
		fprintf(stderr, "no boot config found and no built-in disk on this board\n");
		return -1;
#else
		fprintf(stderr, "booting built-in SD-less DOS fallback\n");
		conf.mem_size = 4 * 1024 * 1024;
		conf.vga_mem_size = 768 * 1024;
		conf.cpu_gen = 3;
		conf.bios = "@bios";
		conf.vga_bios = "@vga";
		conf.disks[0] = "@fallback";
		conf.fill_cmos = 1;
		conf.vga_force_8dm = 1;
#endif
	}

	if (conf.width != LCD_WIDTH || conf.height != LCD_HEIGHT) {
		fprintf(stderr, "fixing width/height mismatch %dx%d => %dx%d\n",
			conf.width, conf.height, LCD_WIDTH, LCD_HEIGHT);
		conf.width = LCD_WIDTH;
		conf.height = LCD_HEIGHT;
	}

	Console *console = console_init(conf.width, conf.height);
	PC *pc = pc_new(redraw, console, console->fb, &conf);
	console->pc = pc;
	globals.pc = pc;
	globals.kbd = pc->kbd;
	globals.mouse = pc->mouse;
	xEventGroupSetBits(global_event_group, BIT0);

	load_bios_and_reset(pc);

	pc->boot_start_time = get_uticks();
	for (; pc->shutdown_state != 8;) {
		pc_step(pc);
#ifdef BOOT_MENU
		boot_autolaunch(pc);
#endif
	}
	return 0;
}

//

void *esp_psram_get(size_t *size);
void vga_task(void *arg);
void i2s_main();
void wifi_main(const char *, const char *);
void storage_init(void);
void usb_setup(void);

struct esp_ini_config {
	const char *filename;
	char ssid[33];
	char pass[65];
	char web_pass[65];
	int enable_usb;
	int web;
};

static void i386_task(void *arg)
{
	struct esp_ini_config *config = arg;
	int core_id = esp_cpu_get_core_id();
	fprintf(stderr, "main runs on core %d\n", core_id);
	/* Wait for LCD panel (and panel_fb) to be ready before starting the
	 * PC emulator.  console_init() uses globals.panel_fb if set. */
	xEventGroupWaitBits(global_event_group,
	                    BIT1,
	                    pdFALSE,
	                    pdFALSE,
	                    portMAX_DELAY);
	/* The USB-Serial/JTAG console is the boot-menu file channel; hand it
	 * to term.c before the DOS serial keyboard starts. */
	sdcli_stop();
	pc_main(config->filename);
	vTaskDelete(NULL);
}

static char *psram;
static long psram_off;
static long psram_len;
void *psmalloc(long size)
{
	void *ret = psram + psram_off;

	size = (size + 4095) / 4096 * 4096;
	if (psram_off + size > psram_len) {
		fprintf(stderr, "psram error %ld %ld %ld\n", size, psram_off, psram_len);
		abort();
	}
	psram_off += size;
	return ret;
}

void *fbmalloc(long size)
{
	void *fb = (uint8_t *) heap_caps_calloc(1, size, MALLOC_CAP_DMA);
	if (!fb) {
		fprintf(stderr, "fbmalloc error %ld\n", size);
		abort();
	}
	return fb;
}

#ifndef BOARD_NO_BUILTIN_DOS_IMG
static void install_fast_boot(void)
{
	const uint8_t *blob = _binary_fb_dos_img_start;
	size_t blob_len = _binary_fb_dos_img_end - _binary_fb_dos_img_start;

	FILE *f = fopen("/sdcard/tiny386.ini", "rb");
	if (!f)
		return;
	char cur[512];
	int n = fread(cur, 1, sizeof(cur) - 1, f);
	fclose(f);
	if (n < 0)
		n = 0;
	cur[n] = 0;

	bool need_img = true;
	FILE *img = fopen("/sdcard/dos_fast.img", "rb");
	if (img) {
		fseek(img, 0, SEEK_END);
		need_img = ftell(img) != (long)blob_len;
		fclose(img);
	}
	if (need_img) {
		fprintf(stderr, "installing fast boot disk...\n");
		FILE *o = fopen("/sdcard/dos_fast.img", "wb");
		if (!o) {
			perror("dos_fast.img");
			return;
		}
		size_t off = 0;
		while (off < blob_len) {
			size_t k = blob_len - off;
			if (k > 16384)
				k = 16384;
			if (fwrite(blob + off, 1, k, o) != k) {
				perror("dos_fast.img write");
				fclose(o);
				return;
			}
			off += k;
		}
		fclose(o);
		fprintf(stderr, "fast boot disk installed\n");
	}

	bool has_cdb = strstr(cur, "cdb") != NULL;
	bool has_iso = false;
	FILE *iso = fopen("/sdcard/paldos.iso", "rb");
	if (iso) {
		has_iso = true;
		fclose(iso);
	}

	if (strstr(cur, "dos_fast.img")) {
		if (has_cdb || !has_iso) {
			if (!has_iso)
				fprintf(stderr, "paldos.iso missing: PAL CD check will fail\n");
			return;
		}
	}

	static const char ini[] =
		"[pc]\n"
		"bios = /sdcard/bios.bin\n"
		"vga_bios = /sdcard/vgabios.bin\n"
		"mem_size = 4M\n"
		"vga_mem_size = 768K\n"
		"hda = /sdcard/dos_fast.img\n"
		"hdb = /sdcard/games.img\n"
		"%s"
		"fill_cmos = 1\n"
		"vga_force_8dm = 1\n"
		"\n[display]\n"
		"width = 640\n"
		"height = 480\n"
		"\n[cpu]\n"
		"gen = 3\n"
		"fpu = 0\n";

	FILE *b = fopen("/sdcard/tiny386.bak", "wb");
	if (b) {
		fwrite(cur, 1, n, b);
		fclose(b);
	}
	FILE *w = fopen("/sdcard/tiny386.ini", "wb");
	if (!w) {
		perror("tiny386.ini");
		return;
	}
	fprintf(w, ini, has_iso ? "cdb = /sdcard/paldos.iso\n" : "");
	fclose(w);
	fprintf(stderr, "tiny386.ini switched to fast boot%s\n",
		has_iso ? " (cdb added)" : "");
}
#endif

static bool ini_key_is(const char *p, size_t len, const char *key)
{
	size_t k = strlen(key);

	if (len < k || strncmp(p, key, k) != 0)
		return false;
	return len == k || p[k] == ' ' || p[k] == '\t' || p[k] == '=';
}

/* Add one cdb line to the existing [pc] section if it is missing.  The old
 * version rewrote the whole file from a template and silently discarded
 * every user setting.  This only inserts the missing key. */
static void ensure_cdb_in_ini(void)
{
	FILE *f = fopen("/sdcard/tiny386.ini", "rb");
	long size;
	char *cur;
	size_t n;
	bool in_pc = false, has_cdb = false, insert_nl = false;
	char *insert = NULL;
	char *line;

	if (!f)
		return;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return;
	}
	size = ftell(f);
	if (size < 0 || size > 8191) {
		fclose(f);
		fprintf(stderr, "tiny386.ini too large; cdb not added\n");
		return;
	}
	rewind(f);
	cur = malloc((size_t) size + 1);
	if (!cur) {
		fclose(f);
		return;
	}
	n = fread(cur, 1, (size_t) size, f);
	fclose(f);
	cur[n] = 0;

	for (line = cur; *line; ) {
		char *eol = strchr(line, '\n');
		size_t len = eol ? (size_t) (eol - line) : strlen(line);
		char *s = line;
		size_t l = len;

		if (l && s[l - 1] == '\r')
			l--;
		while (l && (*s == ' ' || *s == '\t')) {
			s++;
			l--;
		}
		if (l && s[0] == '[') {
			in_pc = l >= 4 && !strncmp(s, "[pc]", 4);
			if (in_pc && !insert) {
				insert = eol ? eol + 1 : line + len;
				insert_nl = eol != NULL;
			}
		} else if (in_pc && ini_key_is(s, l, "cdb")) {
			has_cdb = true;
			break;
		}
		if (!eol)
			break;
		line = eol + 1;
	}
	if (has_cdb) {
		free(cur);
		return;
	}
	if (!insert) {
		free(cur);
		fprintf(stderr, "tiny386.ini: no [pc] section, cdb not added\n");
		return;
	}
	FILE *iso = fopen("/sdcard/paldos.iso", "rb");
	if (!iso) {
		free(cur);
		fprintf(stderr, "paldos.iso missing: cdb not added\n");
		return;
	}
	fclose(iso);

	/* Keep the user's tiny386.bak untouched: it may hold the config the
	 * old template overwrote.  Use a separate backup for this edit. */
	FILE *b = fopen("/sdcard/tiny386.cdb.bak", "wb");
	if (b) {
		fwrite(cur, 1, n, b);
		fclose(b);
	}
	FILE *w = fopen("/sdcard/.ini.tmp", "wb");
	if (!w) {
		free(cur);
		perror("ini tmp");
		return;
	}
	size_t off = (size_t) (insert - cur);
	fwrite(cur, 1, off, w);
	if (!insert_nl)
		fputc('\n', w);
	fputs("cdb = /sdcard/paldos.iso\n", w);
	fwrite(cur + off, 1, n - off, w);
	fflush(w);
	fclose(w);
	remove("/sdcard/tiny386.ini");
	if (rename("/sdcard/.ini.tmp", "/sdcard/tiny386.ini") != 0) {
		perror("tiny386.ini rename");
		rename("/sdcard/tiny386.cdb.bak", "/sdcard/tiny386.ini");
		free(cur);
		return;
	}
	free(cur);
	fprintf(stderr, "tiny386.ini: cdb added, other settings preserved\n");
}

static void copy_ini_value(char *dst, size_t dst_len, const char *value)
{
	size_t n = strlen(value);

	if (n >= dst_len)
		n = dst_len - 1;
	memcpy(dst, value, n);
	dst[n] = 0;
}

static int parse_ini(void* user, const char* section,
		     const char* name, const char* value)
{
	struct esp_ini_config *conf = user;
#define SEC(a) (strcmp(section, a) == 0)
#define NAME(a) (strcmp(name, a) == 0)
	if (SEC("esp")) {
		if (NAME("ssid")) {
			copy_ini_value(conf->ssid, sizeof(conf->ssid), value);
		} else if (NAME("pass")) {
			copy_ini_value(conf->pass, sizeof(conf->pass), value);
		} else if (NAME("web_pass")) {
			copy_ini_value(conf->web_pass, sizeof(conf->web_pass),
				       value);
		} else if (NAME("web")) {
			conf->web = atoi(value) != 0;
		} else if (NAME("enable_usb")) {
			conf->enable_usb = atoi(value);
		}
	}
#undef SEC
#undef NAME
	return 1;
}

void app_main(void)
{
	global_event_group = xEventGroupCreate();

#if defined(USE_LCD_SERIAL) || defined(USE_USB_SERIAL_KBD)
	usb_serial_jtag_driver_config_t usb_cfg = {
		.tx_buffer_size = 2048,
		.rx_buffer_size = 2048,
	};
	ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));
	esp_vfs_usb_serial_jtag_use_driver();
#endif

#ifdef ESPDEBUG
	uart_config_t uart_config = {
		.baud_rate = 115200,
		.data_bits = UART_DATA_8_BITS,
		.parity	= UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};

	uart_param_config(UART_NUM_0, &uart_config);
	if (uart_driver_install(UART_NUM_0, 2 * 1024, 0, 0, NULL, 0) != ESP_OK) {
		assert(false);
	}
#endif

	i2s_main();
	storage_init();
#if defined(USE_GAMEPAD) || defined(BATTERY_ADC_GPIO)
	board_adc2_init();
#endif
	battery_init();
	gamepad_init();
#ifdef SD_LIST
	{
		DIR *d = opendir("/sdcard");
		if (d) {
			struct dirent *e;
			while ((e = readdir(d))) {
				char p[320];
				struct stat st;
				snprintf(p, sizeof(p), "/sdcard/%s", e->d_name);
				if (stat(p, &st) == 0)
					fprintf(stderr, "SD %s %ld\n", e->d_name, (long)st.st_size);
			}
			closedir(d);
		}
		FILE *f = fopen("/sdcard/tiny386.ini", "rb");
		if (f) {
			char b[512];
			int n = fread(b, 1, sizeof(b) - 1, f);
			b[n] = 0;
			fclose(f);
			fprintf(stderr, "INI:\n%s\n", b);
		}
	}
#endif
#ifndef BOARD_NO_BUILTIN_DOS_IMG
	install_fast_boot();
#endif
	ensure_cdb_in_ini();

	esp_psram_init();
#ifndef PSRAM_ALLOC_LEN
	// use the whole psram
	size_t len;
	psram = esp_psram_get(&len);
	psram_len = len;
#else
	psram_len = PSRAM_ALLOC_LEN;
	psram = heap_caps_calloc(1, psram_len, MALLOC_CAP_SPIRAM);
#endif
	const static char *files[] = {
		"/sdcard/tiny386.ini",
		"/spiflash/tiny386.ini",
		NULL,
	};
	static struct esp_ini_config config;
	/* The C6 hosted link on this board only passes broadcast traffic:
	 * ARP/DHCP work but unicast ICMP/TCP does not, so the web UI cannot be
	 * reached.  Leave it off by default; set web = 1 in [esp] to try it
	 * after fixing the C6/host data path. */
	config.web = 0;
	for (int i = 0; files[i]; i++) {
		if (ini_parse(files[i], parse_ini, &config) == 0) {
			config.filename = files[i];
			break;
		}
	}
	if (!config.filename)
		fprintf(stderr, "no ini found, booting built-in DOS fallback\n");

	if (config.enable_usb) {
		vTaskDelay(2000 / portTICK_PERIOD_MS);
		usb_setup();
	}

#ifdef BOOT_MENU
	/* vga_task starts this right before the boot menu and stops it when a
	 * game is picked, so WiFi never runs during emulation. */
	wifi_configure(config.ssid, config.pass, config.web_pass, config.web);
#else
	if (config.ssid[0])
		wifi_main(config.ssid, config.pass);
#endif
	/* Available until the menu selection starts the emulator. */
	sdcli_start();

	if (psram) {
		xTaskCreatePinnedToCore(i386_task, "i386_main", 8192, &config, 3, NULL, 1);
		xTaskCreatePinnedToCore(vga_task, "vga_task", 8192, NULL, 0, NULL, 0);
	}
}
