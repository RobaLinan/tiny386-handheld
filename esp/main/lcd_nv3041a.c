#ifdef USE_LCD_NV3041A
/*
 * NV3041A 480x272 QSPI panel on the Guition JC4827W543N.
 *
 * Wire format copied from the vendor Arduino demo (Arduino_ESP32QSPI):
 * commands are 0x02 + a 16-bit address carrying the command byte, parameters
 * follow on one line; pixels are 0x32 + 0x3C00 with the payload on four
 * lanes.  Transactions are sent with polling, not the esp_lcd queue: the
 * queue path costs ~20 ms per band on this IDF build, while a polling
 * 32 KB QIO transfer takes ~2.5 ms.
 */
#include <stdio.h>
#include <string.h>
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "common.h"
#include "../../pc.h"
#include "../../term.h"
#include "../../i8042.h"

static const char *TAG = "lcd";

#define LCD_HOST        SPI2_HOST
#define PIN_CS          45
#define PIN_SCK         47
#define PIN_D0          21
#define PIN_D1          48
#define PIN_D2          40
#define PIN_D3          39
#define PIN_BL          1
#define LCD_PCLK_HZ     (32 * 1000 * 1000)

#define QSPI_WRITE_CMD   0x02
#define QSPI_WRITE_COLOR 0x32
#define QSPI_COLOR_ADDR  0x003C00

typedef struct {
	uint8_t cmd;
	uint8_t data;
	uint16_t delay_ms;
} nv3041a_init_cmd_t;

static const nv3041a_init_cmd_t init_cmds[] = {
	{0xFF, 0xA5, 0},
	{0x3A, 0x01, 0},
	{0x40, 0x01, 0},
	{0x41, 0x03, 0},
	{0x55, 0x01, 0},
	{0x36, 0x00, 0},
	{0x44, 0x15, 0},
	{0x45, 0x15, 0},
	{0x7D, 0x03, 0},
	{0xC1, 0xAB, 0},
	{0xC2, 0x17, 0},
	{0xC3, 0x10, 0},
	{0xC6, 0x3A, 0},
	{0xC7, 0x25, 0},
	{0xC8, 0x11, 0},
	{0x6F, 0x2F, 0},
	{0x78, 0x4B, 0},
	{0x7A, 0x49, 0},
	{0xC9, 0x00, 0},
	{0x51, 0x20, 0},
	{0x52, 0x7C, 0},
	{0x53, 0x1C, 0},
	{0x54, 0x77, 0},
	{0x46, 0x0A, 0},
	{0x47, 0x2A, 0},
	{0x48, 0x0A, 0},
	{0x49, 0x1A, 0},
	{0x56, 0x43, 0},
	{0x57, 0x42, 0},
	{0x58, 0x3C, 0},
	{0x59, 0x64, 0},
	{0x5A, 0x41, 0},
	{0x5B, 0x3C, 0},
	{0x5C, 0x02, 0},
	{0x5D, 0x3C, 0},
	{0x5E, 0x1F, 0},
	{0x60, 0x80, 0},
	{0x61, 0x3F, 0},
	{0x62, 0x21, 0},
	{0x63, 0x07, 0},
	{0x64, 0xE0, 0},
	{0x65, 0x01, 0},
	{0xCA, 0x20, 0},
	{0xCB, 0x52, 0},
	{0xCC, 0x10, 0},
	{0xCD, 0x42, 0},
	{0xD0, 0x20, 0},
	{0xD1, 0x52, 0},
	{0xD2, 0x10, 0},
	{0xD3, 0x42, 0},
	{0xD4, 0x0A, 0},
	{0xD5, 0x32, 0},
	{0x80, 0x03, 0},
	{0xA0, 0x00, 0},
	{0x81, 0x07, 0},
	{0xA1, 0x05, 0},
	{0x82, 0x06, 0},
	{0xA2, 0x04, 0},
	{0xA2, 0x04, 0},
	{0x83, 0x39, 0},
	{0xA3, 0x39, 0},
	{0x84, 0x3A, 0},
	{0xA4, 0x3A, 0},
	{0x85, 0x3F, 0},
	{0x85, 0x3F, 0},
	{0xA5, 0x3F, 0},
	{0x86, 0x2C, 0},
	{0xA6, 0x2A, 0},
	{0x87, 0x43, 0},
	{0xA7, 0x47, 0},
	{0x88, 0x08, 0},
	{0xA8, 0x08, 0},
	{0x89, 0x0F, 0},
	{0xA9, 0x0F, 0},
	{0x8A, 0x17, 0},
	{0xAA, 0x17, 0},
	{0x8B, 0x10, 0},
	{0xAB, 0x10, 0},
	{0x8C, 0x16, 0},
	{0xAC, 0x16, 0},
	{0x8D, 0x14, 0},
	{0xAD, 0x14, 0},
	{0x8E, 0x11, 0},
	{0xAE, 0x11, 0},
	{0x8F, 0x14, 0},
	{0xAF, 0x14, 0},
	{0x90, 0x06, 0},
	{0xB0, 0x06, 0},
	{0x91, 0x0F, 0},
	{0xB1, 0x0F, 0},
	{0x92, 0x16, 0},
	{0xB2, 0x16, 0},
	{0xFF, 0x00, 0},
	{0x11, 0x00, 40},
	{0x29, 0x00, 0},
	{0xE7, 0x00, 0},
	{0x35, 0x00, 30},
};

