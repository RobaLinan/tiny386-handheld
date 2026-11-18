/* Small file console on USB-Serial/JTAG.
 *
 * It runs from boot until the emulator starts, so the DOS serial keyboard
 * (term.c) owns the port afterwards.  The host side is tools/sdctl.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"

#include "sdcli.h"
#include "../../ini.h"

#if defined(USE_USB_SERIAL_KBD) || defined(USE_LCD_SERIAL)

static const char *TAG = "sdcli";
static volatile int s_run;
static TaskHandle_t s_task;

static void cli_tx(const void *data, size_t len)
{
	const uint8_t *p = data;

	while (len) {
		int n = usb_serial_jtag_write_bytes(p, len,
						   pdMS_TO_TICKS(500));

		if (n <= 0)
			return;
		p += n;
		len -= n;
	}
}

static void cli_puts(const char *s)
{
	cli_tx(s, strlen(s));
}

static void cli_printf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	cli_puts(buf);
}

static bool sd_path(const char *arg, char *out, size_t out_len)
{
	if (!arg || !arg[0] || strstr(arg, "..") || strchr(arg, '\\'))
		return false;
	if (arg[0] == '/')
		snprintf(out, out_len, "/sdcard%s", arg);
	else
		snprintf(out, out_len, "/sdcard/%s", arg);
	return true;
}

static int ini_check_cb(void *user, const char *section, const char *name,
			const char *value)
{
	(void) name;
	(void) value;
	if (!strcmp(section, "pc"))
		*(bool *) user = true;
	return 1;
}

static void cmd_ls(const char *arg)
{
	char path[320];
	DIR *dir;
	struct dirent *ent;

	if (!arg || !arg[0])
		arg = "/";
	if (!sd_path(arg, path, sizeof(path))) {
		cli_puts("ERR bad path\r\n");
		return;
	}
	dir = opendir(path);
	if (!dir) {
		cli_puts("ERR open failed\r\n");
		return;
	}
	while ((ent = readdir(dir)) != NULL) {
		char full[640];
		struct stat st;

		if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
			continue;
		snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
		if (stat(full, &st) != 0)
			continue;
		cli_printf("%c %10ld %s\r\n", S_ISDIR(st.st_mode) ? 'd' : 'f',
			   (long) st.st_size, ent->d_name);
	}
	closedir(dir);
}

static void cmd_cat(const char *arg)
{
	char path[320];
	char buf[512];
	FILE *f;
	size_t n;
	size_t total = 0;

	if (!sd_path(arg, path, sizeof(path))) {
		cli_puts("ERR bad path\r\n");
		return;
	}
	f = fopen(path, "rb");
	if (!f) {
		cli_puts("ERR open failed\r\n");
		return;
	}
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && s_run) {
		total += n;
		if (total > 65536) {
			cli_puts("\r\nERR file too large for cat\r\n");
			break;
		}
		cli_tx(buf, n);
	}
	fclose(f);
	cli_puts("\r\n");
}

static void cmd_get(const char *arg)
{
	char path[320];
	char buf[1024];
	struct stat st;
	FILE *f;
	size_t n;
	long sent = 0;

	if (!sd_path(arg, path, sizeof(path)) || stat(path, &st) != 0 ||
	    S_ISDIR(st.st_mode)) {
		cli_puts("ERR not found\r\n");
		return;
	}
	f = fopen(path, "rb");
	if (!f) {
		cli_puts("ERR open failed\r\n");
		return;
	}
	cli_printf("OK %ld\r\n", (long) st.st_size);
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && s_run) {
		cli_tx(buf, n);
		sent += n;
	}
	fclose(f);
	cli_puts("\r\nEND\r\n");
	if (sent != st.st_size)
		cli_puts("ERR short read\r\n");
}

static void cmd_put(const char *arg)
{
	char path[320];
	char bak[328];
	char part[328];
	char buf[1024];
	char *p = (char *) arg;
	char *size_str;
	unsigned long size;
	unsigned long got = 0;
	uint64_t total = 0, free_bytes = 0;
	FILE *f;
	bool failed = false;
	bool existed;

	while (*p == ' ')
		p++;
	size_str = strchr(p, ' ');
	if (!size_str) {
		cli_puts("ERR usage: put <path> <size>\r\n");
		return;
	}
	*size_str++ = 0;
	while (*size_str == ' ')
		size_str++;
	size = strtoul(size_str, NULL, 10);
	if (size == 0 || !sd_path(p, path, sizeof(path))) {
		cli_puts("ERR bad size/path\r\n");
		return;
	}
	esp_vfs_fat_info("/sdcard", &total, &free_bytes);
	if (size > free_bytes) {
		cli_puts("ERR not enough space\r\n");
		return;
	}
	snprintf(part, sizeof(part), "%s.part", path);
	snprintf(bak, sizeof(bak), "%s.bak", path);
	f = fopen(part, "wb");
	if (!f) {
		cli_puts("ERR create failed\r\n");
		return;
	}
	cli_puts("READY\r\n");
	while (got < size && s_run) {
		uint32_t want = size - got > sizeof(buf) ? sizeof(buf) :
				(uint32_t) (size - got);
		int n = usb_serial_jtag_read_bytes(buf, want,
						   pdMS_TO_TICKS(500));

		if (n <= 0)
			continue;
		if (fwrite(buf, 1, n, f) != (size_t) n) {
			failed = true;
			break;
		}
		got += n;
	}
	fflush(f);
	fsync(fileno(f));
	if (fclose(f) != 0)
		failed = true;
	if (failed || got != size) {
		remove(part);
		cli_puts("ERR transfer failed\r\n");
		return;
	}
	if (!strcmp(strrchr(path, '/') + 1, "tiny386.ini")) {
		bool has_pc = false;

		if (ini_parse(part, ini_check_cb, &has_pc) != 0 || !has_pc) {
			remove(part);
			cli_puts("ERR invalid ini (missing [pc])\r\n");
			return;
		}
	}
	{
		struct stat st;

		existed = stat(path, &st) == 0;
	}
	if (existed) {
		remove(bak);
		if (rename(path, bak) != 0) {
			remove(part);
			cli_puts("ERR backup failed\r\n");
			return;
		}
	}
	if (rename(part, path) != 0) {
		if (existed)
			rename(bak, path);
		remove(part);
		cli_puts("ERR replace failed\r\n");
		return;
	}
	cli_printf("OK %lu\r\n", size);
	ESP_LOGI(TAG, "put %s (%lu bytes)", path, size);
}

static void cmd_rm(const char *arg)
{
	char path[320];

	if (!sd_path(arg, path, sizeof(path)) || remove(path) != 0) {
		cli_puts("ERR remove failed\r\n");
		return;
	}
	cli_puts("OK\r\n");
}

static void cmd_free(void)
{
	uint64_t total = 0, free_bytes = 0;

	if (esp_vfs_fat_info("/sdcard", &total, &free_bytes) == ESP_OK)
		cli_printf("OK free=%llu total=%llu\r\n",
			   (unsigned long long) free_bytes,
			   (unsigned long long) total);
	else
		cli_puts("ERR not mounted\r\n");
}

static void handle_line(char *line)
{
	char *arg;
	char *sp;

	while (*line == ' ')
		line++;
	sp = strchr(line, ' ');
	if (sp) {
		*sp++ = 0;
		while (*sp == ' ')
			sp++;
		arg = sp;
	} else {
		arg = line + strlen(line);
	}
	if (!strcmp(line, "ls"))
		cmd_ls(arg);
	else if (!strcmp(line, "cat"))
		cmd_cat(arg);
	else if (!strcmp(line, "get"))
		cmd_get(arg);
	else if (!strcmp(line, "put"))
		cmd_put(arg);
	else if (!strcmp(line, "rm"))
		cmd_rm(arg);
	else if (!strcmp(line, "free"))
		cmd_free();
	else if (!strcmp(line, "reboot")) {
		cli_puts("OK reboot\r\n");
		vTaskDelay(pdMS_TO_TICKS(300));
		esp_restart();
	} else if (!strcmp(line, "help")) {
		cli_puts("ls [dir] | cat <file> | get <file> | put <file> <size>\r\n"
			 "rm <file> | free | reboot | help\r\n");
	} else if (!strcmp(line, "")) {
		/* prompt again */
	} else {
		cli_puts("ERR unknown command (help)\r\n");
	}
}

