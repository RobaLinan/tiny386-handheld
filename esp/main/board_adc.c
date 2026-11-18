#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "board_adc.h"

#if defined(USE_GAMEPAD) || defined(BATTERY_ADC_GPIO)

static const char *TAG = "adc";

static adc_oneshot_unit_handle_t adc2;
static SemaphoreHandle_t adc2_mutex;

adc_oneshot_unit_handle_t board_adc2_init(void)
{
	if (!adc2_mutex)
		adc2_mutex = xSemaphoreCreateMutex();
	if (adc2)
		return adc2;

	adc_oneshot_unit_init_cfg_t cfg = { .unit_id = ADC_UNIT_2 };

	if (adc_oneshot_new_unit(&cfg, &adc2) != ESP_OK) {
		ESP_LOGW(TAG, "ADC2 unavailable (already in use)");
		adc2 = NULL;
	}
	return adc2;
}

adc_oneshot_unit_handle_t board_adc2_get(void)
{
	return adc2;
}

esp_err_t board_adc_read(adc_oneshot_unit_handle_t adc, adc_channel_t ch,
			 int *raw)
{
	esp_err_t err;

	if (!adc || !adc2_mutex)
		return ESP_ERR_INVALID_STATE;

	xSemaphoreTake(adc2_mutex, portMAX_DELAY);
	err = adc_oneshot_read(adc, ch, raw);
	xSemaphoreGive(adc2_mutex);
	return err;
}

#else

adc_oneshot_unit_handle_t board_adc2_init(void)
{
	return NULL;
}

adc_oneshot_unit_handle_t board_adc2_get(void)
{
	return NULL;
}

esp_err_t board_adc_read(adc_oneshot_unit_handle_t adc, adc_channel_t ch,
			 int *raw)
{
	(void) adc;
	(void) ch;
	(void) raw;
	return ESP_ERR_INVALID_STATE;
}

#endif
