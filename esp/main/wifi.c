/* Simple WiFi Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include <sys/socket.h>

#include "common.h"
#include "wifi.h"
#include "websrv.h"
#include "../../i8042.h"

/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_MAX_STA_CONN       CONFIG_MAX_STA_CONN

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define EXAMPLE_H2E_IDENTIFIER ""

static const char *TAG = "simple wifi";

void (*_Atomic esp32_send_packet)(uint8_t *buf, int size);

#ifndef ESP_IF_WIFI_STA
#define ESP_IF_WIFI_STA WIFI_IF_STA
#endif
static void send_packet(uint8_t *buf, int size)
{
	esp_wifi_internal_tx(ESP_IF_WIFI_STA, buf, size);
}

static void event_handler(void* arg, esp_event_base_t event_base,
			  int32_t event_id, void* event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		esp_wifi_connect();
		ESP_LOGI(TAG, "retry to connect to the AP");
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
		ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
	}

	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		atomic_store_explicit(&esp32_send_packet, NULL,
				      memory_order_relaxed);
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
		atomic_store_explicit(&esp32_send_packet, send_packet,
				      memory_order_relaxed);
	}
}

#ifdef USE_HOSTED_WIFI
#include "esp_hosted.h"
#endif

void wifi_init_sta(const char *ssid, const char *pass)
{
	s_wifi_event_group = xEventGroupCreate();

	ESP_ERROR_CHECK(esp_netif_init());

#ifdef USE_HOSTED_WIFI
	ESP_LOGI(TAG, "init ESP-Hosted");
	esp_hosted_init();
	esp_hosted_connect_to_slave();
#endif

	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	esp_event_handler_instance_t instance_any_id;
	esp_event_handler_instance_t instance_got_ip;
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
							    ESP_EVENT_ANY_ID,
							    &event_handler,
							    NULL,
							    &instance_any_id));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
							    IP_EVENT_STA_GOT_IP,
							    &event_handler,
							    NULL,
							    &instance_got_ip));

	wifi_config_t wifi_config = {
		.sta = {
			/* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (pasword len => 8).
			 * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
			 * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
			 * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
			 */
			.threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
			.sae_pwe_h2e = ESP_WIFI_SAE_MODE,
			.sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
		},
	};
	if (strlen(ssid) < 32 && strlen(pass) < 64) {
		strcpy((char *) wifi_config.sta.ssid, ssid);
		strcpy((char *) wifi_config.sta.password, pass);
	} else {
		ESP_LOGE(TAG, "ssid or password is too long");
		return;
	}
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
	ESP_ERROR_CHECK(esp_wifi_start() );

	ESP_LOGI(TAG, "wifi_init_sta finished.");

	/* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
	 * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
	EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
					       WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
					       pdFALSE,
					       pdFALSE,
					       portMAX_DELAY);

	/* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
	 * happened. */
	if (bits & WIFI_CONNECTED_BIT) {
		ESP_LOGI(TAG, "connected to ap SSID:%s", ssid);
	} else if (bits & WIFI_FAIL_BIT) {
		ESP_LOGI(TAG, "Failed to connect to SSID:%s", ssid);
	} else {
		ESP_LOGE(TAG, "UNEXPECTED EVENT");
	}
}

static void __attribute__((noreturn)) task_fatal_error()
{
	ESP_LOGE(TAG, "Exiting task due to fatal error...");
	(void)vTaskDelete(NULL);

	while (1) {
		;
	}
}

