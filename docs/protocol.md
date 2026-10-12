# CAN protocol

500 kbit/s, 11-bit IDs, DLC 8. Byte H of every frame is a CRC-8 over bytes A-G.

| ID | Direction | Period | Purpose |
|---|---|---|---|
| 0x5A6 | module → bus | 10 ms | Fuel data |
| 0x5A7 | module → bus | 100 ms | Diagnostics (3 pages) |
| 0x5A8 | tool → module | on demand | Installer configuration |

## CRC-8

Polynomial 0x1D, init 0xFF, no reflection, no final XOR, computed over bytes A-G (byte H skipped).
This is *not* catalogue CRC-8/SAE-J1850, which XORs 0xFF at the end.

```c
static const uint8_t LO[16] = {0x00,0x1D,0x3A,0x27,0x74,0x69,0x4E,0x53,0xE8,0xF5,0xD2,0xCF,0x9C,0x81,0xA6,0xBB};
static const uint8_t HI[16] = {0x00,0xCD,0x87,0x4A,0x13,0xDE,0x94,0x59,0x26,0xEB,0xA1,0x6C,0x35,0xF8,0xB2,0x7F};

uint8_t crc8(const uint8_t *buf, uint8_t len, uint8_t skip, uint8_t crc)  /* crc8(frame, 8, 7, 0xFF) */
{
    for (uint8_t j = 0; j < len; j++) {
        if (j == skip) continue;
        uint8_t d = buf[j] ^ crc;
        crc = HI[d >> 4] ^ LO[d & 0x0F];
    }
    return crc;
}
```

Test vector: `12 34 56 78 9A BC DE xx` → `xx = 4E`.

## 0x5A6 — fuel data

| Byte | Content | Encoding | Fail-safe value |
|---|---|---|---|
| A | Ethanol content | 0-100 % | 0 |
| B | Fuel temperature | °C + 40 (0-165) | 165 (= 125 °C) |
| C-D | Fuel pressure | kPa gauge, unsigned 16-bit big-endian | 0 |
| E | Status | bits below | |
| F | Counter / version | bits 7-4 rolling counter 0-15, bits 3-0 protocol version (= 2) | |
| G | Raw flex frequency | Hz, 0 = no signal, 255 = >254 Hz | |
| H | CRC-8 | | |

Status byte E:

| Bit | Meaning |
|---|---|
| 7 | Ethanol sensor error (no signal, <45 Hz, >155 Hz, contaminated) |
| 6 | Fuel temperature error (pulse width outside 0.9-5.1 ms) |
| 5 | Fuel pressure sensor error (signal outside 5-95 % of supply, sensor supply out of range) |
| 4 | Ethanol contaminated / sensor self-diagnosed (165-200 Hz); bit 7 also set |
| 3 | Ethanol slew-limited (value is ramping toward the raw reading) |
| 2 | Pressure implausible (>830 kPa); value still reported |
| 1 | Module internal fault; all three values are replaced by their fail-safe values |
| 0 | Not ready (first 500 ms after power-up) — ignore values |

Example: 85 %, 90 °C, 475 kPa, no faults, counter 0, 135 Hz: `5A6#558201DB000287AF`

### Rules

- **Fail safe.** When an error bit is set, the matching value is also replaced by its fail-safe value, so a receiver
  that ignores the status byte still sees low ethanol and no pressure.
- **Sticky errors.** Error bits stay set for at least 1 s after the last bad sample.
- **Staleness.** The counter increments every frame. Treat the module as dead if the counter has not changed for 50 ms.
- **Not fitted.** A sensor configured as not fitted (0x5A8) reports its fail-safe value with no error bit.
- **Ethanol slew.** Rises are limited to 3 %/s; falls are immediate. After power-up or a fault the value starts
  at 0 and jumps to the raw reading once it has been valid and within 3 % for 1 s (an E85 car reads 85 % about
  1.5 s after key-on). A raw reading above the reported value that holds within 3 % for 5 s is also adopted in one step,
  which covers a sensor that drained while parked and refills on pump prime.

## 0x5A7 — diagnostics

Pages cycle 0 → 1 → 2. Byte G = firmware version << 4 | page. Firmware version is currently 3.

| Page | A | B | C | D | E | F |
|---|---|---|---|---|---|---|
| 0 | Pressure signal mV (BE16) | | Sensor 5 V mV (BE16) | | Supply, 0.1 V | Board temp °C + 40 |
| 1 | CAN TX error count | CAN RX error count | Self-test bits | CAN restarts | Uptime s (BE16) | |
| 2 | Fitted mask | Config source | Last config result | Config writes since boot | 0 | 0 |

Page 1 self-test bits (1 = problem, *latched until power cycle*): 0 watchdog reset\*, 1 ADC self-test,
2 5 V rail outside 4.5-5.5 V, 3 CAN bus-off or stalled TX, 4 flash CRC mismatch\*, 5 RAM test\*, 6 stack overflow\*,
7 module fault currently active. Bits 1-6 force the module fault bit in 0x5A6. Counters saturate at 255.

Page 2: fitted mask bit 0 = flex sensor, bit 1 = pressure sensor. Config source 0 = defaults, 1 = stored,
2 = stored record corrupt (defaults used). Last config result 0 = none received, 1 = stored, 2 = frame rejected,
3 = applied but flash write failed.

## 0x5A8 — installer configuration

Tells the module which sensors are installed. Stored in data flash and applied immediately; confirm on 0x5A7 page 2.
Default: both fitted.

| Byte | Content |
|---|---|
| A | 0xC5 |
| B | Fitted mask: bit 0 flex sensor, bit 1 pressure sensor, other bits 0 |
| C | 0xFF − B |
| D-G | 0 |
| H | CRC-8 |

| Configuration | Frame |
|---|---|
| Both sensors | `5A8#C503FC0000000025` |
| Flex only | `5A8#C501FE000000007D` |
| Pressure only | `5A8#C502FD0000000009` |

Frames that fail any check are ignored.
