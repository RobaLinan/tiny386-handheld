/*
 * Handheld gamepad input for the JC4880P443 board.
 *
 * 3x4 key matrix (diodes, active high):
 *   rows GPIO28/29/30, columns GPIO31/32/33/52
 *     Up   Down Left Right
 *     A    B    C    D
 *     Sel  OK   L2   R2
 * M5Stack Unit Joystick2 (hall effect) on the shared I2C bus, addr 0x63.
 *
 * Buttons send PS/2 set-1 scancodes; the stick sends the configured arrow
 * keys or, with Mouse=1, PS/2 mouse motion. Mapping comes from menu.ini.
 * With ShopCycle=1 the stick button walks through Shift/Ctrl/Alt+F1..F10
 * while held, for games with keyboard-only hidden shops.
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "gamepad.h"

static const char *const pad_button_names[PAD_BUTTON_COUNT] = {
	"Up", "Down", "Left", "Right",
	"A", "B", "C", "D",
	"Select", "OK", "L2", "R2",
};

struct gamepad_config gamepad_default_config(void)
{
	struct gamepad_config c = {
		.mouse = 0,
		.shop_cycle = 0,
		.key = {
			[PAD_UP] = 0xE048,
			[PAD_DOWN] = 0xE050,
			[PAD_LEFT] = 0xE04B,
			[PAD_RIGHT] = 0xE04D,
			[PAD_A] = 0x1C,      /* Enter */
			[PAD_B] = 0x01,      /* Esc */
			[PAD_C] = 0x39,      /* Space */
			[PAD_D] = 0x1D,      /* Ctrl */
			[PAD_SELECT] = 0x3B, /* F1 */
			[PAD_OK] = 0x1C,     /* Enter */
			[PAD_L2] = 0x2A,     /* Shift */
			[PAD_R2] = 0x0F,     /* Tab */
		},
		.mouse_l = PAD_OK,
		.mouse_r = PAD_L2,
	};

	return c;
}

struct gamepad_settings gamepad_default_settings(void)
{
	struct gamepad_settings s = { .invert_x = 0, .invert_y = 0 };

	return s;
}

struct key_name {
	const char *name;
	int code;
};

static const struct key_name key_names[] = {
	{ "enter", 0x1C }, { "return", 0x1C },
	{ "esc", 0x01 }, { "escape", 0x01 },
	{ "space", 0x39 }, { "spacebar", 0x39 },
	{ "ctrl", 0x1D }, { "control", 0x1D }, { "lctrl", 0x1D },
	{ "rctrl", 0xE01D },
	{ "shift", 0x2A }, { "lshift", 0x2A }, { "rshift", 0x36 },
	{ "alt", 0x38 }, { "lalt", 0x38 }, { "ralt", 0xE038 },
	{ "tab", 0x0F },
	{ "backspace", 0x0E }, { "bksp", 0x0E },
	{ "up", 0xE048 }, { "down", 0xE050 },
	{ "left", 0xE04B }, { "right", 0xE04D },
	{ "home", 0xE047 }, { "end", 0xE04F },
	{ "pgup", 0xE049 }, { "pageup", 0xE049 },
	{ "pgdn", 0xE051 }, { "pagedown", 0xE051 },
	{ "insert", 0xE052 }, { "ins", 0xE052 },
	{ "delete", 0xE053 }, { "del", 0xE053 },
	{ "caps", 0x3A }, { "capslock", 0x3A },
	{ "numlock", 0x45 }, { "scrolllock", 0x46 },
	{ "f1", 0x3B }, { "f2", 0x3C }, { "f3", 0x3D }, { "f4", 0x3E },
	{ "f5", 0x3F }, { "f6", 0x40 }, { "f7", 0x41 }, { "f8", 0x42 },
	{ "f9", 0x43 }, { "f10", 0x44 }, { "f11", 0x57 }, { "f12", 0x58 },
	{ "-", 0x0C }, { "=", 0x0D }, { "[", 0x1A }, { "]", 0x1B },
	{ ";", 0x27 }, { "'", 0x28 }, { "`", 0x29 }, { "\\", 0x2B },
	{ ",", 0x33 }, { ".", 0x34 }, { "/", 0x35 },
	{ "minus", 0x0C }, { "equals", 0x0D },
	{ "lbracket", 0x1A }, { "rbracket", 0x1B },
	{ "semicolon", 0x27 }, { "quote", 0x28 }, { "backtick", 0x29 },
	{ "backslash", 0x2B }, { "comma", 0x33 }, { "period", 0x34 },
	{ "slash", 0x35 },
};

