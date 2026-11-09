#include <Arduino_GFX_Library.h>

static Arduino_ESP32QSPI *bus;
static Arduino_NV3041A *panel;
static Arduino_Canvas *canvas;
static bool initialized = false;

extern "C" void gfx_lcd_init(void) {
    bus = new Arduino_ESP32QSPI(45, 47, 21, 48, 40, 39);
    panel = new Arduino_NV3041A(bus, GFX_NOT_DEFINED, 0, true);
    canvas = new Arduino_Canvas(480, 272, panel);
    if (!canvas->begin()) {
        return;
    }
    // Backlight
    pinMode(1, OUTPUT);
    digitalWrite(1, HIGH);
    initialized = true;
}

extern "C" void gfx_lcd_draw(int x, int y, int w, int h, uint16_t *pixels) {
    if (!initialized) return;
    canvas->draw16bitRGBBitmap(x, y, pixels, w, h);
    canvas->flush();
}
