#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"

#include "common.h"
#include "websrv.h"
#include "wifi.h"
#include "../../ini.h"

static const char *TAG = "websrv";
static httpd_handle_t s_server;
static char s_pass[65];

extern const uint8_t _binary_web_index_html_start[];
extern const uint8_t _binary_web_index_html_end[];

/* ---------------- authentication ---------------- */

static int b64_val(int c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

static int b64_decode(const char *src, char *dst, size_t dst_len)
{
	unsigned int acc = 0;
	int bits = 0;
	size_t o = 0;

	for (; *src && *src != '='; src++) {
		int v = b64_val((unsigned char) *src);

		if (v < 0)
			continue;
		acc = ((acc << 6) | (unsigned int) v) & 0xffffff;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (o + 1 < dst_len)
				dst[o++] = (char) ((acc >> bits) & 0xff);
		}
	}
	if (dst_len)
		dst[o] = 0;
	return (int) o;
}

static bool authorized(httpd_req_t *req)
{
	char hdr[192];
	char dec[160];
	char *colon;
	int n;

	if (!s_pass[0])
		return true;
	if (httpd_req_get_hdr_value_str(req, "Authorization", hdr,
					sizeof(hdr)) != ESP_OK)
		return false;
	if (strncasecmp(hdr, "Basic ", 6) != 0)
		return false;
	n = b64_decode(hdr + 6, dec, sizeof(dec));
	if (n <= 0)
		return false;
	colon = strchr(dec, ':');
	if (!colon)
		return false;
	return strcmp(colon + 1, s_pass) == 0;
}

static bool require_auth(httpd_req_t *req)
{
	if (authorized(req))
		return true;
	httpd_resp_set_status(req, "401 Unauthorized");
	httpd_resp_set_hdr(req, "WWW-Authenticate",
			   "Basic realm=\"Tiny386\"");
	httpd_resp_sendstr(req, "authentication required\n");
	return false;
}

static esp_err_t send_status(httpd_req_t *req, const char *status,
			     const char *body)
{
	httpd_resp_set_status(req, status);
	httpd_resp_set_type(req, "text/plain; charset=utf-8");
	return httpd_resp_sendstr(req, body);
}

/* ---------------- paths and JSON ---------------- */

static int hex_val(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static void url_decode(char *s)
{
	char *o = s;

	for (char *p = s; *p; p++) {
		if (*p == '+') {
			*o++ = ' ';
		} else if (*p == '%' && p[1] && p[2]) {
			int hi = hex_val((unsigned char) p[1]);
			int lo = hex_val((unsigned char) p[2]);

			if (hi >= 0 && lo >= 0) {
				*o++ = (char) (hi * 16 + lo);
				p += 2;
			} else {
				*o++ = *p;
			}
		} else {
			*o++ = *p;
		}
	}
	*o = 0;
}

static bool get_query(httpd_req_t *req, const char *key, char *out,
		      size_t out_len)
{
	char query[512];
	char value[256];

	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
		return false;
	if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK)
		return false;
	snprintf(out, out_len, "%s", value);
	url_decode(out);
	return true;
}

/* /menu/a.ini -> /sdcard/menu/a.ini */
static bool resolve_path(const char *rel, char *out, size_t out_len)
{
	if (!rel || rel[0] != '/')
		return false;
	if (strstr(rel, "..") || strchr(rel, '\\'))
		return false;
	if (strlen(rel) > 240)
		return false;
	if (!strcmp(rel, "/"))
		snprintf(out, out_len, "/sdcard");
	else
		snprintf(out, out_len, "/sdcard%s", rel);
	return true;
}

static void json_escape(const char *s, char *out, size_t out_len)
{
	size_t o = 0;

	for (; *s && o + 7 < out_len; s++) {
		unsigned char c = (unsigned char) *s;

		if (c == '"' || c == '\\') {
			out[o++] = '\\';
			out[o++] = (char) c;
		} else if (c < 0x20 || c >= 0x80) {
			o += snprintf(out + o, out_len - o, "\\u%04x", c);
		} else {
			out[o++] = (char) c;
		}
	}
	out[o] = 0;
}

/* ---------------- handlers ---------------- */