static void server_thread(void *pvParameter)
{
	struct sockaddr_in sock_info;

	int sfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sfd == -1) {
		ESP_LOGE(TAG, "Create socket failed!");
		return;
	}

	memset(&sock_info, 0, sizeof(struct sockaddr_in));
	sock_info.sin_family = AF_INET;
	sock_info.sin_addr.s_addr = htonl(INADDR_ANY);
	sock_info.sin_port = htons(9999);

	if(bind(sfd, (struct sockaddr *)&sock_info, sizeof(sock_info)) < 0)
		task_fatal_error();

	if(listen(sfd, 10) < 0)
		task_fatal_error();

	for (;;) {
		static int state;
		static int8_t mouse[4];

		int socket_id = accept(sfd, NULL, 0);
		state = 0;
		xEventGroupWaitBits(global_event_group,
				    BIT0,
				    pdFALSE,
				    pdFALSE,
				    portMAX_DELAY);

		bool flag = true;
		char text[16];
		while (flag) {
			int buff_len = recv(socket_id, text, 16, 0);
			if (buff_len < 0) { /*receive error*/
				ESP_LOGE(TAG, "Error: receive data error! errno=%d", errno);
				flag = false;
				close(socket_id);
			} else if (buff_len > 0) { /*deal with response body*/
				for (int i = 0; i < buff_len; i++) {
					if (state == 0) {
						if (text[i] == 0) {
							state = 4;
						} else {
							int is_down = !(text[i] >> 7);
							int keycode = text[i] & 0x7f;
							ps2_put_keycode(globals.kbd, is_down, keycode);
							vTaskDelay(5 / portTICK_PERIOD_MS);
						}
					} else {
						mouse[4 - state] = text[i];
						if (state == 1) {
							state = 0;
							ps2_mouse_event(globals.mouse,
									mouse[0], mouse[1], mouse[2], mouse[3]);
							vTaskDelay(5 / portTICK_PERIOD_MS);
						} else {
							state--;
						}
					}
				}
			} else if (buff_len == 0) {  /*packet over*/
				ESP_LOGI(TAG, "Connection closed, all packets received");
				flag = false;
				close(socket_id);
			}
		}
	}
}

void wifi_main(const char *ssid, const char *pass)
{
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);
	wifi_init_sta(ssid, pass);
	xTaskCreatePinnedToCore(server_thread, "wifi", 4096, NULL, 1, NULL, 0);
}

/* ------------------------------------------------------------------
 * Boot-menu WiFi
 *
 * On the handheld we only want WiFi while the game menu is on screen:
 * esp_hosted + the C6 cost power and internal RAM, and the emulator
 * needs both.  vga_task calls wifi_menu_start() before boot_menu() and
 * wifi_menu_stop() after a game has been picked.
 * ------------------------------------------------------------------ */

#define MENU_WEB_DEFAULT_PASS "tiny386pda"

static struct {
	char ssid[33];
	char pass[65];
	char web_pass[65];
	int web;
} s_menu_cfg;
static int s_menu_cfg_ok;
static volatile int s_menu_active;
static volatile int s_menu_state;   /* 0 off, 1 starting, 2 up, 3 failed */
static int s_wifi_inited;
static int s_default_pass;
static int s_sta_ok;
static char s_sta_ip[16];
static char s_ap_ssid[33];
static char s_ap_ip[16];
static SemaphoreHandle_t s_menu_lock;
static esp_timer_handle_t s_retry_timer;
static int s_retry_ms = 2000;

void wifi_configure(const char *ssid, const char *pass, const char *web_pass,
		    int web_enabled)
{
	memset(&s_menu_cfg, 0, sizeof(s_menu_cfg));
	snprintf(s_menu_cfg.ssid, sizeof(s_menu_cfg.ssid), "%s",
		 ssid ? ssid : "");
	snprintf(s_menu_cfg.pass, sizeof(s_menu_cfg.pass), "%s",
		 pass ? pass : "");
	snprintf(s_menu_cfg.web_pass, sizeof(s_menu_cfg.web_pass), "%s",
		 web_pass ? web_pass : "");
	s_menu_cfg.web = web_enabled;
	s_default_pass = 0;
	if (strlen(s_menu_cfg.web_pass) < 8) {
		snprintf(s_menu_cfg.web_pass, sizeof(s_menu_cfg.web_pass), "%s",
			 MENU_WEB_DEFAULT_PASS);
		s_default_pass = 1;
	}
	s_menu_cfg_ok = 1;
}

