if(NOT DEFINED BOARD)
    set(BOARD "jc3248w535")
endif()

set(CPU_SRC "../../i386.c")

idf_component_register(SRCS "esp_main.c"
"../../adlib.c"
"../../fmopl.c"
"../../fpu.c"
"${CPU_SRC}"
"../../i8042.c"
"../../i8254.c"
"../../i8259.c"
"../../ide.c"
"../../ini.c"
"../../pc.c"
"../../misc.c"
"../../pci.c"
"../../vga.c"
"../../vgafont.c"
"../../ne2000.c"
"../../i8257.c"
"../../sb16.c"
"../../pcspk.c"
"wifi.c"
"lcd_axs15231b.c"
"lcd_st7701.c"
"lcd_elecrow7s3.c"
"lcd_nv3041a.c"
"lcd_serial.c"
"../../term.c"
"sdmmc_crc_wrap.c"
"i2s.c"
"storage.c"
"usb_hid.c"
"usb_input.c"
INCLUDE_DIRS "")

target_add_binary_data(${COMPONENT_LIB} "fb_bios.bin" BINARY)
target_add_binary_data(${COMPONENT_LIB} "fb_vga.bin" BINARY)

# Boards whose app partition cannot hold the 2.65MB built-in DOS disk
# (4MB-flash S3, 1792K app on P4) load disks from SD only. fb_dos.img
# contains a DOS installation and is not distributed with this repository.
if(NOT BOARD STREQUAL "jc4827w543" AND NOT BOARD STREQUAL "jc4880p443")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fb_dos.img")
        target_add_binary_data(${COMPONENT_LIB} "fb_dos.img" BINARY)
    else()
        message(STATUS "fb_dos.img not present: no built-in DOS fallback")
        add_definitions(-DBOARD_NO_BUILTIN_DOS_IMG)
    endif()
endif()

message(STATUS "Building for BOARD=${BOARD}")

add_definitions(-include ${CMAKE_CURRENT_SOURCE_DIR}/board_${BOARD}.h)

# Linker wrap for SPI SD CMD59 workaround (elecrow7s3 only)
if(BOARD STREQUAL "elecrow7s3")
    target_link_options(${COMPONENT_LIB} INTERFACE
        -Wl,--wrap=sdmmc_init_spi_crc
        -Wl,--undefined=__wrap_sdmmc_init_spi_crc)
endif()

add_definitions(-DI386_ENABLE_FPU)
add_definitions(-DUSE_FMOPL)

set_source_files_properties("../../adlib.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("../../fmopl.c" PROPERTIES COMPILE_FLAGS "-Wno-all -fsingle-precision-constant")
set_source_files_properties("../../fpu.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("../../i386.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3")
set_source_files_properties("../../fast/fast.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3")
set_source_files_properties("../../i8042.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("../../vga.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3")
set_source_files_properties("../../pc.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3")
set_source_files_properties("../../ne2000.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("../../sb16.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("../../i8257.c" PROPERTIES COMPILE_FLAGS "-Wno-all")

if(NOT EXISTS "../../fmopl.inc")
    message(FATAL_ERROR "Please run `make prepare` first")
endif()