static esp_err_t root_get(httpd_req_t *req)
{
	size_t len;

	if (!require_auth(req))
		return ESP_OK;
	len = _binary_web_index_html_end - _binary_web_index_html_start;
	httpd_resp_set_type(req, "text/html; charset=utf-8");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	return httpd_resp_send(req, (const char *) _binary_web_index_html_start,
			       len);
}

static esp_err_t favicon_get(httpd_req_t *req)
{
	httpd_resp_set_status(req, "204 No Content");
	return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_get(httpd_req_t *req)
{
	char wifi[192];
	char e_wifi[768];
	char json[1024];
	uint64_t total = 0, free_bytes = 0;

	if (!require_auth(req))
		return ESP_OK;
	wifi_status_line(wifi, sizeof(wifi));
	json_escape(wifi, e_wifi, sizeof(e_wifi));
	esp_vfs_fat_info("/sdcard", &total, &free_bytes);
	snprintf(json, sizeof(json),
		 "{\"wifi\":\"%s\",\"free\":%llu,\"total\":%llu,"
		 "\"writable\":%s}",
		 e_wifi, (unsigned long long) free_bytes,
		 (unsigned long long) total,
		 globals.pc ? "false" : "true");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	return httpd_resp_sendstr(req, json);
}

static esp_err_t list_get(httpd_req_t *req)
{
	char rel[256] = "/";
	char dir_path[320];
	char epath[1536];
	char hdr[1792];
	DIR *dir;
	struct dirent *ent;
	bool first = true;
	esp_err_t err = ESP_OK;

	if (!require_auth(req))
		return ESP_OK;
	get_query(req, "path", rel, sizeof(rel));
	if (!resolve_path(rel, dir_path, sizeof(dir_path))) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path\n");
		return ESP_OK;
	}
	dir = opendir(dir_path);
	if (!dir) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found\n");
		return ESP_OK;
	}

	json_escape(rel, epath, sizeof(epath));
	snprintf(hdr, sizeof(hdr), "{\"path\":\"%s\",\"entries\":[", epath);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	if (httpd_resp_send_chunk(req, hdr, strlen(hdr)) != ESP_OK) {
		closedir(dir);
		return ESP_OK;
	}

	while ((ent = readdir(dir)) != NULL) {
		char full[640];
		char ename[1600];
		char item[1792];
		struct stat st;
		int n;

		if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
			continue;
		if (strlen(rel) + strlen(ent->d_name) + 2 >= sizeof(full))
			continue;
		if (!strcmp(rel, "/"))
			snprintf(full, sizeof(full), "/sdcard/%s", ent->d_name);
		else
			snprintf(full, sizeof(full), "/sdcard%s/%s", rel,
				 ent->d_name);
		if (stat(full, &st) != 0)
			continue;
		json_escape(ent->d_name, ename, sizeof(ename));
		n = snprintf(item, sizeof(item),
			     "%s{\"name\":\"%s\",\"size\":%ld,\"dir\":%d}",
			     first ? "" : ",", ename, (long) st.st_size,
			     S_ISDIR(st.st_mode) ? 1 : 0);
		if (n < 0)
			continue;
		if (n >= (int) sizeof(item))
			n = sizeof(item) - 1;
		first = false;
		if (httpd_resp_send_chunk(req, item, n) != ESP_OK) {
			err = ESP_FAIL;
			break;
		}
	}
	closedir(dir);
	if (err == ESP_OK) {
		httpd_resp_send_chunk(req, "]}", 2);
		httpd_resp_send_chunk(req, NULL, 0);
	}
	return ESP_OK;
}

static void safe_filename(const char *path, char *out, size_t out_len)
{
	const char *base = strrchr(path, '/');
	size_t o = 0;

	base = base ? base + 1 : path;
	for (; *base && o + 1 < out_len; base++) {
		char c = *base;

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
			out[o++] = c;
		else
			out[o++] = '_';
	}
	if (!o)
		snprintf(out, out_len, "file.bin");
	else
		out[o] = 0;
}