static spi_device_handle_t lcd_dev;

static inline void cs_low(void) { gpio_set_level(PIN_CS, 0); }
static inline void cs_high(void) { gpio_set_level(PIN_CS, 1); }

/* Match Arduino_ESP32QSPI::writeC8D8(c,d): USE_TXDATA | MULTILINE_CMD | MULTILINE_ADDR,
 * single-line data from tx_data register (not DMA buffer). */
static void lcd_cmd(uint8_t cmd, const uint8_t *param, size_t n)
{
	spi_transaction_ext_t t = {0};
	t.base.cmd = QSPI_WRITE_CMD;
	t.base.addr = (uint32_t)cmd << 8;
	t.base.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
	if (param && n) {
		t.base.flags |= SPI_TRANS_USE_TXDATA;
		t.base.length = n * 8;
		for (size_t i = 0; i < n && i < 4; i++)
			t.base.tx_data[i] = param[i];
	}
	cs_low();
	spi_device_acquire_bus(lcd_dev, portMAX_DELAY);
	ESP_ERROR_CHECK(spi_device_polling_transmit(lcd_dev, &t.base));
	spi_device_release_bus(lcd_dev);
	cs_high();
}

/* Match Arduino_ESP32QSPI::writeCommand(c): cmd+addr only, no data. */
static void lcd_write_cmd(uint8_t c)
{
	spi_transaction_ext_t t = {0};
	t.base.cmd = QSPI_WRITE_CMD;
	t.base.addr = (uint32_t)c << 8;
	t.base.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
	cs_low();
	spi_device_acquire_bus(lcd_dev, portMAX_DELAY);
	ESP_ERROR_CHECK(spi_device_polling_transmit(lcd_dev, &t.base));
	spi_device_release_bus(lcd_dev);
	cs_high();
}

/* Match Arduino_ESP32QSPI::write(d): cmd=0x32, addr=0x003C00, QIO byte. */
static void lcd_write_byte(uint8_t d)
{
	spi_transaction_ext_t t = {0};
	t.base.cmd = QSPI_WRITE_COLOR;
	t.base.addr = QSPI_COLOR_ADDR;
	t.base.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_MODE_QIO;
	t.base.tx_data[0] = d;
	t.base.length = 8;
	cs_low();
	spi_device_acquire_bus(lcd_dev, portMAX_DELAY);
	ESP_ERROR_CHECK(spi_device_polling_transmit(lcd_dev, &t.base));
	spi_device_release_bus(lcd_dev);
	cs_high();
}

