#define BUILD_ESP32
#define IRAM_ATTR_CPU_EXEC1 IRAM_ATTR

#define BPP 16
#define FULL_UPDATE
#define USE_LCD_SERIAL
#define LCD_WIDTH 640
#define LCD_HEIGHT 480

/* Seeed XIAO Expansion Board microSD (SPI) */
#define SD_SPI_MOSI 9
#define SD_SPI_MISO 8
#define SD_SPI_SCK 7
#define SD_SPI_CS 3
#define SD_SPI_FREQ_KHZ 5000