static const int letter_scan[26] = {
	0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24,
	0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14,
	0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
};

static const int digit_scan[10] = {
	0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
};

int gamepad_parse_key(const char *name)
{
	if (!name || !name[0])
		return 0;
	if (!strcasecmp(name, "none") || !strcasecmp(name, "off") ||
	    !strcmp(name, "-"))
		return 0;
	if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
		char *end;
		long v = strtol(name, &end, 16);

		if (*end == '\0' && v > 0 && v <= 0xE0FF)
			return (int) v;
		return -1;
	}
	for (size_t i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++)
		if (!strcasecmp(name, key_names[i].name))
			return key_names[i].code;
	if (name[1] == '\0') {
		unsigned char ch = (unsigned char) tolower((unsigned char) name[0]);

		if (ch >= 'a' && ch <= 'z')
			return letter_scan[ch - 'a'];
		if (ch >= '0' && ch <= '9')
			return digit_scan[ch - '0'];
	}
	return -1;
}

int gamepad_button_index(const char *name)
{
	if (!name)
		return -1;
	for (int i = 0; i < PAD_BUTTON_COUNT; i++)
		if (!strcasecmp(name, pad_button_names[i]))
			return i;
	/* Schematic labels; the button tester calls these C and D. */
	if (!strcasecmp(name, "x"))
		return PAD_C;
	if (!strcasecmp(name, "y"))
		return PAD_D;
	if (!strcasecmp(name, "start"))
		return PAD_OK;
	if (!strcasecmp(name, "l1"))
		return PAD_L2;
	if (!strcasecmp(name, "r1"))
		return PAD_R2;
	return -1;
}

#ifdef USE_GAMEPAD

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "common.h"
#include "../../i8042.h"

#define ROW_N 3
#define COL_N 4

/* M5Stack Unit Joystick2: X/Y raw little-endian at 0x00, calibrated
 * signed 12-bit offsets at 0x50, firmware version at 0xFE. */
#define JOY_I2C_ADDR 0x63
#define JOY_I2C_HZ 400000
#define JOY_I2C_HZ_SLOW 100000
#define JOY_REG_RAW 0x00
#define JOY_REG_BTN 0x20       /* 1 byte, 0 = stick pressed */
#define JOY_REG_OFFSET 0x50
#define JOY_REG_FW 0xFE
#define JOY_RAW_12BIT_MAX 4095
#define JOY_RAW_16BIT_MAX 65535
#define JOY_DEADZONE_PCT 4     /* radial, percent of full scale */
#define JOY_DIR_PRESS 120      /* normalized -1000..1000 */
#define JOY_DIR_RELEASE 80
#define MOUSE_MAX_STEP 10

#define POLL_MS 10
#define STABLE_N 3
#define SETTLE_US 200

/* Hidden-shop helper: holding the stick button walks through
 * {Shift,Ctrl,Alt} x F1..F10.  A release stops after releasing the
 * keys that are currently down. */
#define SHOP_BTN_STABLE 3
#define SHOP_MOD_LEAD_US 20000
#define SHOP_KEY_HOLD_US 80000
#define SHOP_GAP_US 120000
#define SHOP_FKEY_FIRST 0x3B
#define SHOP_COMBO_N 30

#define REPEAT_DELAY_US 500000
#define REPEAT_RATE_US 80000
#define MOUSE_INTERVAL_US 20000
#define MENU_REPEAT_DELAY_US 450000
#define MENU_REPEAT_RATE_US 300000

#define MAX_HELD_KEYS 16