/* Match writeC8D16D16Split: writeCommand + 4x write(byte). */
static void lcd_set_window(uint8_t cmd, uint16_t start, uint16_t end)
{
	lcd_write_cmd(cmd);
	lcd_write_byte(start >> 8);
	lcd_write_byte(start & 0xff);
	lcd_write_byte(end >> 8);
	lcd_write_byte(end & 0xff);
}

#define LCD_CHUNK 8192

void lcd_draw(int x_start, int y_start, int x_end, int y_end, void *src)
{
	static uint8_t *chunk;
	if (!lcd_dev)
		return;
	if (!chunk)
		chunk = heap_caps_malloc(LCD_CHUNK, MALLOC_CAP_DMA);
	if (!chunk) {
		ESP_LOGE(TAG, "no DMA chunk");
		return;
	}

	lcd_set_window(0x2A, x_start, x_end - 1);
	lcd_set_window(0x2B, y_start, y_end - 1);
	lcd_write_cmd(0x2C);

	size_t total = (size_t)(x_end - x_start) * (y_end - y_start) * 2;
	const uint16_t *p = src;
	int first = 1;

	cs_low();
	spi_device_acquire_bus(lcd_dev, portMAX_DELAY);
	while (total) {
		size_t n = total > LCD_CHUNK ? LCD_CHUNK : total;
		/* Byte-swap to big-endian on wire (matches MSB_16_SET). */
		size_t npix = n / 2;
		for (size_t i = 0; i < npix; i++) {
			uint16_t v = p[i];
			chunk[i * 2] = v >> 8;
			chunk[i * 2 + 1] = v & 0xff;
		}

		spi_transaction_ext_t t = {0};
		t.base.flags = SPI_TRANS_MODE_QIO;
		t.base.length = n * 8;
		t.base.tx_buffer = chunk;

		if (first) {
			t.base.cmd = QSPI_WRITE_COLOR;
			t.base.addr = QSPI_COLOR_ADDR;
			first = 0;
		} else {
			t.base.flags |= SPI_TRANS_VARIABLE_CMD |
					SPI_TRANS_VARIABLE_ADDR |
					SPI_TRANS_VARIABLE_DUMMY;
			t.command_bits = 0;
			t.address_bits = 0;
			t.dummy_bits = 0;
		}

		ESP_ERROR_CHECK(spi_device_polling_transmit(lcd_dev, &t.base));
		p += npix;
		total -= n;
	}
	spi_device_release_bus(lcd_dev);
	cs_high();
}

static void backlight_init(void)
{
	const ledc_timer_config_t timer = {
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.duty_resolution = LEDC_TIMER_10_BIT,
		.timer_num = LEDC_TIMER_0,
		.freq_hz = 5000,
		.clk_cfg = LEDC_AUTO_CLK,
	};
	const ledc_channel_config_t ch = {
		.gpio_num = PIN_BL,
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.channel = LEDC_CHANNEL_0,
		.timer_sel = LEDC_TIMER_0,
		.duty = 800,
		.hpoint = 0,
	};
	ESP_ERROR_CHECK(ledc_timer_config(&timer));
	ESP_ERROR_CHECK(ledc_channel_config(&ch));
}

#ifdef LCD_TE_PROBE
#define PIN_TE 0
static int te_measure(void)
{
	int edges = 0, last = gpio_get_level(PIN_TE);
	int64_t t0 = esp_timer_get_time();
	while (esp_timer_get_time() - t0 < 100000) {
		int v = gpio_get_level(PIN_TE);
		if (v != last)
			edges++;
		last = v;
	}
	return edges;
}

