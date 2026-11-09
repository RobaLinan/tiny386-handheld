// Guition JC4827W543N: ESP32-S3-N4R8, 480x272 QSPI NV3041A panel.
#define BUILD_ESP32

#define IRAM_ATTR_CPU_EXEC1 IRAM_ATTR

#define BPP 16
#define SCALE_3_2
#define FULL_UPDATE
#define USE_LCD_NV3041A
#define LCD_WIDTH 480
#define LCD_HEIGHT 272

// 4MB flash: the 2.65MB built-in DOS image does not fit next to the firmware,
// so this board boots only from the SD card.
#define BOARD_NO_BUILTIN_DOS_IMG

// TF card is wired to the third SPI controller (GPIO matrix); the QSPI panel
// occupies SPI2, so SD must not share its bus.
#define SD_SPI_HOST 2
#define SD_SPI_CS   10
#define SD_SPI_SCK  12
#define SD_SPI_MOSI 11
#define SD_SPI_MISO 13
#define SD_SPI_FREQ_KHZ 20000

#define USE_USB_SERIAL_KBD
#define LCD_NO_BAND_DELAY
#define LCD_SINGLE_FRAME

#define I2S_MCLK I2S_GPIO_UNUSED
#define I2S_BCLK 42
#define I2S_WS   2
#define I2S_DOUT 41
