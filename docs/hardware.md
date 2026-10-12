# Hardware (rev A)

Two-layer, 76 × 45 mm, every non-sensor part AEC-Q100/Q101/Q200 and stocked at JLCPCB/LCSC.
Design files: `hardware/kicad/` (KiCad 8). Schematic: `hardware/kicad/preview/fuel-center-schematic.pdf`.

## Block diagram

```
12V_IGN ─[B340A]─[SM8S24A TVS]─[ferrite]─┬─ TPS7B6950-Q1 5 V ─┬─ S32K116 MCU (5 V)
 reverse   load dump                     │                    ├─ TCAN1042V-Q1 CAN transceiver
                                         │                    └─ TPS7B4250-Q1 tracking LDO ─ 5V_SENS → pressure sensor
                                         └─[ferrite]─ F_12V → flex sensor supply

Pressure Vout ─┬─ 47k pull-down ─ GND          (open wire reads 0 V = fault, never "0 psi")
               └─ 4.7k / 22 nF ─ BAT54S clamp ─ ADC
5V_SENS ─ 10k/10k ─ ADC                        (ratiometric check)
Flex out ─ 4.7k pull-up to 5 V ─ 10k / 1 nF ─ BAT54S clamp ─ SN74LVC1G17-Q1 Schmitt ─ FTM input capture
CAN_H/CAN_L ─ PESD2CAN ─ TCAN1042V-Q1          (split termination fitted as DNP)
```

The sensor 5 V comes from a short-circuit-protected tracking regulator, so a shorted sensor harness cannot take
down the MCU: the module keeps running and reports the fault.

## Connector J1 — TE Deutsch DT13-12PA

Mating plug DT06-12SA with W12S wedge.

| Pin | Signal | Goes to |
|---|---|---|
| 1 | VBAT_IN | Ignition-switched 12 V, fused 3 A |
| 2 | GND | Chassis ground |
| 3 | CAN_H | |
| 4 | CAN_L | |
| 5 | 5V_SENS | Pressure sensor B |
| 6 | P_SIG | Pressure sensor C |
| 7 | GND | Pressure sensor A |
| 8 | F_SIG | Flex sensor C |
| 9 | F_12V | Flex sensor A |
| 10 | GND | Flex sensor B |
| 11 | — | Not connected |
| 12 | GND | |

## Programming header J2 — TC2030-NL (Tag-Connect)

VCC, SWDIO, nRESET, SWCLK, GND, UART TX (115200 8N1). UART RX on test pad TP1.

## MCU pins (S32K116, LQFP-48)

| Signal | Pin | Function |
|---|---|---|
| CAN TX / RX | 3 / 4 | PTE5 / PTE4 |
| Flex input | 11 | PTD15 FTM0_CH0 |
| Pressure | 22 | PTC14 ADC0_SE12 |
| Sensor 5 V ÷ 2 | 21 | PTC15 ADC0_SE13 |
| Battery ÷ 11 | 20 | PTC16 ADC0_SE14 |
| 8 MHz crystal | 8 / 9 | PTB7 / PTB6 |
| UART TX / RX | 35 / 36 | PTA3 / PTA2 |

## Ordering

Everything JLCPCB needs is in `hardware/kicad/`. On jlcpcb.com:

1. Upload `fuel-center-gerbers.zip`.
2. PCB options: 2 layers, FR-4, 1.6 mm, 1 oz outer copper, surface finish **ENIG**. Leave the rest at defaults.
3. Turn on **PCB Assembly**: PCBA type Standard, top side.
4. Upload `jlc_bom.csv` and `jlc_cpl.csv`. Check the placement preview (rotations matched on rev A).
5. Add **conformal coating**. Ask in the order notes for J1, the J2 programming pads and TP1/TP2 to be kept
   clear, or the board cannot be programmed.
6. Decline part substitutions: JLCPCB's Basic parts are not automotive-qualified.

Before ordering, run `python3 hardware/check_stock.py <boards>`. If a part is short, use the alternate listed in
`hardware/BOM.csv`, or buy the part through JLCPCB Global Sourcing. If a part can no longer be bought, the design
is updated. `hardware/export.sh` re-runs ERC/DRC and regenerates the gerbers, zip and previews after a change.

## Installation

- Mount the module on the inner fender, firewall or in the cabin. Do not mount it on the engine or near the
  turbos or exhaust.
- Mount the flex sensor in the fuel **feed** line at a low point so it stays full with the pump off. A drained
  sensor reads air (0 %) until the pump primes it.
- Keep the pressure sensor's three wires in one run back to J1.
- Terminate CAN elsewhere unless this module ends the bus (then fit R8/R9/C17).
- Underhood: use a sealed enclosure with a vent.

## Limitations

- Built from automotive-grade parts; the assembled module is not an automotive-certified product.
- A pressure sensor output stuck at an in-range voltage looks like a valid reading. Use engine-side plausibility
  checks (e.g. pressure vs. duty and boost) for any protection strategy that relies on pressure.