static esp_err_t download_get(httpd_req_t *req)
{
	char rel[256] = "/";
	char path[320];
	char query[512];
	char text[8] = "";
	struct stat st;
	FILE *f;
	bool as_text = false;

	if (!require_auth(req))
		return ESP_OK;
	get_query(req, "path", rel, sizeof(rel));
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
		httpd_query_key_value(query, "text", text, sizeof(text));
	as_text = text[0] == '1';
	if (!resolve_path(rel, path, sizeof(path))) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path\n");
		return ESP_OK;
	}
	if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found\n");
		return ESP_OK;
	}
	f = fopen(path, "rb");
	if (!f) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
				    "open failed\n");
		return ESP_OK;
	}

	if (as_text) {
		long size;

		fseek(f, 0, SEEK_END);
		size = ftell(f);
		rewind(f);
		if (size < 0 || size > 65536) {
			fclose(f);
			httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE,
					    "text file too large\n");
			return ESP_OK;
		}
		char *buf = malloc((size_t) size + 1);

		if (!buf) {
			fclose(f);
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
					    "no memory\n");
			return ESP_OK;
		}
		size_t n = fread(buf, 1, (size_t) size, f);

		fclose(f);
		buf[n] = 0;
		httpd_resp_set_type(req, "text/plain; charset=utf-8");
		httpd_resp_set_hdr(req, "Cache-Control", "no-store");
		httpd_resp_send(req, buf, n);
		free(buf);
		return ESP_OK;
	}

	char name[128];
	char disp[192];
	char buf[4096];
	size_t n;

	safe_filename(path, name, sizeof(name));
	snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
	httpd_resp_set_type(req, "application/octet-stream");
	httpd_resp_set_hdr(req, "Content-Disposition", disp);
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		if (httpd_resp_send_chunk(req, buf, n) != ESP_OK)
			break;
	}
	fclose(f);
	httpd_resp_send_chunk(req, NULL, 0);
	return ESP_OK;
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

static esp_err_t upload_put(httpd_req_t *req)
{
	char rel[256];
	char path[320];
	char part[328];
	char bak[328];
	char len_str[32];
	struct stat st;
	bool existed;
	unsigned long long len, total_len;
	uint64_t total = 0, free_bytes = 0;
	char *buf = NULL;
	FILE *f = NULL;
	bool failed = false;
	int timeouts = 0;

	if (!require_auth(req))
		return ESP_OK;
	if (globals.pc)
		return send_status(req, "409 Conflict",
				   "game is running; reboot to manage files\n");
	if (!get_query(req, "path", rel, sizeof(rel)) ||
	    !resolve_path(rel, path, sizeof(path)) || !strcmp(rel, "/") ||
	    rel[strlen(rel) - 1] == '/') {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path\n");
		return ESP_OK;
	}
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
		return send_status(req, "409 Conflict", "is a directory\n");
	if (httpd_req_get_hdr_value_str(req, "Content-Length", len_str,
					sizeof(len_str)) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_411_LENGTH_REQUIRED,
				    "content length required\n");
		return ESP_OK;
	}
	len = strtoull(len_str, NULL, 10);
	total_len = len;
	esp_vfs_fat_info("/sdcard", &total, &free_bytes);
	if (len > free_bytes)
		return send_status(req, "507 Insufficient Storage",
				   "not enough free space\n");
	if (!strcmp(strrchr(path, '/') + 1, "tiny386.ini") &&
	    len > 1024 * 1024) {
		httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE,
				    "ini too large\n");
		return ESP_OK;
	}

	snprintf(part, sizeof(part), "%s.part", path);
	snprintf(bak, sizeof(bak), "%s.bak", path);
	f = fopen(part, "wb");
	if (!f) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
				    "cannot create file\n");
		return ESP_OK;
	}
	buf = malloc(8192);
	if (!buf) {
		fclose(f);
		remove(part);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
				    "no memory\n");
		return ESP_OK;
	}

	while (len > 0) {
		size_t want = len > 8192 ? 8192 : (size_t) len;
		int n = httpd_req_recv(req, buf, want);

		if (n == HTTPD_SOCK_ERR_TIMEOUT) {
			if (++timeouts > 5) {
				failed = true;
				break;
			}
			continue;
		}
		if (n <= 0) {
			failed = true;
			break;
		}
		timeouts = 0;
		if (fwrite(buf, 1, (size_t) n, f) != (size_t) n) {
			failed = true;
			break;
		}
		len -= (unsigned long long) n;
	}
	free(buf);
	if (f) {
		fflush(f);
		fsync(fileno(f));
		if (fclose(f) != 0)
			failed = true;
	}
	if (failed) {
		remove(part);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
				    "upload failed\n");
		return ESP_OK;
	}

	if (!strcmp(strrchr(path, '/') + 1, "tiny386.ini")) {
		bool has_pc = false;

		if (ini_parse(part, ini_check_cb, &has_pc) != 0 || !has_pc) {
			remove(part);
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
					    "invalid ini (missing [pc] or syntax error)\n");
			return ESP_OK;
		}
	}

	existed = stat(path, &st) == 0;
	if (existed) {
		remove(bak);
		if (rename(path, bak) != 0) {
			remove(part);
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
					    "cannot replace file\n");
			return ESP_OK;
		}
	}
	if (rename(part, path) != 0) {
		if (existed)
			rename(bak, path);
		remove(part);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
				    "cannot replace file\n");
		return ESP_OK;
	}

	ESP_LOGI(TAG, "uploaded %s (%llu bytes)", path, total_len);
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t delete_delete(httpd_req_t *req)
{
	char rel[256];
	char path[320];
	struct stat st;

	if (!require_auth(req))
		return ESP_OK;
	if (globals.pc)
		return send_status(req, "409 Conflict",
				   "game is running; reboot to manage files\n");
	if (!get_query(req, "path", rel, sizeof(rel)) ||
	    !resolve_path(rel, path, sizeof(path)) || !strcmp(rel, "/")) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path\n");
		return ESP_OK;
	}
	if (stat(path, &st) != 0) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found\n");
		return ESP_OK;
	}
	if (remove(path) != 0) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
				    "delete failed (directory not empty?)\n");
		return ESP_OK;
	}
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t reboot_post(httpd_req_t *req)
{
	if (!require_auth(req))
		return ESP_OK;
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, "{\"ok\":true}");
	vTaskDelay(pdMS_TO_TICKS(300));
	esp_restart();
	return ESP_OK;
}

