#ifdef USE_LCD_SERIAL
#include "common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "../../vga.h"
#include "../../i8042.h"
#include "../../pc.h"
#include "../../term.h"

static void put_key(void *o, int scan_code, int is_pressed)
{
	ps2_put_keycode(o, is_pressed, scan_code);
}

void lcd_draw(int x_start, int y_start, int x_end, int y_end, void *src)
{
	(void)x_start;
	(void)y_start;
	(void)x_end;
	(void)y_end;
	(void)src;
}

void vga_task(void *arg)
{
	(void)arg;
	xEventGroupSetBits(global_event_group, BIT1);
	xEventGroupWaitBits(global_event_group, BIT0, pdFALSE, pdFALSE, portMAX_DELAY);

	PC *pc = (PC *)globals.pc;
	Term *term = term_init(pc->vga, put_key, pc->kbd);
	for (;;) {
		pc_vga_step(pc);
		term_step(term);
		vTaskDelay(10 / portTICK_PERIOD_MS);
	}
}
#endif
