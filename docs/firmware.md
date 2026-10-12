# Firmware

| Directory | Contents |
|---|---|
| `firmware/core/` | Portable C: CRC-8, flex decoder, pressure decoder, frame builder, config record |
| `firmware/app/` | Main loop (10 ms data frame, 100 ms diagnostics); `bsp.h` is the hardware interface |
| `firmware/s32k116/` | NXP S32K116 board support, startup and linker script |
| `firmware/sim/` | Host simulator that replays a fault scenario |
| `firmware/test/` | Host unit tests |
| `firmware/tools/` | Image signing |

## Build

```sh
make -C firmware test                 # host unit tests
make -C firmware sim                  # print simulated CAN frames (candump format)
make -C firmware/s32k116              # target build -> firmware/s32k116/build/fuel-center.{elf,hex,bin}
make -C firmware/s32k116 TOOLCHAIN=/path/to/arm-gnu-toolchain/bin
```

The target build needs the Arm GNU toolchain and Python 3. The build signs the image: the CRC-32 of flash
0x00000-0x1FFFB is written to 0x1FFFC and checked at every boot. Always flash the signed `fuel-center.*` files;
an unsigned image reports a permanent module fault.

## Flash

SWD via a TC2030-NL cable on J2, with any CMSIS-DAP, J-Link or PEmicro probe:

```sh
pyocd flash -t s32k116 firmware/s32k116/build/fuel-center.hex
```

After reset the UART prints `fuelcan boot`, then 0x5A6 appears at 100 Hz with status 0x01 (not ready) for 500 ms.

## Target configuration

| Block | Setting |
|---|---|
| Clocks | FIRC 48 MHz core, 24 MHz bus; 8 MHz crystal for CAN and capture |
| Flex input | FTM0 CH0 dual-edge capture at 1 MHz, median of 5 periods/pulses, 100 ms no-signal timeout |
| ADC | 12-bit, 16× averaging; pressure, sensor 5 V, battery, die temperature, bandgap, VREFH/VREFL self-check |
| CAN | FlexCAN 500 kbit/s, 81 % sample point; one TX mailbox per ID; a stalled mailbox re-initialises the controller |
| Watchdog | 50 ms; a watchdog reset sets the module fault bit for 5 s |
| Config | 8-byte record in FlexNVM data flash, outside the signed image |

## Self-tests

At boot: RAM march test, flash CRC, stack canary. Every 10 ms: stack canary, ADC reference check, 5 V rail
4.5-5.5 V, sensor 5 V tracking within 5 %, CAN mailbox health. Any failure sets the module fault bit and the
fail-safe values on 0x5A6; details on 0x5A7 page 1 (see [protocol.md](protocol.md)).

## Porting

Everything hardware-specific sits behind `firmware/app/bsp.h`. Implement it for another MCU and the core and
application build unchanged.
