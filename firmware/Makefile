# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ByteShift Labs LLC
CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O2 -Icore
CORE := core/fuel_crc8.c core/fw_crc32.c core/flex_decode.c core/pressure.c core/fuel_frame.c core/fuel_config.c
test: build/test_core
	./build/test_core
build/test_core: $(CORE) test/test_core.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $^ -lm
clean:
	rm -rf build
.PHONY: test clean
sim: build/sim
	./build/sim
build/sim: $(CORE) app/main.c sim/bsp_sim.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Iapp -DSIM_MAX_MS=62000 -o $@ $^ -lm
.PHONY: sim
