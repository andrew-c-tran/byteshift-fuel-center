# Sensors

## Flex fuel — GM 13577429 (Continental SE1004S)

Also sold as 13577379 / 13577394; electrically identical. Connector Aptiv GT150 3-way.

| Pin | Function |
|---|---|
| A | +12 V (9-18 V) |
| B | Ground |
| C | Open-collector output (pull-up on the module) |

- **Ethanol:** frequency, 50 Hz = 0 %, 150 Hz = 100 %.
- **Temperature:** low pulse width, 1 ms = −40 °C, 5 ms = 125 °C, linear.
- **Faults:** ~180 Hz = contaminated fuel or internal fault; no pulses = open wire, short or dead sensor.
- The sensor is capacitive and needs no flow. In air it reads ~50 Hz (0 %).
- Counterfeits are common and read 20-30 % on E0. Buy ACDelco or Continental from a real distributor.

Module limits: valid 45-155 Hz, contaminated 165-200 Hz, pulse width 0.9-5.1 ms.

## Fuel pressure — Honeywell MIPAN2XX150PSAAX (EFI Hardware H-PS150G-HMIP)

0-150 psi sealed gauge, Metri-Pack 150 3-way, 1/8-27 NPT, stainless wetted parts.

| Pin | Function |
|---|---|
| A | Ground |
| B | +5 V |
| C | Output, ratiometric 10-90 % of supply (0.5-4.5 V at 5 V) |

- **Transfer:** P [psi] = 150 × (Vout / Vs − 0.10) / 0.80. At 5.00 V: P [kPa] = 258.55 × Vout − 129.3.
- **Faults:** the sensor drives 2.5 % Vs on under-pressure and 97.5 % Vs on internal fault or over-pressure.

Module limits: valid 5-95 % of supply, supply 4.5-5.5 V, below −14 kPa = fault, above 830 kPa flagged implausible.

## Why the fail-safe values

Over-reported ethanol makes the ECU add fuel and timing for a fuel that isn't there: lean and over-advanced, which
causes detonation. Phantom pressure defeats fuel-pressure engine protection. The module therefore fails to
**0 % ethanol** and **0 kPa** and flags the error.

## References

- AEM 30-2201 flex fuel kit instructions (same sensor):
  https://documents.aemelectronics.com/techlibrary_30-2201_flex_fuel_content_sensor_kit.pdf
- Honeywell MIP series datasheet 32353264.
- rusEFI flex sensor validation: https://github.com/rusefi/rusefi (firmware/controllers/sensors/flex_sensor.h)
