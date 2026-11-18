#ifndef BOARD_ADC_H
#define BOARD_ADC_H

#include "esp_adc/adc_oneshot.h"

/* ADC2 is shared: the stick (CH0/CH1) and the battery divider (CH2) must use
 * one unit handle, because adc_oneshot_new_unit() claims the whole unit. */
adc_oneshot_unit_handle_t board_adc2_init(void);
adc_oneshot_unit_handle_t board_adc2_get(void);

/* Serialized read: adc_oneshot_read() uses a non-blocking unit lock, so two
 * tasks reading ADC2 would otherwise fail each other with ESP_ERR_TIMEOUT. */
esp_err_t board_adc_read(adc_oneshot_unit_handle_t adc, adc_channel_t ch,
			 int *raw);

#endif /* BOARD_ADC_H */
