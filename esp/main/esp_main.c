/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
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

#ifdef LCD_FPS_OVERLAY
extern const uint8_t vgafont16[256 * 16];

static void fps_px(Console *s, int x, int y, uint16_t c)
{
	if (x >= 0 && x < LCD_WIDTH && y >= 0 && y < LCD_HEIGHT)
#ifdef SWAPXY
		((uint16_t *) s->fb)[x * LCD_HEIGHT + y] = c;
#else
		((uint16_t *) s->fb)[y * LCD_WIDTH + x] = c;
#endif
}

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
					fps_px(s, x0 + col + 1, y0 + row + 1,
					       0x0000);
					fps_px(s, x0 + col, y0 + row, 0xFFFF);
				}
		x0 += 8;
	}
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
#ifdef LCD_FPS_LOG
	static int frames, last_us;
	static long draw_us, prep_us;
	int now = get_uticks();
	int t0 = now;
#endif
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
#ifdef LCD_FILL_DIRECT
	if (fill) {
		int td = get_uticks();
		lcd_fill_frame(s->fb, cx, cy, cw, ch);
		vga_step(s->pc->vga);
#ifdef LCD_FPS_LOG
		draw_us += get_uticks() - td;
#endif
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
#ifdef LCD_FPS_LOG
		int tp = get_uticks();
#endif
#if defined(LCD_FILL_SCREEN) && !defined(LCD_FILL_DIRECT)
		if (fill) {
			fill_band(s, cx, cy, cw, ch, y0, y1);
		} else
#endif
		{
			uint16_t *src = (uint16_t *) s->fb + (size_t)FB_W * y0;
			memcpy(s->fb1, src, (size_t)FB_W * rows * 2);
		}
#ifdef LCD_FPS_LOG
		int td = get_uticks();
		prep_us += td - tp;
#endif
		lcd_draw(0, y0, FB_W, y1, s->fb1);
		vga_step(s->pc->vga);
#ifndef LCD_NO_BAND_DELAY
		usleep(900);
#endif
#ifdef LCD_FPS_LOG
		draw_us += get_uticks() - td;
#endif
	}
#endif
#ifdef LCD_FPS_LOG
	(void)t0;
	frames++;
	if (now - last_us >= 5000000) {
		extern uint32_t vga_render_us, vga_render_frames;
		fprintf(stderr, "lcd: %d fps, render %lu us, draw %ld us, rect %d,%d %dx%d fill %d text %d\n",
			(int)((long long)frames * 1000000 / (now - last_us)),
			(unsigned long)(vga_render_frames ?
				       vga_render_us / vga_render_frames : 0),
			frames ? draw_us / frames : 0,
			cx, cy, cw, ch, fill,
			vga_is_text_mode(s->pc->vga));
		vga_render_us = 0;
		vga_render_frames = 0;
		frames = 0;
		prep_us = 0;
		draw_us = 0;
		last_us = now;
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

	if (done || boot_choice > 1)
		return;
	if (!started) {
		if (now < next_scan)
			return;
		next_scan = now + 200000;
		if (!dos_prompt_visible(pc))
			return;
		cmd = boot_menu_cmd(boot_choice);
		if (!cmd) {
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
	char ssid[16];
	char pass[32];
	int enable_usb;
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

/* Insert cdb into an existing ini without disturbing its other settings. */
static void add_cdb_to_ini(void)
{
	FILE *f = fopen("/sdcard/tiny386.ini", "rb");
	if (!f)
		return;
	char cur[512];
	int n = fread(cur, 1, sizeof(cur) - 1, f);
	fclose(f);
	if (n < 0)
		n = 0;
	cur[n] = 0;
	if (strstr(cur, "hdc") && strstr(cur, "cdb"))
		return;
	FILE *iso = fopen("/sdcard/paldos.iso", "rb");
	if (!iso) {
		fprintf(stderr, "paldos.iso missing: PAL CD check will fail\n");
		return;
	}
	fclose(iso);
	FILE *b = fopen("/sdcard/tiny386.bak", "wb");
	if (b) {
		fwrite(cur, 1, n, b);
		fclose(b);
	}
	static const char ini[] =
		"[pc]\n"
		"bios = /sdcard/bios.bin\n"
		"vga_bios = /sdcard/vgabios.bin\n"
		"mem_size = 4M\n"
		"vga_mem_size = 768K\n"
		"hda = /sdcard/dos_fast.img\n"
		"hdc = /sdcard/games.img\n"
		"cdb = /sdcard/paldos.iso\n"
		"fill_cmos = 1\n"
		"vga_force_8dm = 1\n"
		"\n[display]\n"
		"width = 640\n"
		"height = 480\n"
		"\n[cpu]\n"
		"gen = 3\n"
		"fpu = 0\n";
	FILE *w = fopen("/sdcard/tiny386.ini", "wb");
	if (!w) {
		perror("tiny386.ini");
		return;
	}
	fputs(ini, w);
	fclose(w);
	fprintf(stderr, "tiny386.ini: PAL layout hdc=D:, cdb=CD\n");
}

static int parse_ini(void* user, const char* section,
		     const char* name, const char* value)
{
	struct esp_ini_config *conf = user;
#define SEC(a) (strcmp(section, a) == 0)
#define NAME(a) (strcmp(name, a) == 0)
	if (SEC("esp")) {
		if (NAME("ssid")) {
			if (strlen(value) < 32)
				strcpy(conf->ssid, value);
		} else if (NAME("pass")) {
			if (strlen(value) < 64)
				strcpy(conf->pass, value);
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
	add_cdb_to_ini();

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

	if (config.ssid[0]) {
		wifi_main(config.ssid, config.pass);
	}

	if (psram) {
		xTaskCreatePinnedToCore(i386_task, "i386_main", 8192, &config, 3, NULL, 1);
		xTaskCreatePinnedToCore(vga_task, "vga_task", 8192, NULL, 0, NULL, 0);
	}
}