static void lcd_te_probe(void)
{
	gpio_config_t cfg = {
		.pin_bit_mask = 1ULL << PIN_TE,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	gpio_config(&cfg);
	uint8_t mode = 1;
	lcd_cmd(0x35, &mode, 1);
	vTaskDelay(pdMS_TO_TICKS(50));
	ESP_LOGI(TAG, "LCD_TE mode1 edges: %d, level %d",
		 te_measure(), gpio_get_level(PIN_TE));
	mode = 2;
	lcd_cmd(0x35, &mode, 1);
	vTaskDelay(pdMS_TO_TICKS(50));
	ESP_LOGI(TAG, "LCD_TE mode2 edges: %d, level %d",
		 te_measure(), gpio_get_level(PIN_TE));
}
#endif

#ifdef LCD_SPLASH
void *psmalloc(long size);
static void show_color(uint16_t *buf, uint16_t c, int ms)
{
	for (int k = 0; k < LCD_WIDTH * LCD_HEIGHT; k++)
		buf[k] = c;
	lcd_draw(0, 0, LCD_WIDTH, LCD_HEIGHT, buf);
	vTaskDelay(pdMS_TO_TICKS(ms));
}

static void backlight_set(int duty)
{
	ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
	ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void show_quadrants(uint16_t *buf)
{
	static const uint16_t colors[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
	const int qw = LCD_WIDTH / 2, qh = LCD_HEIGHT / 2;
	for (int i = 0; i < 4; i++) {
		uint16_t be = (colors[i] << 8) | (colors[i] >> 8);
		for (int k = 0; k < qw * qh; k++)
			buf[k] = be;
		int x = (i & 1) * qw, y = (i >> 1) * qh;
		lcd_draw(x, y, x + qw, y + qh, buf);
	}
}

static void lcd_splash(void)
{
	uint16_t *buf = psmalloc(LCD_WIDTH * LCD_HEIGHT * 2);
	if (!buf)
		return;
	ESP_LOGI(TAG, "splash white/black/red");
	show_color(buf, 0xFFFF, 2000);
	show_color(buf, 0x0000, 2000);
	show_color(buf, 0xF800, 2000);
}
#endif

#ifdef LCD_PROBE
static void lcd_probe(void)
{
	static const uint8_t cmds[] = {0x04, 0x0A, 0x0B};
	for (size_t c = 0; c < sizeof(cmds); c++) {
		uint8_t rx[8] = {0};
		spi_transaction_t t = {0};
		t.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR |
			  SPI_TRANS_CS_KEEP_ACTIVE;
		t.cmd = QSPI_WRITE_CMD;
		t.addr = (uint32_t)cmds[c] << 8;
		esp_err_t e0 = spi_device_acquire_bus(lcd_dev, portMAX_DELAY);
		esp_err_t e1 = spi_device_polling_transmit(lcd_dev, &t);
		spi_transaction_t r = {0};
		r.length = 0;
		r.rxlength = sizeof(rx) * 8;
		r.rx_buffer = rx;
		esp_err_t e2 = spi_device_polling_transmit(lcd_dev, &r);
		spi_device_release_bus(lcd_dev);
		ESP_LOGI(TAG, "probe %02X: %s/%s/%s %02X %02X %02X %02X %02X %02X %02X %02X",
			 cmds[c], esp_err_to_name(e0), esp_err_to_name(e1),
			 esp_err_to_name(e2),
			 rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7]);
	}
}
#endif

#ifdef LCD_BENCH
void *psmalloc(long size);
static void lcd_bench(void)
{
	uint8_t *buf = psmalloc(LCD_WIDTH * LCD_HEIGHT * 2);
	if (!buf) {
		ESP_LOGE(TAG, "bench alloc failed");
		return;
	}
	for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++)
		((uint16_t *)buf)[i] = (i & 32) ? 0xF800 : 0x001F;

	lcd_draw(0, 0, LCD_WIDTH, LCD_HEIGHT, buf);
	int64_t t0 = esp_timer_get_time();
	for (int f = 0; f < 10; f++)
		lcd_draw(0, 0, LCD_WIDTH, LCD_HEIGHT, buf);
	int64_t t1 = esp_timer_get_time();
	ESP_LOGI(TAG, "frame bench: %lld us/frame",
		 (long long)((t1 - t0) / 10));

	spi_device_interface_config_t tcfg = {
		.clock_speed_hz = LCD_PCLK_HZ,
		.mode = 0,
		.spics_io_num = -1,
		.queue_size = 1,
		.flags = SPI_DEVICE_HALFDUPLEX,
	};
	spi_device_handle_t th;
	if (spi_bus_add_device(LCD_HOST, &tcfg, &th) == ESP_OK) {
		uint8_t *ibuf = heap_caps_malloc(32768, MALLOC_CAP_DMA);
		for (int mode = 0; mode < 3; mode++) {
			spi_transaction_t t = {0};
			t.flags = SPI_TRANS_MODE_QIO;
			if (mode >= 1) {
				t.cmd = QSPI_WRITE_COLOR;
				t.addr = QSPI_COLOR_ADDR;
			}
			if (mode == 2)
				t.flags |= SPI_TRANS_MULTILINE_CMD |
					   SPI_TRANS_MULTILINE_ADDR;
			t.length = 32768 * 8;
			t.tx_buffer = (mode == 3) ? buf : ibuf;
			int64_t s0 = esp_timer_get_time();
			spi_device_polling_transmit(th, &t);
			int64_t s1 = esp_timer_get_time();
			ESP_LOGI(TAG, "qio test %d: %lld us", mode,
				 (long long)(s1 - s0));
		}
		heap_caps_free(ibuf);
		spi_bus_remove_device(th);
	}
}
#endif

static void panel_init(void)
{
	/* Manual CS GPIO — matches Arduino_ESP32QSPI which uses spics_io_num=-1. */
	gpio_config_t cs_cfg = {
		.pin_bit_mask = 1ULL << PIN_CS,
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	gpio_config(&cs_cfg);
	gpio_set_level(PIN_CS, 1);

	const spi_bus_config_t buscfg = {
		.mosi_io_num = PIN_D0,
		.miso_io_num = PIN_D1,
		.sclk_io_num = PIN_SCK,
		.quadwp_io_num = PIN_D2,
		.quadhd_io_num = PIN_D3,
		.data4_io_num = -1,
		.data5_io_num = -1,
		.data6_io_num = -1,
		.data7_io_num = -1,
		.max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2,
		.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
	};
	ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

	const spi_device_interface_config_t devcfg = {
		.clock_speed_hz = LCD_PCLK_HZ,
		.mode = 0,
		.command_bits = 8,
		.address_bits = 24,
		.spics_io_num = -1,
		.queue_size = 1,
		.flags = SPI_DEVICE_HALFDUPLEX,
	};
	ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &devcfg, &lcd_dev));

	lcd_cmd(0x01, NULL, 0);
	vTaskDelay(pdMS_TO_TICKS(120));

	for (size_t i = 0; i < sizeof(init_cmds) / sizeof(init_cmds[0]); i++) {
		lcd_cmd(init_cmds[i].cmd, &init_cmds[i].data, 1);
		if (init_cmds[i].delay_ms)
			vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
	}
	/* Rotation 0: MY|MX = 0xC0, RGB order, no inversion. */
	lcd_cmd(0x36, &(uint8_t){0xC0}, 1);
	lcd_cmd(0x20, NULL, 0);
	ESP_LOGI(TAG, "NV3041A init done");
}

void pc_vga_step(void *o);

static void put_key(void *o, int scan_code, int is_pressed)
{
	ps2_put_keycode(o, is_pressed, scan_code);
}

void vga_task(void *arg)
{
	fprintf(stderr, "vga runs on core %d\n", esp_cpu_get_core_id());

	panel_init();
#ifdef LCD_BENCH
	lcd_bench();
#endif
	backlight_init();
#ifdef LCD_SPLASH
	lcd_splash();
#endif
#ifdef LCD_TE_PROBE
	lcd_te_probe();
#endif
#ifdef LCD_PROBE
	lcd_probe();
#endif
	globals.panel = lcd_dev;

	xEventGroupSetBits(global_event_group, BIT1);
	xEventGroupWaitBits(global_event_group, BIT0,
			    pdFALSE, pdFALSE, portMAX_DELAY);
	PC *pc = (PC *)globals.pc;
#if defined(USE_USB_SERIAL_KBD)
	Term *term = term_init_input(put_key, pc->kbd);
#endif

	for (;;) {
		pc_vga_step(pc);
#if defined(USE_USB_SERIAL_KBD)
		term_step(term);
#endif
		vTaskDelay(10 / portTICK_PERIOD_MS);
	}
}
#endif