void wifi_status_line(char *buf, size_t len)
{
	int state = s_menu_state;
	int sta = 0;
	char ip[16] = "";
	char ap[33] = "";
	char ap_ip[16] = "";
	int def = 0;
	int web = s_menu_cfg.web;

	if (!buf || !len)
		return;
	if (!s_menu_cfg_ok) {
		snprintf(buf, len, "WiFi: off");
		return;
	}
	if (!web) {
		snprintf(buf, len, "SD CLI: USB serial (tools/sdctl.py)");
		return;
	}
	if (state == 0) {
		snprintf(buf, len, "WiFi: off");
		return;
	}
	if (state == 1) {
		snprintf(buf, len, "WiFi starting...");
		return;
	}
	if (state == 3) {
		snprintf(buf, len, "WiFi: unavailable");
		return;
	}
	if (s_menu_lock) {
		xSemaphoreTake(s_menu_lock, portMAX_DELAY);
		sta = s_sta_ok;
		snprintf(ip, sizeof(ip), "%s", s_sta_ip);
		snprintf(ap, sizeof(ap), "%s", s_ap_ssid);
		snprintf(ap_ip, sizeof(ap_ip), "%s", s_ap_ip);
		def = s_default_pass;
		xSemaphoreGive(s_menu_lock);
	}
	if (sta && ap[0] && def)
		snprintf(buf, len, "WiFi %s  http://%s  | AP %s pw %s", ip, ip,
			 ap, MENU_WEB_DEFAULT_PASS);
	else if (sta && ap[0])
		snprintf(buf, len, "WiFi %s  http://%s  | AP %s", ip, ip, ap);
	else if (sta)
		snprintf(buf, len, "WiFi %s  http://%s", ip, ip);
	else if (ap[0] && def)
		snprintf(buf, len, "AP %s  http://%s  pw %s", ap,
			 ap_ip[0] ? ap_ip : "192.168.4.1",
			 MENU_WEB_DEFAULT_PASS);
	else if (ap[0])
		snprintf(buf, len, "AP %s  http://%s", ap,
			 ap_ip[0] ? ap_ip : "192.168.4.1");
	else
		snprintf(buf, len, "WiFi: connecting...");
}

static void menu_retry_cb(void *arg)
{
	(void) arg;
	if (s_menu_active)
		esp_wifi_connect();
}

static void menu_wifi_event(void *arg, esp_event_base_t base, int32_t id,
			    void *data)
{
	(void) arg;
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
		if (s_menu_active)
			esp_wifi_connect();
	} else if (base == WIFI_EVENT &&
		   id == WIFI_EVENT_STA_DISCONNECTED) {
		if (s_menu_lock) {
			xSemaphoreTake(s_menu_lock, portMAX_DELAY);
			s_sta_ok = 0;
			s_sta_ip[0] = 0;
			xSemaphoreGive(s_menu_lock);
		}
		if (!s_menu_active)
			return;
		s_retry_ms *= 2;
		if (s_retry_ms > 30000)
			s_retry_ms = 30000;
		if (s_retry_timer) {
			esp_timer_stop(s_retry_timer);
			esp_timer_start_once(s_retry_timer,
					     (uint64_t) s_retry_ms * 1000);
		}
	} else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *ev = (ip_event_got_ip_t *) data;

		if (s_menu_lock) {
			xSemaphoreTake(s_menu_lock, portMAX_DELAY);
			snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR,
				 IP2STR(&ev->ip_info.ip));
			s_sta_ok = 1;
			xSemaphoreGive(s_menu_lock);
		}
		s_retry_ms = 2000;
		ESP_LOGI(TAG, "menu wifi got ip " IPSTR,
			 IP2STR(&ev->ip_info.ip));
	}
}

