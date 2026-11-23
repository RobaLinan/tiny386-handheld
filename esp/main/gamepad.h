#ifndef GAMEPAD_H
#define GAMEPAD_H

enum pad_button {
	PAD_UP,
	PAD_DOWN,
	PAD_LEFT,
	PAD_RIGHT,
	PAD_A,
	PAD_B,
	PAD_C,
	PAD_D,
	PAD_SELECT,
	PAD_OK,
	PAD_L2,
	PAD_R2,
	PAD_BUTTON_COUNT,
};

struct gamepad_config {
	int mouse;                 /* 1: the stick drives the PS/2 mouse */
	int shop_cycle;            /* 1: the stick button cycles modifier+F1..F10 */
	int key[PAD_BUTTON_COUNT]; /* PS/2 set-1 scancode, 0 = unmapped */
	int mouse_l;               /* PAD_* used as left click, -1 = none */
	int mouse_r;
};

struct gamepad_settings {
	int invert_x;
	int invert_y;
};

struct gamepad_config gamepad_default_config(void);
struct gamepad_settings gamepad_default_settings(void);

/* PS/2 set-1 scancode for a menu.ini key name (0 = None, -1 = unknown).
 * Accepts names such as Enter/Up/F1/a/0x1C. */
int gamepad_parse_key(const char *name);

/* PAD_* index for a menu.ini button name, -1 if unknown. */
int gamepad_button_index(const char *name);

void gamepad_init(void);

/* Apply [gamepad] settings early (the boot menu uses the stick too). */
void gamepad_set_settings(const struct gamepad_settings *settings);

/* Gate key/mouse injection (e.g. hold off while autolaunch types). */
void gamepad_set_enabled(int enabled);

/* Boot-menu events. poll() returns 0 when the queue is empty. */
enum gamepad_menu_event {
	GAMEPAD_MENU_LEFT = 1,
	GAMEPAD_MENU_RIGHT,
	GAMEPAD_MENU_START,
};

void gamepad_menu_ready(void);
int gamepad_menu_poll(void);

#endif /* GAMEPAD_H */