#define STICK_SRC_UP (PAD_BUTTON_COUNT + 0)
#define STICK_SRC_DOWN (PAD_BUTTON_COUNT + 1)
#define STICK_SRC_LEFT (PAD_BUTTON_COUNT + 2)
#define STICK_SRC_RIGHT (PAD_BUTTON_COUNT + 3)
#define SRC_COUNT (PAD_BUTTON_COUNT + 4)

static const char *TAG = "gamepad";

static const gpio_num_t row_gpio[ROW_N] = {
	GPIO_NUM_28, GPIO_NUM_29, GPIO_NUM_30
};
static const gpio_num_t col_gpio[COL_N] = {
	GPIO_NUM_31, GPIO_NUM_32, GPIO_NUM_33, GPIO_NUM_52
};

/* The PCB wires the Down and Left switches to each other's columns. */
static const uint8_t pad_map[ROW_N * COL_N] = {
	PAD_UP, PAD_LEFT, PAD_DOWN, PAD_RIGHT,
	PAD_A, PAD_B, PAD_C, PAD_D,
	PAD_SELECT, PAD_OK, PAD_L2, PAD_R2,
};

static struct gamepad_config cfg;
static struct gamepad_settings cfg_settings;
static PS2KbdState *kbd;

static bool src_down[SRC_COUNT];
static bool pad_stable[ROW_N][COL_N];
static uint8_t pad_debounce[ROW_N][COL_N];
static bool stick_stable[4];
static uint8_t stick_debounce[4];

static volatile bool input_enabled;
static int64_t enable_fallback_us;

static QueueHandle_t menu_queue;
static bool menu_ready;
static bool menu_left_held, menu_right_held, menu_any_held;
static int64_t menu_left_repeat, menu_right_repeat;

struct held_key {
	int code;
	int count;
	int64_t repeat_at;
};

static struct held_key held[MAX_HELD_KEYS];

static i2c_master_dev_handle_t joy_dev;
static bool joy_ready;
static int joy_cx, joy_cy;
static int joy_max = JOY_RAW_12BIT_MAX;
static int joy_deadzone = JOY_RAW_12BIT_MAX * JOY_DEADZONE_PCT / 100;
static int64_t joy_log_us;

enum shop_state {
	SHOP_IDLE,
	SHOP_WAIT_KEY,
	SHOP_HOLD_KEY,
	SHOP_GAP,
};

static const int shop_mod_code[3] = { 0x2A, 0x1D, 0x38 };
static const char *const shop_mod_name[3] = { "shift", "ctrl", "alt" };

static bool shop_btn_stable;
static uint8_t shop_btn_debounce;
static int shop_state;
static int shop_combo;
static int shop_mod, shop_key;
static int64_t shop_next_us;

#ifdef BOOT_MENU
extern int boot_choice;
const struct gamepad_config *boot_menu_gamepad_config(int idx);
const struct gamepad_settings *boot_menu_gamepad_settings(void);
#endif