static void menu_wifi_task(void *arg)
{
	esp_err_t err;
	wifi_mode_t mode;
	wifi_config_t ap_cfg;
	wifi_config_t sta_cfg;
	esp_netif_ip_info_t ap_ip;
	uint8_t mac[6] = {0};
	esp_event_handler_instance_t any_id;
	esp_event_handler_instance_t got_ip;

	(void) arg;
	err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
	    err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		nvs_flash_erase();
		err = nvs_flash_init();
	}
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
		goto failed;
	}
	err = esp_netif_init();
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		ESP_LOGE(TAG, "netif init failed: %s", esp_err_to_name(err));
		goto failed;
	}
#ifdef USE_HOSTED_WIFI
	if (esp_hosted_init() != 0) {
		ESP_LOGE(TAG, "esp_hosted_init failed");
		goto failed;
	}
	if (esp_hosted_connect_to_slave() != 0) {
		ESP_LOGE(TAG, "esp_hosted_connect_to_slave failed");
		goto failed;
	}
#endif
	err = esp_event_loop_create_default();
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		ESP_LOGE(TAG, "event loop failed: %s", esp_err_to_name(err));
		goto failed;
	}

	/* ESP_MAC_WIFI_SOFTAP is not reliable before esp_wifi_init() on the
	 * hosted P4 (the suffix changed between boots); the eFuse base MAC is
	 * stable, so the AP name stays Tiny386-XXXX across reboots. */
	if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
		uint32_t r = esp_random();

		mac[4] = (uint8_t) (r >> 8);
		mac[5] = (uint8_t) r;
	}
	snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Tiny386-%02X%02X",
		 mac[4], mac[5]);
	snprintf(s_ap_ip, sizeof(s_ap_ip), "192.168.4.1");

	esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
	if (!ap_netif) {
		ESP_LOGE(TAG, "cannot create AP netif");
		goto failed;
	}
	if (s_menu_cfg.ssid[0] &&
	    !esp_netif_create_default_wifi_sta()) {
		ESP_LOGE(TAG, "cannot create STA netif");
		goto failed;
	}

	wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
	err = esp_wifi_init(&init_cfg);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
		goto failed;
	}
	s_wifi_inited = 1;

	esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
					    menu_wifi_event, NULL, &any_id);
	esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
					    menu_wifi_event, NULL, &got_ip);

	esp_timer_create_args_t timer_args = {
		.callback = menu_retry_cb,
		.name = "wifi_retry",
	};
	if (esp_timer_create(&timer_args, &s_retry_timer) != ESP_OK)
		s_retry_timer = NULL;

	mode = s_menu_cfg.ssid[0] ? WIFI_MODE_APSTA : WIFI_MODE_AP;
	err = esp_wifi_set_mode(mode);
	if (err != ESP_OK && mode == WIFI_MODE_APSTA) {
		ESP_LOGW(TAG, "APSTA unavailable (%s), AP only",
			 esp_err_to_name(err));
		mode = WIFI_MODE_AP;
		err = esp_wifi_set_mode(mode);
	}
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "wifi mode failed: %s", esp_err_to_name(err));
		goto failed;
	}

	memset(&ap_cfg, 0, sizeof(ap_cfg));
	size_t ap_len = strlen(s_ap_ssid);

	if (ap_len > 32)
		ap_len = 32;
	memcpy(ap_cfg.ap.ssid, s_ap_ssid, ap_len);
	ap_cfg.ap.ssid_len = ap_len;
	size_t pass_len = strlen(s_menu_cfg.web_pass);

	if (pass_len > 64)
		pass_len = 64;
	memcpy(ap_cfg.ap.password, s_menu_cfg.web_pass, pass_len);
	ap_cfg.ap.channel = 1;
	ap_cfg.ap.max_connection = 4;
	ap_cfg.ap.authmode = pass_len >= 8 ? WIFI_AUTH_WPA2_PSK :
					    WIFI_AUTH_OPEN;
	err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "AP config failed: %s", esp_err_to_name(err));
		goto failed;
	}

	if (mode == WIFI_MODE_APSTA) {
		memset(&sta_cfg, 0, sizeof(sta_cfg));
		size_t n = strlen(s_menu_cfg.ssid);

		if (n > 32)
			n = 32;
		memcpy(sta_cfg.sta.ssid, s_menu_cfg.ssid, n);
		n = strlen(s_menu_cfg.pass);
		if (n > 64)
			n = 64;
		memcpy(sta_cfg.sta.password, s_menu_cfg.pass, n);
		sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
		sta_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_HUNT_AND_PECK;
		err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "STA config failed: %s",
				 esp_err_to_name(err));
			goto failed;
		}
	}

	err = esp_wifi_start();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
		goto failed;
	}
	if (esp_netif_get_ip_info(ap_netif, &ap_ip) == ESP_OK &&
	    ap_ip.ip.addr) {
		xSemaphoreTake(s_menu_lock, portMAX_DELAY);
		snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&ap_ip.ip));
		xSemaphoreGive(s_menu_lock);
	}

	if (!s_menu_active) {
		/* The menu was dismissed while the C6 was coming up.  Stop
		 * the radio here too, in case wifi_menu_stop() already gave
		 * up waiting for this task. */
		esp_wifi_disconnect();
		vTaskDelay(pdMS_TO_TICKS(100));
		esp_wifi_stop();
		s_menu_state = 2;
		vTaskDelete(NULL);
		return;
	}
	websrv_start(s_menu_cfg.web_pass);
	s_menu_state = 2;
	ESP_LOGI(TAG, "menu wifi up: AP %s, STA %s", s_ap_ssid,
		 s_menu_cfg.ssid[0] ? s_menu_cfg.ssid : "(none)");
	vTaskDelete(NULL);
	return;

