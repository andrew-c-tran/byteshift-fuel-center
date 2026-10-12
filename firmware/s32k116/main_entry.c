/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ByteShift Labs LLC */
/* Reset_Handler -> SystemInit -> init_data_bss -> main -> app_main (never returns). */
int app_main(void);
int main(void) { return app_main(); }
