cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(blueline)

target_sources(app PRIVATE
	src/main.c
	src/fsm_main.c
	src/pico_io.c
	src/bt_uart.c
	src/bt_fsm.c
	src/bm_83.c
	src/slic.c
)
