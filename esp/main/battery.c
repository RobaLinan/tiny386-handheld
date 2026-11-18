/*
 * 1S Li-ion gauge on the battery divider (BAT+ --10k--+--10k-- GND).
 * The node sits on JP1 pin 9 / GPIO51 / ADC2_CH2 and needs no capacitor:
 * the 5k source is fine for the ADC sample-and-hold.
 */

#include <stdbool.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "battery.h"

#ifdef BATTERY_ADC_GPIO

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "board_adc.h"

#define SAMPLES 16
#define SAMPLE_GAP_MS 20
#define UPDATE_MS 3000
#define DIVIDER_RATIO 2

#ifndef BATTERY_FULL_MV
#define BATTERY_FULL_MV 4200
#endif

#define VBAT_MIN_MV 2500
#define VBAT_MAX_MV 5000

static const char *TAG = "bat";

/* -1 until the first valid reading reaches the screen. */
static volatile int s_mv = -1;
static volatile int s_pct = -1;

static bool cali_init(adc_unit_t unit, adc_atten_t atten, adc_cali_handle_t *out)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
	adc_cali_curve_fitting_config_t cfg = {
		.unit_id = unit,
		.atten = atten,
		.bitwidth = ADC_BITWIDTH_DEFAULT,
	};
	return adc_cali_create_scheme_curve_fitting(&cfg, out) == ESP_OK;
#else
	return false;
#endif
}

static int to_mv(bool has_cali, adc_cali_handle_t cali, int raw)
{
	int mv = 0;

	if (has_cali && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK)
		return mv;
	return raw * 3300 / 4095;
}

static int cmp_int(const void *a, const void *b)
{
	return *(const int *) a - *(const int *) b;
}

/* Average the middle half of the sorted samples to drop outliers. */
static int read_channel(adc_oneshot_unit_handle_t adc, adc_channel_t ch)
{
	int v[SAMPLES];
	int n = 0;

	for (int i = 0; i < SAMPLES; i++) {
		int raw;

		if (board_adc_read(adc, ch, &raw) == ESP_OK)
			v[n++] = raw;
		vTaskDelay(pdMS_TO_TICKS(SAMPLE_GAP_MS));
	}
	if (n < SAMPLES / 2)
		return -1;
	qsort(v, n, sizeof(v[0]), cmp_int);

	int sum = 0, cnt = 0;

	for (int i = n / 4; i < n * 3 / 4; i++, cnt++)
		sum += v[i];
	return sum / cnt;
}

/* Rough 1S Li-ion rest-voltage curve, shifted so a full pack under load
 * reads 100%; load and charging shift it further. */
static int level_pct(int mv)
{
	static const int v[] = { 3300, 3500, 3700, 3800, 4000, BATTERY_FULL_MV };
	static const int p[] = { 0, 10, 35, 50, 80, 100 };

	if (mv <= v[0])
		return 0;
	for (int i = 1; i < 6; i++)
		if (mv <= v[i])
			return p[i - 1] + (mv - v[i - 1]) * (p[i] - p[i - 1]) /
					  (v[i] - v[i - 1]);
	return 100;
}

static void battery_task(void *arg)
{
	adc_unit_t unit;
	adc_channel_t ch;
	adc_oneshot_unit_handle_t adc = board_adc2_get();
	adc_cali_handle_t cali = NULL;
	bool has_cali;

	(void) arg;
	if (!adc ||
	    adc_oneshot_io_to_channel(BATTERY_ADC_GPIO, &unit, &ch) != ESP_OK ||
	    unit != ADC_UNIT_2) {
		ESP_LOGW(TAG, "GPIO%d unavailable, battery display off",
			 BATTERY_ADC_GPIO);
		vTaskDelete(NULL);
		return;
	}

	adc_oneshot_chan_cfg_t chan_cfg = {
		.atten = ADC_ATTEN_DB_12,
		.bitwidth = ADC_BITWIDTH_DEFAULT,
	};

	if (adc_oneshot_config_channel(adc, ch, &chan_cfg) != ESP_OK) {
		ESP_LOGW(TAG, "channel config failed, battery display off");
		vTaskDelete(NULL);
		return;
	}

	has_cali = cali_init(unit, ADC_ATTEN_DB_12, &cali);
	ESP_LOGI(TAG, "GPIO%d -> ADC%d_CH%d, divider x%d, cali=%s",
		 BATTERY_ADC_GPIO, unit + 1, ch, DIVIDER_RATIO,
		 has_cali ? "on" : "off");

	int last_pct = -1;

	for (;;) {
		int raw = read_channel(adc, ch);

		if (raw >= 0) {
			int pin_mv = to_mv(has_cali, cali, raw);
			int mv = pin_mv * DIVIDER_RATIO;

			if (mv >= VBAT_MIN_MV && mv <= VBAT_MAX_MV) {
				s_mv = s_mv < 0 ? mv : (s_mv * 3 + mv) / 4;
				s_pct = level_pct(s_mv);
				if (s_pct != last_pct) {
					last_pct = s_pct;
					ESP_LOGI(TAG, "Vbat %d mV (~%d%%), pin %d mV",
						 s_mv, s_pct, pin_mv);
				}
			} else {
				s_pct = -1;
			}
		}
		vTaskDelay(pdMS_TO_TICKS(UPDATE_MS));
	}
}

void battery_init(void)
{
	xTaskCreatePinnedToCore(battery_task, "battery", 4096, NULL, 1, NULL, 0);
}

int battery_get_percent(void)
{
	return s_pct;
}

int battery_get_mv(void)
{
	return s_mv;
}

#else /* !BATTERY_ADC_GPIO */

void battery_init(void)
{
}

int battery_get_percent(void)
{
	return -1;
}

int battery_get_mv(void)
{
	return -1;
}

#endif