static void matrix_init(void)
{
	uint64_t row_mask = 0;
	uint64_t col_mask = 0;

	for (int r = 0; r < ROW_N; r++)
		row_mask |= 1ULL << row_gpio[r];
	for (int c = 0; c < COL_N; c++)
		col_mask |= 1ULL << col_gpio[c];

	const gpio_config_t row_cfg = {
		.pin_bit_mask = row_mask,
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	const gpio_config_t col_cfg = {
		.pin_bit_mask = col_mask,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_ENABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK(gpio_config(&row_cfg));
	ESP_ERROR_CHECK(gpio_config(&col_cfg));
	for (int r = 0; r < ROW_N; r++)
		gpio_set_level(row_gpio[r], 0);
}

static void scan_matrix(uint8_t raw[ROW_N])
{
	for (int r = 0; r < ROW_N; r++) {
		for (int i = 0; i < ROW_N; i++)
			gpio_set_level(row_gpio[i], i == r);
		esp_rom_delay_us(SETTLE_US);

		uint8_t bits = 0;

		for (int c = 0; c < COL_N; c++)
			bits |= (uint8_t) gpio_get_level(col_gpio[c]) << c;
		raw[r] = bits;
	}
	for (int i = 0; i < ROW_N; i++)
		gpio_set_level(row_gpio[i], 0);
}

static bool joy_i2c_read(uint8_t reg, uint8_t *buf, size_t len)
{
	if (!joy_dev)
		return false;
	return i2c_master_transmit_receive(joy_dev, &reg, 1, buf, len, 50) ==
	       ESP_OK;
}

static bool joy_read(int *x, int *y)
{
	uint8_t buf[4];

	if (!joy_i2c_read(JOY_REG_RAW, buf, sizeof(buf)))
		return false;
	*x = buf[0] | (buf[1] << 8);
	*y = buf[2] | (buf[3] << 8);
	return true;
}

static bool joy_button_read(void)
{
	uint8_t v;

	if (!joy_i2c_read(JOY_REG_BTN, &v, 1))
		return false;
	return v == 0;
}

static void joy_log_offset(void)
{
	uint8_t buf[4];
	int16_t ox, oy;

	if (!joy_i2c_read(JOY_REG_OFFSET, buf, sizeof(buf)))
		return;
	memcpy(&ox, buf, 2);
	memcpy(&oy, buf + 2, 2);
	ESP_LOGI(TAG, "module calibrated offset X=%d Y=%d", ox, oy);
}

static bool joy_calibrate(void)
{
	int sx = 0, sy = 0, raw_max = 0;

	for (int i = 0; i < 32; i++) {
		int x, y;

		if (!joy_read(&x, &y))
			return false;
		sx += x;
		sy += y;
		if (x > raw_max)
			raw_max = x;
		if (y > raw_max)
			raw_max = y;
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	joy_cx = sx / 32;
	joy_cy = sy / 32;
	joy_max = raw_max > JOY_RAW_12BIT_MAX ? JOY_RAW_16BIT_MAX
					      : JOY_RAW_12BIT_MAX;
	joy_deadzone = joy_max * JOY_DEADZONE_PCT / 100;
	ESP_LOGI(TAG, "stick center X=%d Y=%d, range 0..%d, deadzone %d",
		 joy_cx, joy_cy, joy_max, joy_deadzone);
	joy_log_offset();
	if (joy_cx < joy_max / 8 || joy_cx > joy_max * 7 / 8 ||
	    joy_cy < joy_max / 8 || joy_cy > joy_max * 7 / 8)
		ESP_LOGW(TAG, "stick center is far from mid-scale; "
			 "held during boot or mounted oddly");
	return true;
}

static bool joy_probe(void)
{
	uint8_t fw = 0;

	if (!joy_i2c_read(JOY_REG_FW, &fw, 1))
		return false;
	ESP_LOGI(TAG, "Unit Joystick2 found, firmware %u", fw);
	return true;
}

static bool joy_open(int hz)
{
	i2c_master_bus_handle_t bus = NULL;
	i2c_device_config_t dev_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = JOY_I2C_ADDR,
		.scl_speed_hz = hz,
	};

	if (i2c_master_get_bus_handle(ES8311_I2C_NUM, &bus) != ESP_OK || !bus) {
		ESP_LOGW(TAG, "no I2C bus available for the joystick");
		return false;
	}
	if (i2c_master_bus_add_device(bus, &dev_cfg, &joy_dev) != ESP_OK ||
	    !joy_probe()) {
		if (joy_dev) {
			i2c_master_bus_rm_device(joy_dev);
			joy_dev = NULL;
		}
		return false;
	}
	return true;
}

static bool joy_init(void)
{
	uint8_t btn = 0xFF;

	if (!joy_open(JOY_I2C_HZ)) {
		ESP_LOGW(TAG, "joystick silent at %d Hz, retrying at %d Hz",
			 JOY_I2C_HZ, JOY_I2C_HZ_SLOW);
		if (!joy_open(JOY_I2C_HZ_SLOW)) {
			ESP_LOGW(TAG, "joystick not found at 0x%02X "
				 "(check SDA/SCL wiring)", JOY_I2C_ADDR);
			return false;
		}
	}
	if (!joy_calibrate())
		return false;
	if (joy_i2c_read(JOY_REG_BTN, &btn, 1))
		ESP_LOGI(TAG, "stick button register %u (0=pressed)", btn);
	else
		ESP_LOGW(TAG, "stick button register unreadable");
	return true;
}

static int source_key(int src)
{
	if (src < PAD_BUTTON_COUNT)
		return cfg.key[src];
	return cfg.key[PAD_UP + (src - PAD_BUTTON_COUNT)];
}

static bool source_is_click(int src)
{
	if (!cfg.mouse || src >= PAD_BUTTON_COUNT)
		return false;
	return src == cfg.mouse_l || src == cfg.mouse_r;
}

static struct held_key *held_find(int code)
{
	for (int i = 0; i < MAX_HELD_KEYS; i++)
		if (held[i].count > 0 && held[i].code == code)
			return &held[i];
	return NULL;
}

static void key_down(int code)
{
	struct held_key *h = held_find(code);

	if (!h) {
		for (int i = 0; i < MAX_HELD_KEYS; i++) {
			if (held[i].count == 0) {
				held[i] = (struct held_key) {
					.code = code,
					.repeat_at = esp_timer_get_time() +
						     REPEAT_DELAY_US,
				};
				h = &held[i];
				break;
			}
		}
		if (!h)
			return;
	}
	if (h->count++ == 0) {
		ps2_put_keycode(kbd, 1, code);
		h->repeat_at = esp_timer_get_time() + REPEAT_DELAY_US;
	}
}

static void key_up(int code)
{
	struct held_key *h = held_find(code);

	if (!h)
		return;
	if (--h->count == 0) {
		ps2_put_keycode(kbd, 0, code);
		h->code = 0;
	}
}

static void set_source(int src, bool down)
{
	int code;

	if (src_down[src] == down)
		return;
	src_down[src] = down;
	if (source_is_click(src))
		return;
	code = source_key(src);
	if (!code)
		return;
	if (down)
		key_down(code);
	else
		key_up(code);
}

static void repeat_keys(int64_t now)
{
	for (int i = 0; i < MAX_HELD_KEYS; i++) {
		if (held[i].count <= 0 || now < held[i].repeat_at)
			continue;
		ps2_put_keycode(kbd, 1, held[i].code);
		held[i].repeat_at = now + REPEAT_RATE_US;
	}
}

static void shop_begin(int64_t now)
{
	shop_mod = shop_mod_code[shop_combo / 10];
	shop_key = SHOP_FKEY_FIRST + shop_combo % 10;
	ps2_put_keycode(kbd, 1, shop_mod);
	shop_state = SHOP_WAIT_KEY;
	shop_next_us = now + SHOP_MOD_LEAD_US;
}

static void shop_cancel(void)
{
	if (shop_state == SHOP_IDLE)
		return;
	if (kbd) {
		if (shop_state == SHOP_HOLD_KEY)
			ps2_put_keycode(kbd, 0, shop_key);
		if (shop_state != SHOP_GAP)
			ps2_put_keycode(kbd, 0, shop_mod);
	}
	shop_state = SHOP_IDLE;
}

static void shop_cycle_tick(int64_t now)
{
	bool raw;

	if (!cfg.shop_cycle) {
		shop_cancel();
		shop_btn_stable = false;
		shop_btn_debounce = 0;
		return;
	}

	raw = joy_button_read();
	if (raw == shop_btn_stable) {
		shop_btn_debounce = 0;
	} else if (++shop_btn_debounce >= SHOP_BTN_STABLE) {
		shop_btn_debounce = 0;
		shop_btn_stable = raw;
		if (!raw) {
			shop_cancel();
		} else if (input_enabled && kbd) {
			shop_combo = 0;
			shop_begin(now);
		}
	}

	if (shop_state == SHOP_IDLE || now < shop_next_us)
		return;

	switch (shop_state) {
	case SHOP_WAIT_KEY:
		ps2_put_keycode(kbd, 1, shop_key);
		shop_state = SHOP_HOLD_KEY;
		shop_next_us = now + SHOP_KEY_HOLD_US;
		ESP_LOGI(TAG, "shop combo %d/%d %s+F%d",
			 shop_combo + 1, SHOP_COMBO_N,
			 shop_mod_name[shop_combo / 10],
			 shop_combo % 10 + 1);
		break;
	case SHOP_HOLD_KEY:
		ps2_put_keycode(kbd, 0, shop_key);
		ps2_put_keycode(kbd, 0, shop_mod);
		shop_state = SHOP_GAP;
		shop_next_us = now + SHOP_GAP_US;
		break;
	case SHOP_GAP:
		shop_combo = (shop_combo + 1) % SHOP_COMBO_N;
		shop_begin(now);
		break;
	default:
		break;
	}
}

static bool dir_neg(bool was_down, int delta, int press, int release)
{
	return delta <= -(was_down ? release : press);
}

static bool dir_pos(bool was_down, int delta, int press, int release)
{
	return delta >= (was_down ? release : press);
}

static bool stick_debounce_dir(int dir, bool pressed)
{
	if (pressed == stick_stable[dir]) {
		stick_debounce[dir] = 0;
		return stick_stable[dir];
	}
	if (++stick_debounce[dir] >= STABLE_N) {
		stick_stable[dir] = pressed;
		stick_debounce[dir] = 0;
	}
	return stick_stable[dir];
}

static int axis_norm(int delta, int center, int lo, int hi)
{
	int span = delta >= 0 ? hi - center : center - lo;
	int v;

	if (span <= 0)
		return 0;
	v = delta * 1000 / span;
	if (v > 1000)
		v = 1000;
	if (v < -1000)
		v = -1000;
	return v;
}

static int mouse_step(int norm)
{
	int v = norm * MOUSE_MAX_STEP / 1000;

	if (!v && norm)
		v = norm > 0 ? 1 : -1;
	return v;
}

static int mouse_buttons(void)
{
	int b = 0;

	for (int i = 0; i < PAD_BUTTON_COUNT; i++) {
		if (!src_down[i])
			continue;
		if (i == cfg.mouse_l)
			b |= 1;
		if (i == cfg.mouse_r)
			b |= 2;
	}
	return b;
}

static void mouse_tick(bool joy_ok, int nx, int ny, int64_t now)
{
	static int sent_buttons;
	static int64_t next_us;

	int buttons = mouse_buttons();

	if (buttons != sent_buttons) {
		ps2_mouse_event(globals.mouse, 0, 0, 0, buttons);
		sent_buttons = buttons;
	}
	if (!joy_ok || now < next_us)
		return;
	next_us = now + MOUSE_INTERVAL_US;
	int mx = mouse_step(nx);
	int my = mouse_step(ny);

	if (mx || my)
		ps2_mouse_event(globals.mouse, mx, my, 0, buttons);
}

static void menu_tick(const bool want[SRC_COUNT], int64_t now)
{
	bool left = want[PAD_LEFT] || stick_stable[2];
	bool right = want[PAD_RIGHT] || stick_stable[3];
	bool any = false;

	for (int i = PAD_A; i < PAD_BUTTON_COUNT; i++) {
		if (want[i]) {
			any = true;
			break;
		}
	}
	if (!menu_ready || !menu_queue) {
		menu_left_held = left;
		menu_right_held = right;
		menu_any_held = any;
		if (left)
			menu_left_repeat = now + MENU_REPEAT_DELAY_US;
		if (right)
			menu_right_repeat = now + MENU_REPEAT_DELAY_US;
		return;
	}
	if (left && !menu_left_held) {
		uint8_t ev = GAMEPAD_MENU_LEFT;

		xQueueSend(menu_queue, &ev, 0);
		menu_left_repeat = now + MENU_REPEAT_DELAY_US;
	} else if (left && now >= menu_left_repeat) {
		uint8_t ev = GAMEPAD_MENU_LEFT;

		xQueueSend(menu_queue, &ev, 0);
		menu_left_repeat = now + MENU_REPEAT_RATE_US;
	}
	menu_left_held = left;

	if (right && !menu_right_held) {
		uint8_t ev = GAMEPAD_MENU_RIGHT;

		xQueueSend(menu_queue, &ev, 0);
		menu_right_repeat = now + MENU_REPEAT_DELAY_US;
	} else if (right && now >= menu_right_repeat) {
		uint8_t ev = GAMEPAD_MENU_RIGHT;

		xQueueSend(menu_queue, &ev, 0);
		menu_right_repeat = now + MENU_REPEAT_RATE_US;
	}
	menu_right_held = right;

	if (any && !menu_any_held) {
		uint8_t ev = GAMEPAD_MENU_START;

		xQueueSend(menu_queue, &ev, 0);
	}
	menu_any_held = any;
}

void gamepad_menu_ready(void)
{
	if (!menu_queue)
		return;
	xQueueReset(menu_queue);
	menu_ready = true;
}

int gamepad_menu_poll(void)
{
	uint8_t ev;

	if (!menu_ready || !menu_queue)
		return 0;
	if (xQueueReceive(menu_queue, &ev, 0) == pdTRUE)
		return ev;
	return 0;
}

static void load_config(void)
{
	cfg = gamepad_default_config();
	cfg_settings = gamepad_default_settings();
#ifdef BOOT_MENU
	const struct gamepad_config *c = boot_menu_gamepad_config(boot_choice);
	const struct gamepad_settings *s = boot_menu_gamepad_settings();

	if (c)
		cfg = *c;
	if (s)
		cfg_settings = *s;
#endif
	ESP_LOGI(TAG, "mouse=%d shop=%d invert=%d/%d keys %04X %04X %04X %04X "
		      "%02X %02X %02X %02X %02X %02X %02X %02X",
		 cfg.mouse, cfg.shop_cycle, cfg_settings.invert_x,
		 cfg_settings.invert_y,
		 cfg.key[PAD_UP], cfg.key[PAD_DOWN], cfg.key[PAD_LEFT],
		 cfg.key[PAD_RIGHT], cfg.key[PAD_A], cfg.key[PAD_B],
		 cfg.key[PAD_C], cfg.key[PAD_D], cfg.key[PAD_SELECT],
		 cfg.key[PAD_OK], cfg.key[PAD_L2], cfg.key[PAD_R2]);
}

static void gamepad_task(void *arg)
{
	(void) arg;

	matrix_init();
	joy_ready = joy_init();
	cfg_settings = gamepad_default_settings();

	bool menu_mode = true;

	for (;;) {
		uint8_t raw[ROW_N];
		bool want[SRC_COUNT] = { false };
		int x = 0, y = 0, nx = 0, ny = 0;
		bool joy_ok = joy_ready && joy_read(&x, &y);
		int64_t now = esp_timer_get_time();
		uint32_t bits = xEventGroupGetBits(global_event_group);

		if (menu_mode && (bits & BIT0)) {
			menu_mode = false;
			menu_ready = false;
			kbd = globals.kbd;
			load_config();
			memset(src_down, 0, sizeof(src_down));
			memset(held, 0, sizeof(held));
			menu_left_held = menu_right_held = menu_any_held = false;
			if (menu_queue)
				xQueueReset(menu_queue);
#ifdef BOOT_MENU
			input_enabled = false;
#else
			input_enabled = true;
#endif
			enable_fallback_us = 0;
		}

		scan_matrix(raw);
		for (int r = 0; r < ROW_N; r++) {
			for (int c = 0; c < COL_N; c++) {
				bool now_down = (raw[r] >> c) & 1;
				int idx = r * COL_N + c;

				if (now_down == pad_stable[r][c]) {
					pad_debounce[r][c] = 0;
				} else if (++pad_debounce[r][c] >= STABLE_N) {
					pad_stable[r][c] = now_down;
					pad_debounce[r][c] = 0;
				}
				want[pad_map[idx]] = pad_stable[r][c];
			}
		}

		if (joy_ok) {
			int dx = x - joy_cx;
			int dy = y - joy_cy;

			if (dx * dx + dy * dy > joy_deadzone * joy_deadzone) {
				nx = axis_norm(dx, joy_cx, 0, joy_max);
				ny = axis_norm(dy, joy_cy, 0, joy_max);
				if (cfg_settings.invert_x)
					nx = -nx;
				if (cfg_settings.invert_y)
					ny = -ny;
			}
			if ((abs(dx) > joy_deadzone || abs(dy) > joy_deadzone) &&
			    now - joy_log_us > 200000) {
				joy_log_us = now;
				ESP_LOGI(TAG, "stick X=%d Y=%d d=%d,%d",
					 x, y, dx, dy);
			}
			want[STICK_SRC_UP] = stick_debounce_dir(0,
				dir_neg(stick_stable[0], ny,
					JOY_DIR_PRESS, JOY_DIR_RELEASE));
			want[STICK_SRC_DOWN] = stick_debounce_dir(1,
				dir_pos(stick_stable[1], ny,
					JOY_DIR_PRESS, JOY_DIR_RELEASE));
			want[STICK_SRC_LEFT] = stick_debounce_dir(2,
				dir_neg(stick_stable[2], nx,
					JOY_DIR_PRESS, JOY_DIR_RELEASE));
			want[STICK_SRC_RIGHT] = stick_debounce_dir(3,
				dir_pos(stick_stable[3], nx,
					JOY_DIR_PRESS, JOY_DIR_RELEASE));
		} else {
			for (int i = 0; i < 4; i++) {
				stick_stable[i] = false;
				stick_debounce[i] = 0;
			}
		}

		if (menu_mode) {
			menu_tick(want, now);
			shop_cancel();
			vTaskDelay(pdMS_TO_TICKS(POLL_MS));
			continue;
		}

		if (!input_enabled) {
			for (int i = 0; i < SRC_COUNT; i++)
				if (src_down[i])
					set_source(i, false);
			if (enable_fallback_us == 0)
				enable_fallback_us = now + 30000000;
			else if (now >= enable_fallback_us)
				input_enabled = true;
			memset(pad_debounce, 0, sizeof(pad_debounce));
			memset(stick_debounce, 0, sizeof(stick_debounce));
			shop_cancel();
			vTaskDelay(pdMS_TO_TICKS(POLL_MS));
			continue;
		}
		enable_fallback_us = 0;

		if (cfg.mouse) {
			want[STICK_SRC_UP] = false;
			want[STICK_SRC_DOWN] = false;
			want[STICK_SRC_LEFT] = false;
			want[STICK_SRC_RIGHT] = false;
		}
		for (int i = 0; i < SRC_COUNT; i++)
			if (src_down[i] && !want[i])
				set_source(i, false);
		for (int i = 0; i < SRC_COUNT; i++)
			if (!src_down[i] && want[i])
				set_source(i, true);

		if (cfg.mouse)
			mouse_tick(joy_ok, nx, ny, now);
		shop_cycle_tick(now);
		repeat_keys(now);
		vTaskDelay(pdMS_TO_TICKS(POLL_MS));
	}
}

void gamepad_init(void)
{
	menu_queue = xQueueCreate(8, sizeof(uint8_t));
	cfg_settings = gamepad_default_settings();
#ifdef BOOT_MENU
	input_enabled = false;
#else
	input_enabled = true;
#endif
	xTaskCreatePinnedToCore(gamepad_task, "gamepad", 4096, NULL, 1, NULL, 0);
}

void gamepad_set_enabled(int enabled)
{
	input_enabled = enabled != 0;
}

void gamepad_set_settings(const struct gamepad_settings *settings)
{
	if (settings)
		cfg_settings = *settings;
}

#else /* !USE_GAMEPAD */

void gamepad_init(void)
{
}

void gamepad_set_enabled(int enabled)
{
	(void) enabled;
}

void gamepad_set_settings(const struct gamepad_settings *settings)
{
	(void) settings;
}

void gamepad_menu_ready(void)
{
}

int gamepad_menu_poll(void)
{
	return 0;
}

#endif /* USE_GAMEPAD */