/* ---------------- registration ---------------- */

static const httpd_uri_t s_uris[] = {
	{ .uri = "/", .method = HTTP_GET, .handler = root_get },
	{ .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get },
	{ .uri = "/api/list", .method = HTTP_GET, .handler = list_get },
	{ .uri = "/api/download", .method = HTTP_GET, .handler = download_get },
	{ .uri = "/api/upload", .method = HTTP_PUT, .handler = upload_put },
	{ .uri = "/api/delete", .method = HTTP_DELETE, .handler = delete_delete },
	{ .uri = "/api/status", .method = HTTP_GET, .handler = status_get },
	{ .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post },
};

bool websrv_start(const char *password)
{
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	size_t i;

	if (s_server)
		return true;
	snprintf(s_pass, sizeof(s_pass), "%s", password ? password : "");
	config.max_uri_handlers = 10;
	/* list_get needs ~8KB of path/name buffers; leave headroom for the
	 * HTTP parser and socket calls. */
	config.stack_size = 16384;
	config.lru_purge_enable = true;
	config.recv_wait_timeout = 15;
	config.send_wait_timeout = 15;

	if (httpd_start(&s_server, &config) != ESP_OK) {
		ESP_LOGE(TAG, "httpd_start failed");
		s_server = NULL;
		return false;
	}
	for (i = 0; i < sizeof(s_uris) / sizeof(s_uris[0]); i++) {
		if (httpd_register_uri_handler(s_server, &s_uris[i]) != ESP_OK)
			ESP_LOGW(TAG, "register %s failed", s_uris[i].uri);
	}
	ESP_LOGI(TAG, "file manager on port 80");
	return true;
}

void websrv_stop(void)
{
	if (!s_server)
		return;
	httpd_stop(s_server);
	s_server = NULL;
	ESP_LOGI(TAG, "file manager stopped");
}

bool websrv_running(void)
{
	return s_server != NULL;
}
