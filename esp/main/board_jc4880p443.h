// requires esp-idf v6.0.x
#define BUILD_ESP32

/* 16M: Theme Hospital needs ~10M guest RAM + 768K VGA (12M/768K recommended). */
#define PSRAM_ALLOC_LEN (16 * 1024 * 1024)
#define BOARD_NO_BUILTIN_DOS_IMG

/* USB-Serial/JTAG doubles as the keyboard console (same as the 543 port). */
#define USE_USB_SERIAL_KBD

/* DOS/VGA pictures are 640x400; stretch them to the 800x480 glass with PPA. */
#define LCD_FILL_SCREEN
#define LCD_FILL_DIRECT
#define LCD_NO_BAND_DELAY
#define LCD_FPS_OVERLAY
#define BOOT_MENU

/* 3x4 key matrix + analog stick on the handheld PCB. */
#define USE_GAMEPAD

/* Battery divider node: JP1 pin 9, BAT+ --10k--+--10k-- GND. */
#define BATTERY_ADC_GPIO 51

/* Gauge top: a full pack reads ~4.15 V under normal load (USB and battery),
 * so 4.10 V is treated as 100% to keep some margin for load sag and noise. */
#define BATTERY_FULL_MV 4100

// XXX: ld reports "error: Total discarded sections size is X bytes"
//#define IRAM_ATTR_CPU_EXEC1 IRAM_ATTR
#define IRAM_ATTR_CPU_EXEC1

#define BPP 16
#define FULL_UPDATE
#define SWAPXY
#define USE_LCD_ST7701
#define LCD_WIDTH 800
#define LCD_HEIGHT 480

#define SD_CLK 43
#define SD_CMD 44
#define SD_D0 39
#define SD_D1 40
#define SD_D2 41
#define SD_D3 42
#define SD_PWR_CTRL_LDO_IO_ID 4

#define USE_HOSTED_WIFI

#define MIXER_BUF_LEN 512
#define I2S_MCLK 13
#define I2S_BCLK 12
#define I2S_WS   10
#define I2S_DOUT 9
#define I2S_NUM  0

#define USE_ES8311
#define ES8311_PA 11
#define ES8311_I2C_NUM 0
#define ES8311_I2C_SDA 7
#define ES8311_I2C_SCL 8