failed:
	if (websrv_running())
		websrv_stop();
	s_menu_state = 3;
	vTaskDelete(NULL);
}

void wifi_menu_start(void)
{
	if (!s_menu_cfg_ok || !s_menu_cfg.web) {
		s_menu_state = 0;
		return;
	}
	if (s_menu_state != 0)
		return;
	if (!s_menu_lock)
		s_menu_lock = xSemaphoreCreateMutex();
	s_menu_active = 1;
	s_menu_state = 1;
	s_retry_ms = 2000;
	if (xTaskCreate(menu_wifi_task, "wifi_menu", 8192, NULL, 2, NULL) !=
	    pdPASS) {
		ESP_LOGE(TAG, "cannot start wifi task");
		s_menu_state = 3;
	}
}

void wifi_menu_stop(void)
{
	int waited = 0;

	s_menu_active = 0;
	while (s_menu_state == 1 && waited < 120) {
		vTaskDelay(pdMS_TO_TICKS(50));
		waited++;
	}
	if (s_menu_state == 1) {
		/* esp_hosted is still blocked in connect; tearing WiFi down
		 * underneath that call is unsafe.  Leave it alone and let the
		 * next boot clean up. */
		ESP_LOGW(TAG, "wifi init still busy; leaving it running");
		return;
	}
	if (s_menu_state == 0)
		return;
	s_menu_state = 3;
	if (s_retry_timer) {
		esp_timer_stop(s_retry_timer);
		esp_timer_delete(s_retry_timer);
		s_retry_timer = NULL;
	}
	websrv_stop();
	if (s_wifi_inited) {
		esp_wifi_disconnect();
		vTaskDelay(pdMS_TO_TICKS(100));
		esp_wifi_stop();
		vTaskDelay(pdMS_TO_TICKS(100));
	}
	/* Do NOT call esp_hosted_deinit() (or esp_wifi_deinit()) here: on
	 * the P4 the SD card and the C6 share the SDMMC host, and hosted's
	 * transport teardown calls sdmmc_host_deinit(), which would kill
	 * /sdcard access for the emulator.  esp_wifi_stop() turns the RF
	 * off; the hosted link and driver stay parked until the next boot. */
	if (s_menu_lock) {
		xSemaphoreTake(s_menu_lock, portMAX_DELAY);
		s_sta_ok = 0;
		s_sta_ip[0] = 0;
		xSemaphoreGive(s_menu_lock);
	}
	s_menu_state = 0;
	ESP_LOGI(TAG, "menu wifi stopped for the emulator");
}