static void cli_task(void *arg)
{
	char line[160];
	int n = 0;

	(void) arg;
	cli_puts("\r\nTiny386 SD file console. Type help.\r\nsd> ");
	while (s_run) {
		char c;
		int r = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(20));

		if (r != 1)
			continue;
		if (c == '\r' || c == '\n') {
			cli_puts("\r\n");
			if (n) {
				line[n] = 0;
				handle_line(line);
				n = 0;
			}
			if (s_run)
				cli_puts("sd> ");
		} else if (c == 8 || c == 127) {
			if (n) {
				n--;
				cli_puts("\b \b");
			}
		} else if (n < (int) sizeof(line) - 1) {
			line[n++] = c;
			cli_tx(&c, 1);
		}
	}
	s_task = NULL;
	vTaskDelete(NULL);
}

void sdcli_start(void)
{
	if (s_run)
		return;
	s_run = 1;
	if (xTaskCreate(cli_task, "sdcli", 12288, NULL, 1, &s_task) != pdPASS) {
		s_run = 0;
		s_task = NULL;
		ESP_LOGE(TAG, "cannot start");
	}
}

void sdcli_stop(void)
{
	int waited = 0;

	if (!s_task)
		return;
	s_run = 0;
	while (s_task && waited < 200) {
		vTaskDelay(pdMS_TO_TICKS(10));
		waited++;
	}
	if (s_task) {
		vTaskDelete(s_task);
		s_task = NULL;
	}
}

#else

void sdcli_start(void) {}
void sdcli_stop(void) {}

#endif
