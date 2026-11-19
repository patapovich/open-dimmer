# PY32F002A firmware of the AVATTO DMS16 dimmer (WiFi DMS16-W1 dump), analysis 2026-11-16

Source: elektroda topic4066696 (PCB MK-WC1H16A-A V1.1 20230823, same board as ZDMS16 with the radio
module swapped). Dump: mainflash.bin (20 KB, all used), optionbytes.bin. Disassembled with capstone
(Thumb, base 0x08000000). Copies of the dump, the option bytes, the stock ZTU image and the
analysis scripts are kept outside the repo; the dumps are public (see "How the WiFi PY32 dump
was made").

## Option bytes
- OPTR = 0xBEAA (complement 0x4155): RDP = 0xAA → level 0, no readout protection.
- nBOOT1 = 1 → at reset BOOT0 (pin 5, PB6/PF4) high = ROM bootloader (ISP on USART PA2/PA3), low = flash.

## HAL entry points
- HAL_GPIO_Init   0x08000CE8   (GPIO_InitTypeDef on stack: Pin, Mode, Pull, Speed, Alternate)
- HAL_GPIO_ReadPin 0x08000E9C
- key read helper 0x08002574 (index 0 → PB1, index 2 → PA6; 1 = pressed when the pin is low)

## Pin use (TSSOP20 numbers)
| Pin | Port | Use |
|---|---|---|
| 13 / 14 | PA2 / PA3 | AF1 USART1 TX/RX → radio module (WiFi CBU / Zigbee ZTU) |
| 19 | PB1 | input, pull-up, active low → wall switch input (S1 or S2) / key 0 |
| 18 | PB2 | input, pull-up, active low → wall switch input (S2 or S1) |
| 6 | PA6 | input, pull-up, active low → module button (key 2) |
| 15 | PA4 | EXTI rising+falling, pull-up → mains zero-cross |
| 11, 12 | PA0, PA1 | AF13 (timer) → triac gate drive / phase control |
| 4 | PB3 | AF1 (timer) |
| 7 | PA5 | push-pull output (LED?) |
| 2 / 3 | PA13 / PA14 | SWD at reset, reconfigured as push-pull outputs at run time (0x08003B4C) → SWD needs connect-under-reset (NRST pin 10) |
| 5 | PB6/PF4-BOOT0 | never configured by the firmware |

Timers TIM1 and TIM16 are used heavily (phase-angle control); USART1 once.

## Conclusions
- Neither the module button nor S1/S2 is on BOOT0 (confirmed on HW 2026-11-16: button held at
  power-on → MCU ran its own firmware). The button is PA6, S1/S2 are PB1/PB2.
- Remote PY32 ISP needs two wires to free ZTU GPIOs: pin 5 (BOOT0) and pin 10 (NRST); a power
  cycle is no substitute for NRST (see below). The bootloader then talks on the existing UART.
- Readout is not protected: the stock firmware can be read back over SWD or ISP for a rollback copy.

## ZTU module pinout (Tuya ZTU datasheet, FCC ID 2ANDL-ZTU, top view)
- Left edge, top → bottom: NC, D4, C1, RST, C4, **B7**, **B1**  (B1 = UART TX to MCU, B7 = RX)
- Right edge, top → bottom: D3, D7, C0, SWS, B6 (ADC), A0, A1
- Bottom edge (long pads), left → right: VCC, GND, B5, B4, D2, C3, C2
- ZDMS16-2: ZTU sits on a riser with a 5-pin header TX, RX, NC, VCC, GND; middle (NC) unused
  (issue #387 photos). Free GPIOs for extra wiring: C2/C3/D2/B4/B5 (bottom), A0/A1/C0/D7/D3 (right),
  C4/C1/D4 (left). Avoid SWS and RST.

## Remote PY32 programming, what it takes
- BOOT0 alone is not enough: the PY32 samples BOOT0 within ms of power-up, before the Telink
  firmware can drive a GPIO, so a power cycle cannot be used to enter the ROM bootloader.
  BOOT0 + NRST (pin 10) both wired to ZTU GPIOs would be needed (hold BOOT0 high, pulse NRST).
- Cleaner: no permanent wires. Flash a custom PY32 firmware (now Open Dimmer) once on the bench
  (SWD on CN2 + NRST for connect-under-reset), and give it a UART command that jumps to the ROM
  bootloader (system memory) — later updates go Zigbee → Telink → UART ISP. A failed update then
  needs physical access again unless that firmware keeps a small fail-safe loader.

## Update support in the WiFi PY32 firmware (checked 2026-11-16)
- Product info: {"p":"chfpey4klfcp1ipl","v":"1.0.3","m":0}; no MCU OTA fields.
- Frame parser 0x08004050: needs 55 AA 00 (version byte 0, no seq field), data ≤ 31 bytes
  (frame ≤ 0x26, RX buffer 31). The exact limits, and the frames that wedge it: "Stock MCU parser
  limits" below. Commands handled (dispatcher 0x080025D0): 0x00 heartbeat,
  0x01 product info, 0x02 working mode, 0x03 WiFi status, 0x04/0x05 reset, 0x06 DP write,
  0x08 query, 0x0E, 0x34. No 0x0A/0x0B (WiFi MCU OTA): no update over UART.
- Flash writes (HAL unlock 0x08000A54) only touch the last 128-byte page 0x08004F80 → settings.
  No bootloader, no second vector table, no reference to system memory 0x1FFF0000. Flash is
  full (2960 bytes free at 0x080043F0), so there is no room for a second image either.
- The Zigbee unit speaks 55 AA 02 <seq:2> (Tuya Zigbee framing) → its PY32 firmware is a
  different build; this dump says nothing about MCU OTA there.
- Tuya Zigbee UART protocol has MCU OTA: 0x0B version query (module→MCU, reply 1 byte),
  0x0C notify (PID 8, version 1, size 4, checksum 4), 0x0D MCU pulls blocks (PID, version,
  offset 4, size ≤ 50), 0x0E result. Cheap probe: send 0x0B from our ZTU firmware on the
  spare module and log the answer. Definitive: read the Zigbee PY32 over SWD on the bench.
- Stock ZTU image of _TZE284_jtbgusdc: the public 1 MB dump (see "How the WiFi PY32 dump was
  made").

## 0x0B probe on the Zigbee unit (spare <spare IEEE>, fw 1.1.3-e616064b, 2026-11-16 23:13)
- Diag control 0x8B sent 0x0B with no data; the MCU answered 0x0B in 3 ms with data 0x47
  (71). Tuya encoding xx.yy.zzzz → MCU firmware 1.0.7. (The WiFi dump was 1.0.3, other build.)
- So the Zigbee PY32 runs the Tuya Zigbee MCU SDK and knows the version query that belongs to
  the MCU OTA set. That hints at, but does not prove, 0x0C/0x0D/0x0E support.
- Not tried: 0x0C. If the MCU implements OTA by erasing its app and then pulling blocks
  (20 KB flash leaves no room for a second copy), an aborted or malformed start could leave
  the dimmer in its loader. Read the Zigbee PY32 over SWD first to see the loader and the
  image format it expects.

## Stock ZTU firmware (_TZE284_jtbgusdc, the 1 MB dump) — MCU OTA path, 2026-11-16
Disassembled with tc32-elf-objdump (the Telink toolchain from the fork's tools.mk). Layout:
Tuya bootloader 0x0–0x6A0C, app at 0x8000 (KNLT, 0x42214 bytes).
- UART RX dispatcher 0x0000C858, jump table 0x48808 (cmd 1..0x3A): handles MCU→module
  0x01, 0x03, 0x05, 0x06, 0x08, **0x0B, 0x0D, 0x0E**, 0x20, 0x24, 0x25, 0x26, 0x27, 0x29,
  0x2C, 0x36, 0x37, 0x38, 0x3A. No handler for the MCU's 0x0C reply.
- EF00 (manuSpecificTuya) command dispatcher 0x0000B5FC, jump table 0x484C0:
  0x00 → UART 0x04 (DP write), 0x03/0x08 → UART 0x28, **0x10 → UART 0x0B**,
  **0x12 → UART 0x0C**, **0x14 → UART 0x0D reply**, 0x24 → UART 0x24, 0x25 → UART 0x25,
  0x61/0x62 (unknown, 0xB1E4/0xB290).
- The module is a transparent pipe; the gateway serves the MCU image:
  gateway EF00 0x10 → UART 0x0B; MCU 0x0B answer → EF00 0x11 (0xB970).
  gateway EF00 0x12 {seq, PID 8, version, size 4, crc 4} → UART 0x0C, payload copied
  (builder 0xB074, data ≤ 246).
  MCU UART 0x0D {PID, version, offset, size} → EF00 0x13 to the gateway (0xC4D4, frame
  ≤ 61 bytes).
  gateway EF00 0x14 → UART 0x0D reply, copied (0xB12C).
  MCU UART 0x0E → EF00 0x15 + UART ack.
- zigbee-herdsman defines these EF00 commands (mcuVersionRequest 0x10, mcuOtaNotify 0x12,
  mcuOtaBlockDataRequest 0x13, mcuOtaBlockDataResponse 0x14, mcuOtaResult 0x15).
- Consequence: MCU OTA is a real Tuya feature of this device. Our firmware could offer the
  same pipe, and a Z2M script could serve the blocks. Missing piece: an MCU image the PY32
  loader accepts (format, CRC); only an SWD read of the Zigbee PY32 shows that.

## WiFi PY32 firmware, second pass (2026-11-16 late)
Update side, all paths checked:
- UART commands: 0x00–0x05 (switch table), 0x06, 0x08, 0x0E (WiFi test result → indicator,
  0x08004038), 0x34 (extended, sub 1–5: status answers, sub 5 arg 2 = factory defaults
  0x080036F4: brightness 1000, min 200, max 1000). No 0x0A/0x0B, no file/OTA subcommand.
- Flash writes: the HAL erase/program code has exactly two callers, 0x0800379C (program) and
  0x08003814 (erase), both on the settings page 0x08004F80. No other address is ever written.
- No option-byte access (no OPTKEYR keys), no jump to system memory, one vector table at
  0x08000000 (VTOR = flash base). The firmware cannot reflash itself or switch itself into
  the ROM bootloader. WiFi build: updates only by SWD, or ROM ISP with BOOT0 high at reset.
- Eight quick wall-switch toggles: LED blinks 6 × 100 ms and 0x08003404 runs (pairing/reset
  request to the radio module). Counter 0x200000C4.

Dimming engine (the reference for Open Dimmer):
- Clock 24 MHz. TIM16: PSC 23, ARR 19 → 50 kHz tick; its update ISR counts ticks between
  zero crosses (0x20000298) and sets a 10 ms flag every 500 ticks.
- Zero cross: PA4 EXTI both edges → ISR 0x08000C40. Each crossing: TIM1 CNT = 0, TIM1 ARR =
  measured half period (≈ 500 ticks of 20 µs at 50 Hz), then each channel's current value
  moves toward its target by `step` and is applied.
- Output: TIM1 PSC 479 (20 µs), PWM mode 1, polarity high, CH4 = channel 0 (PA1), CH3 =
  channel 1 (PA0). Compare = ARR × value / 1000 → output high from the zero cross for
  value/1000 of the half cycle = trailing-edge (reverse phase) control if the gate drive
  does not invert. Resolution ≈ 500 steps, linear in time; no curve or gamma table.
- Target = DP2 brightness (10..1000) as is; DP3 min / DP5 max only bound hold-dimming.
  Step 5 per half cycle normally (0→1000 in 2 s), 1 while the wall switch is held.
- DPs: 1 on, 2 brightness, 3 min, 4 switch type (1: follow the input level), 5 max,
  6 countdown, 14 power-on behaviour. Channel struct 0x20000274 + 16·ch (current +4,
  target +6, step +8, flags +9..+0xC); settings 0x200002A5 + 10·ch.

## 0x0C probe, wrong PID (spare, fw 1.1.3-5a7cbaa9, 2026-11-16 23:52)
- Sent 0x0C {PID "xxxxxxxx", version 0x48, size 1024, crc 0} through diag attribute 0xff4e.
- No answer at all in 40 s: no 0x0C reply (doc says 0x00/0x01), no 0x0D block request,
  no 0x0E. The MCU kept working (DP dump right after: DP1 0, DP2 829, DP3 201, DP5 1000).
- Reading: 0x0C is most likely not handled (a Tuya-SDK MCU with OTA should answer 0x01 to a
  wrong PID). Not proof: an implementation could drop a wrong PID silently. Remaining test:
  the right PID (probably "jtbgusdc", the _TZE284_ suffix); if that is ignored too, there is
  no MCU OTA. Risk with the right PID: if OTA exists, the MCU starts asking for blocks; we
  answer none, so it should time out, but an implementation that erases on notify would
  leave the spare needing SWD.

## 0x0C probe, right PID (spare, 2026-11-16 23:54)
- 0x0C {PID "jtbgusdc", version 0x48, size 1024, crc 0}: silent again for 60 s (no 0x0C
  reply, no 0x0D, no 0x0E); DP dump afterwards normal.
- CONCLUSION: the Zigbee PY32 (fw 1.0.7) has no MCU OTA; 0x0B is answered, 0x0C ignored.
  Same as the WiFi build. The stock ZTU's OTA pipe is generic Tuya module code.
  PY32 updates are physical only: SWD (connect under reset), or ROM ISP with BOOT0 + NRST.
  Residual doubt: only if the MCU's product key differs from "jtbgusdc" (not read out; the
  diag log keeps only the first 4 bytes of a 0x01 answer).

## How the WiFi PY32 dump was made (elektroda topic4066696, post #64, divadiow, 19 Sep 2026)
- OpenOCD + CMSIS-DAP on an STM32F103 "blue pill" flashed with RadioOperator's
  F103-DAP-SWO-CDC-BLUEPILL-SWD_PB8PB9 firmware (SWD on PB8/PB9). Read: main flash 20 KB,
  system ROM 3584 B (0x1FFF0000), UID / option bytes / factory config 128 B each. PowerShell
  wrapper Dump-PY32F002A.ps1 attached (login needed). Wiring not shown in the public text.
- Earlier (post #1, Jul 2024) the same author could not connect with a J-Link, "even with
  NRST-reset pulled low". BOOT0 is not mentioned.
- Files: github.com/openshwprojects/FlashDumps/tree/main/IoT/TuyaMCUBins/FULL; the stock
  ZTU of our _TZE284_jtbgusdc is IoT/TLSR825X/Tuya_AVATTO_ZDMS16-2_TS0601_TZE284_jtbgusdc_ZTU.bin.

## Board headers (elektroda topic4066696 first post, image 6256248900_1721713000.png, WiFi board)
- CN2 (4 pads next to the PY32) = SWD header: SWD (PA13, pin 2), SWC (PA14, pin 3), GND (pin 1),
  VCC (pin 20), left to right in the close-up. Power + SWD for the bench dump all on CN2.
- CN3 (5 pins, bottom edge) = radio module header: WIFI TX, WIFI RX, NC, VCC, GND.
- BOOT0 = PY32 pin 5 (pin row 1 VSS, 2 SWD, 3 SWC, 4 PB3, 5 BOOT0).
- WiFi build main(): HAL init, clock, HAL_Delay(1500), then GPIO init (0x08003B24) that turns
  PA13/PA14 into outputs → SWD attach + halt within ~1.5 s of power-up works without BOOT0
  (`tools/swd/dump_py32_powerup.sh`; BOOT0 variant `tools/swd/dump_py32.sh`).

## SWD read of the Zigbee PY32 (spare, fw 1.1.3-c28a892e, 2026-11-17)
- Pico/uPesy RP2040 (not pin-compatible with a Pico) + debugprobe + OpenOCD; CN2 pads SWD/SWC/GND/VCC.
  SWD attached (Cortex-M0+ r0p1, DPIDR 0x0bc11477) without BOOT0 (attach within the ~1.5 s boot window).
- OPTION BYTES differ from the WiFi dump: spare OPTR low byte (RDP) = 0x00 → readout protection LEVEL 1
  (WiFi dump RDP = 0xAA = level 0, open). So MAIN FLASH 0x08000000 is unreadable: every read failed
  ("Failed to read memory at 0x08000000"); system ROM, option bytes, UID, factory config read fine.
- Consequence: the Zigbee PY32's own firmware can NOT be dumped; clearing RDP mass-erases the flash.
  Use the WiFi dump (mainflash.bin) as the reference for Open Dimmer.
- Saved outside the repo: system ROM (3584 B), option bytes, UID and factory config (128 B each).

## Power stage from the elektroda photos (WiFi DMS16-W1 board MK-WC1H16A-A V1.1, read 2026-11-17)
- Top: Q1 = NCE ...T260K N-MOSFET in DPAK (tab = drain towards the terminal block, left lead = gate,
  right lead = source); Q2 pads EMPTY on this 1-channel board (ZDMS16-2 = 2 channels → Q2 fitted).
- Bottom: diodes D7..D14 = a rectifier bridge around each MOSFET (one MOSFET switches both half
  cycles: trailing-edge design); small SOT-23 Q3/Q4/Q5 next to resistors (6802 = 68k, 154 = 150k,
  104 = 100k, 3003?) = gate driver / zero-cross detector parts.
- Near Q1's leads: small resistors, probably 202 (2k) and 51R0/5100 = gate series + pull-down
  (unreadable at 1200x900, verify on the spare).
- Pull-down check (off mains, multimeter): resistance Q1/Q2 gate (left lead) ↔ source (right lead),
  both polarities; 1–100 kΩ = pull-down present. Also PY32 pin 11/12 (PA0/PA1) ↔ pin 1 (VSS).
- 2026-11-17 Zigbee board check (spare, Open Dimmer DIAG build): wall push button wired L–S1 (module label) pulls PB1 low
  as a STEADY level while pressed (no 50 Hz pulses); N–S1 gives nothing. ZC detector PA4: symmetric square
  wave 10.07/10.02 ms, ±20 µs jitter, clean under an LED load.

## Stock ZTU and unknown commands (stock ZTU dump, 2026-11-19)
What the stock Tuya ZTU does with the frames of Open Dimmer's v2 bootloader and app that it does
not know (docs/BOOTLOADER.md section 15, "Open Dimmer under a stock Tuya ZTU", review FB-3).
Source: the public 1 MB dump of the _TZE284_jtbgusdc ZTU named above (sha256 f4afb7a8b559...,
one dump of unknown version; nothing of this ran on hardware). Addresses are file offsets; the
TLSR8258 runs the image from flash address 0. `tests/test_stock_ztu_dump.py` pins these facts
against the file (`STOCK_ZTU_DUMP=<path>`; skipped without it).
- Byte parser 0xC858, state table 0x487E0 (10 states): it takes 55 AA 02, a length ≤ 246
  (0xC9A8) and a good checksum (0xC8B4-0xC8E2), with a 100 ms inter-byte timer 0x2E
  (0xC942-0xC948).
- Then len ≤ 255 (0xC9EE) and (cmd − 1) ≤ 0x39 (0xCA02-0xCA0E, `cmp r3, #57` at 0xCA0A); a frame
  that fails either goes to 0xC8E4. So 0xA0..0xA3 and 0xC0 never reach a handler.
- Command table 0x48808, 58 entries for 0x01..0x3A: handlers only for 0x01, 0x03, 0x05, 0x06,
  0x08, 0x0B, 0x0D, 0x0E, 0x20, 0x24-0x27, 0x29, 0x2C, 0x36-0x38 and 0x3A; the other 39 entries
  point at 0xC8E4 too.
- Drop path 0xC8E4: stops timer 0x2E and returns to the parser. No call to the TX queue 0xDCD8,
  no pending-ack entry (0xB898), no NV write. The bootloader's 16-byte 0xA1 header requests
  (probe, rescue windows, no-app mode, link probe) are therefore dropped silently, at any rate
  and for days: they cannot stall it, reset it, make it leave the network or factory-reset it.
  No 0xA2 is ever sent (nothing is written without an answer).
- Its bootloader region 0x0-0x6A0C holds no Telink UART register literal (0x800090-0x80009F);
  the app region has them from 0x22878 on. No request reaches a serial loader.
- The 0x05 handler 0xD60C (DP report) is not gated by the product-info flag 0x842428: that flag
  is read at 0xC45A, 0xC750, 0xCE24, 0xD956 and 0xD9FE and written at 0xDBB0 in the 0x01
  handler 0xDA5A, and none of these is reached from 0xD60C. The app's health DPs 130..137
  probably reach Zigbee2MQTT as unknown raw Tuya DPs: log noise, not harm.
- The app's other frames: 0x20 network queries, DP reports 0x05 / 0x06, 0x03 after a 3 s
  module-button hold, all with handlers. It never answers the stock ZTU's 0x01 product query, so
  the stock ZTU keeps querying every 1 s and, without its NV UART lock record, cycles its UART
  setting (9600 / 115200 × 2 configurations, a step every 1 s, 0xBB10-0xBCC2). The dump holds
  the record (0xEA20A: `aa 00 01 c2 00 01 aa`) and the full name (0xEC641: "_TZE284_jtbgusdc");
  the default name "TS0601" / "_TZE284_" is set at 0xB6BC and the PID appended only in the 0x01
  handler. Why that combination is unsupported and how to leave it: BOOTLOADER.md section 15.
- For other stock releases the design assumes only that a Tuya MCU-SDK module ignores commands
  it does not know (the Zigbee serial protocol has no NAK). The bootloader needs nothing from a
  stock ZTU, and its traffic is bounded at 10 frames per second for 3 s, then at most 2 per
  second.

## Stock MCU parser limits (WiFi 1.0.3 dump, inferred for Zigbee 1.0.7, 2026-11-19)
The limits every frame of an own ZTU to a factory MCU must respect (review FA-5): the written
basis of ZTU contract item 6 (`src/ota_proto.h`, fork `src/zigbee/tuya_mcu_ota.h`), which every
later ZTU release keeps. From the disassembly of the WiFi DMS16-W1 dump (mainflash.bin, above).
The Zigbee PY32 1.0.7 is read-protected ("SWD read of the Zigbee PY32" above) and probably
shares the vendor template, so for 1.0.7 these limits are **inferred**, not read:
- The USART ISR ring holds 23 bytes (0x200001D3..0x200001EA) and drops bytes when it is full
  (0x080038F8-0x0800393E).
- The linear parse buffer holds 31 bytes (`cmp r0, #31` at 0x08004070, 0x0800406E-0x08004072),
  but frame lengths up to 38 bytes are accepted (`cmp r5, #38` at 0x080040C8). A frame of
  32..38 bytes therefore never completes: no byte is read while the count is 31, and the parser
  waits for the rest (0x080040D8 → 0x08004112). RX stays wedged until a power cut; the unit is
  then controllable only from the wall switch.
- Back-to-back bursts over 23 bytes lose bytes while the MCU blocks its main loop (the 600 ms
  LED blink, a flash save).
- `main` runs `HAL_Delay(1500)` before its init (0x08002A68). The Zigbee unit's SWD window of
  ~1.5 s after power-up fits the same start.
- Unknown commands: the dispatcher ignores them (0x080026CA). The Zigbee 1.0.7 answers 0x0B in
  3 ms with 0x47 and ignores 0x0C (the probes above).

What follows for an own ZTU (contract item 6): towards an MCU not identified as Open Dimmer at
most 29 bytes per frame (the largest frame the phase-1 queue builds: 8 + 4 + 16 + 1), one queued
frame in flight, and at most 23 bytes for frames less than 2 ms apart (a reply counts too).
Frames over 29 bytes (the 38 / 86-byte 0xA1 answers) go only as the direct reply to an 0xA1
request, which a stock MCU never sends. Phase 3's own frames are 9 bytes (0xA3) and 20 bytes
(0xA0 v2).
