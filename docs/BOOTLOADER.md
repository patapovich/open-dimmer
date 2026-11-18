# Bootloader v2: design (fix plan phase 3)

Status: design, 2026-11-18, revised the same day after an adversarial review of this design
(three reviewers; every finding and its resolution is in section 15). Nothing here is
implemented yet. Code references are to this repo at 421685a and to the fork
`patapovich/tuya-zigbee-switch`, branch `home-dimmer` at 51f5c95a (paths under `src/zigbee/`
there are marked "fork"). Item numbers refer to the project review of 2026-11-18, which is kept
privately (see STATUS.md).

The bootloader can only be replaced over SWD (mains off, probe on CN2). Every module installed
from now on keeps the v2 bootloader for good, so v2 is designed to never need replacing: it
gets every escape path that does not depend on the app, accepts a larger app than today, and
leaves every policy that may still change (version rules, offers, settings layout) to the app
and the ZTU, which update over the air. What the frozen code needs from the ZTU in return is
written down as a contract (section 10), because the ZTU changes over the air and the
bootloader does not.

Numbers marked *measured* come from throwaway experiments that are not in the repo:
- the project review's host harness (the unmodified `boot/boot.c` against flash and RAM mapped
  at their chip addresses, forked boots, and a C model of the ZTU's block server);
- a prototype of this design (`boot.c` rewritten as specified here, built with
  arm-none-eabi-gcc 13.2.1 `-Os -Werror` like `make`), run in an extended copy of that
  harness. Since the design review the harness feeds the ZTU's bytes through the USART receive
  path one byte every 87 µs (one-byte data register, overrun), charges the CRC 3.75 µs per byte
  and a page write 4.5 ms, records every watchdog key write, and fuzzes the ZTU's frames. All
  scenarios of section 11 pass (about 11 s of CPU time). Every time in this document comes from
  that byte-timed model; the first prototype's harness delivered frames instantly and gave
  2-2.6 times shorter times.

## Decisions in short

| # | Topic | Decision |
|---|---|---|
| 1 | Layout | Bootloader alone in sector 0 (4 KB). It takes apps up to 14 KB (`BOOT_APP_MAX` 0x3800). Every mode with an installed app keeps a new image inside that app's settings layout (12 KB, or 14 KB with the header flag RING_HIGH); only "no valid app" takes up to 14 KB. Settings ring stays at 0x08004000 (4 KB) until an app needs more than 12 KB; a "ring-move" release first moves it to 0x08004800 (2 KB). Today the ZTU's OTA slot, not the bootloader, caps the app near 12 KB (section 12). Sector 0 is not write-protected in phase 3. |
| 2 | Versions | 16-bit build number in the app header's reserved bytes (0xCE..0xCF). Releases are multiples of 100 (the first 300), test builds take the 99 numbers after a release. The version byte stays for display, the converter and old peers. Identity (0xA3) and offer (0xA0) get the build appended; old peers ignore the extra bytes. Build 0 (2.1.x) images stay valid and servable. |
| 3 | Mailbox | The two reserved words become the boot word (tries, rescue info, probe answer time) and its complement. The bootloader writes `restored` only when it opens a counter rescue, so a crash loop never brings back a stale light state. |
| 4 | Boot flow | Gates low first; the reset flags are copied before any RCC write; the bootloader starts the watchdog with its own ~1 s period. Mode order: update (asked, or no valid app) > rescue (button, or 3 unconfirmed boots) > power-on probe > jump. |
| 5 | Probe | After a cold reset only: header-only 0xA1 every 100 ms for up to 3 s (frozen, generous on purpose). The ZTU commits to answering within 1.5 s of power-up (contract, section 10). The light comes at the ZTU's answer time + ~50 ms; the full 3.1 s only when the ZTU cannot answer. Takes only a valid, larger build number that fits the installed settings layout. |
| 6 | Rescue | 30 s listening windows. Takes any non-bench image the ZTU serves whose CRC differs from the installed one and that fits the layout, older ones included. Button: held when the power comes, released within 5 s. |
| 7 | Confirm | The app confirms after 30 s of uptime once it has answered a 0xA3 in this run. Only apps with the header flag CONFIRMS and no bench flag are counted, so 2.1.x apps and bench builds never fall into rescue loops. |
| 8 | Update path | Every mode asks for the carried header first and pins the image it takes; page 0's vectors are checked before anything is written. Give-up is based on progress (item 28). No request while a frame is half received. Requests carry the bootloader version, the mode and the installed image (16 bytes). The SR-flag shim goes; the flash wait is bounded at ~50 ms. |
| 9-11 | App, ZTU, tools | Listed in sections 9-11. Every change except the bootloader ships over the air. |
| 12 | Size | *Measured* prototype: 3216 of 4096 bytes (+1080 over v1's 2136); 880 bytes stay free. |

---

## 1. Flash layout and constants

PY32F002A: 20 KB flash, 128-byte pages (the erase unit used here), 4 KB sectors (the
write-protection unit and pyOCD's erase unit), 3 KB RAM.

| Region | Address | Size | Content | Written by |
|---|---|---|---|---|
| Sector 0 | 0x08000000-0x08000FFF | 4096 | Bootloader v2 (~3.2 KB code, the rest 0xFF). `boot_id_t` at 0x08000010. | SWD only |
| App area | 0x08001000-0x08003FFF now (12 KB), up to 0x080047FF after the ring move (14 KB) | app size | App image: vectors, header at +0xC0, code. | Bootloader |
| Settings | 0x08004000-0x08004FFF now (32 pages); 0x08004800-0x08004FFF after the ring move (16 pages) | 4 KB / 2 KB | Ring of 128-byte records (`src/store.c`) | App |
| RAM | 0x20000000-0x20000BDF | 3040 | .data/.bss/stack of the bootloader or the app (`RAM LENGTH = 3K - 32` in both linker scripts) | - |
| Mailbox | 0x20000BE0-0x20000BFF | 32 | `ota_mailbox_t` (section 3) | Both |

Constants in `src/ota_proto.h` v2. `APP_MAX_SIZE` goes away: its two users each get their own
limit.

```c
#define BOOT_BASE          0x08000000u
#define BOOT_SIZE          0x1000u
#define BOOT_ID_ADDR       0x08000010u   /* boot_id_t, right after the 4-word vector table */
#define APP_BASE           0x08001000u
#define APP_HEADER_OFFSET  0xC0u
#define BOOT_APP_MAX       0x3800u       /* 14 KB: the v2 bootloader's own ceiling */
#define APP_LIMIT_LEGACY   0x3000u       /* 12 KB: an app whose ring is at 0x08004000, and the v1 bootloader's ceiling */

typedef struct {
    uint32_t magic;      /* BOOT_ID_MAGIC 0x4C42444F "ODBL" */
    uint8_t  version;    /* 2 */
    uint8_t  features;   /* bit 0 try counter, 1 power-on probe, 2 button */
    uint16_t reserved;   /* 0xFFFF */
    uint32_t app_max;    /* BOOT_APP_MAX */
    uint32_t mailbox;    /* 0x20000BE0 */
} boot_id_t;             /* 16 bytes at 0x08000010 */

/* 0xA1 request v2, byte 7 (section 8) */
#define OTA_MODE_ASKED     1             /* the app accepted an offer */
#define OTA_MODE_NO_APP    2             /* no valid app (or the give-up dropped it) */
#define OTA_MODE_PROBE     3             /* power-on probe, and the update it started */
#define OTA_MODE_RESCUE    4             /* 3 unconfirmed boots */
#define OTA_MODE_BUTTON    5             /* module button at power-on */
```

`boot_id_t` sits in its own section, placed in `boot.ld` right after `.isr_vector`, so
`boot.bin` stays contiguous and `mkimage.py --full` keeps padding it to 4 KB with 0xFF. On the
v1 bootloader, 0x08000010 holds code (`80 22 03 4b`), so the magic does not match. The app takes
that as "v1: 12 KB, no counter, no probe".

**App limit (app side).** `app_limit() = min(_settings_start - APP_BASE, boot_id ok ? app_max :
APP_LIMIT_LEGACY)`. Today that is 12 KB with either bootloader. After the ring move it is 14 KB
on v2 and stays 12 KB on v1.

### How a >12 KB image never overwrites a live ring at 0x08004000

| Path into a write | Gate |
|---|---|
| Offer (app consent) | 2.1.x apps check `size <= APP_MAX_SIZE` (12 KB, `src/tuya.c:193`). v2 apps check `size <= app_limit()`. |
| Every bootloader mode with a valid installed app (asked, probe, counter rescue, button rescue) | The installed header's RING_HIGH sets the limit: 14 KB with it, 12 KB without. Checked on the carried header before anything is written (section 8). *Measured*: a counter rescue on a 12 KB-ring app with a 13 KB image carried wrote nothing; over a RING_HIGH app it took the image. |
| No valid app | Up to `BOOT_APP_MAX`: the layout is unknown, and recovery comes before settings. The worst case is that records in 0x08004000-0x080047FF are overwritten and the new app loads an older record from the upper half, or its defaults; code is never corrupted. It needs a power cut during an update on a pre-M unit while its ZTU carries a >12 KB image, which steps 2-3 below exclude. |
| v1 bootloader | `app_valid()` and `on_block()` cap at 12 KB (`boot/boot.c:184-186, 242`), so a unit with v1 never runs or writes past 12 KB. |
| ZTU | The phase-3 ZTU keeps `IMAGE_MAX` at 12 KB; it is raised to 14 KB only in the ZTU release that carries the first >12 KB app (step 3). |
| Tools | `mkimage.py` refuses >12 KB without RING_HIGH. `release.sh` refuses the first >12 KB release unless a RING_HIGH release of at most 12 KB was released before it (section 11). |

### Ring-move sequence (only when the app nears 12 KB; CI already warns below 512 bytes free)

0. **Preconditions.**
   - The ZTU's OTA file has room for the larger app (section 12).
   - Page endurance: 16 pages instead of 32 double the erases per page. Check the datasheet
     endurance against the save rate measured on a module (every level change is saved, so an
     automation that dims every 90 s counts). If it is tight, the app first stops saving pure
     level changes from Zigbee when the power-on behaviour is not "last".
1. **Release M (the ring mover).** Same features plus:
   - Linker: `SETTINGS ORIGIN = 0x08004800, LENGTH = 2K`; app `FLASH LENGTH = 14K`.
   - Header: RING_HIGH set (one Makefile switch sets both the linker and the flag; CI checks
     that the flag matches `_settings_start`).
   - Image size: still at most 12 KB, so every installed app accepts it and a v1 bootloader can
     run it.
   - Scan rule, from M on in every app: `store_load()` scans from `max(0x08004000, APP_BASE +
     own image size rounded up to a page)` to 0x08004FFF with today's newest-sequence rule, and
     writes only into its own ring. For M that is all 32 pages. If the newest valid record lies
     outside its own ring, it is saved into the ring at once with seq + 1, before anything else
     is saved; the sequence counter runs across both halves, so the copy is newer than every old
     record. M never erases or writes the lower half. A power cut during the copy leaves the old
     record in place, and the next start copies it again.
   - M reports "ring moved" (a health-report bit: the newest record sits in its own ring) once
     the copy is saved.
2. **Spread M.** Deploy M to every module. Check that each one reports "ring moved", not just
   M's version: a save that failed on a worn page, or an M that crashed before saving, would
   otherwise lose the only copy at step 3.
3. **Larger apps.** Releases over 12 KB are allowed from then on, carried by a ZTU release with
   `IMAGE_MAX` 14 KB. They keep the scan rule, which now starts above their own code. A module
   still on a pre-M app is safe: it declines the big offer, and its probe and rescue refuse it
   (12 KB gates). It stays on its app until the ZTU carries M again.
4. **Downgrade after step 3.** If a rescue installs a pre-M app, that app finds code (no valid
   record) in the lower half and records of its own format in the upper half. It works,
   possibly with older settings. Records it writes into the lower half are found by the next
   post-M app (scan rule).

SWD note: pyOCD erases whole 4 KB sectors (Makefile:98-102). An SWD install of an image over
12 KB therefore erases sector 4, the ring. `install.sh` must refuse such a full image unless
`ERASE_SETTINGS=1` is given, and docs/INSTALL.md must say so.

### Write protection (WRP) of sector 0

How it works: the option bytes at 0x1FFF0E80 have the layout of `tools/swd/ob_bor25.bin` (`aa f9
55 06 | ff 00 00 ff | ff ff ff ff | ff ff 00 00`: OPTR 0xF9AA, SDKR 0x00FF, a reserved word, WRPR
0xFFFF; OPTR, SDKR and WRPR each followed by its complement). A 0 bit in WRPR protects one 4 KB
sector. Sector 0 alone is WRPR 0xFFFE, complement 0x0001: `... fe ff 01 00` in the last four
bytes.

Decision: **not set in phase 3.** The sector stays protectable (nothing else lives in it).
Reasons:
1. The only flash writer in the app is `store.c`. Its addresses come from the ring index modulo
   `STORE_PAGES` (`src/store.c:97-98`). A v2 app also gets a range guard in `flash_write_page()`
   (section 9), so a stray write into sector 0 needs two independent bugs.
2. WRP would close the only non-SWD fix for a defect found in v2 after the modules are
   installed: a one-off "boot-updater" app with a ~150 ms window in which a power cut bricks the
   unit (32 page writes).
3. Setting WRP means a second option-byte write and reload in `install.sh`. That path has never
   run on hardware (item 8), and every later SWD re-flash would have to clear WRP first.

To enable it later: `install.sh WRP=1`. After the image is written and read back, it writes the
option bytes with WRPR 0xFFFE, reloads them (OBL_LAUNCH) and checks WRPR. Re-flashing then
writes WRPR 0xFFFF first. Try this on the spare before any other module.

---

## 2. Version scheme

### App header (16 bytes at image offset 0xC0, flash 0x080010C0, little-endian)

| Offset | Size | Field | 2.1.x | v2 apps |
|---|---|---|---|---|
| 0xC0 | 4 | magic 0x5A445041 "APDZ" | same | same |
| 0xC4 | 4 | size (filled in by `mkimage.py`) | same | same |
| 0xC8 | 4 | CRC-32 of the image, this field read as 0 | same | same |
| 0xCC | 1 | version byte, Tuya packed (2 bits major, 2 minor, 4 patch) | same | same |
| 0xCD | 1 | flags | SIM 0x01, MEASURE 0x02, DIAG 0x04 | + CONFIRMS 0x08, RING_HIGH 0x10; 0x20..0x80 reserved: 0, ignored by the bootloader |
| 0xCE | 2 | build (u16) | reserved {0, 0} | `FW_BUILD` |

`sizeof(app_header_t)` stays 16. The v1 bootloader reads only magic, size and CRC. Old ZTUs read
magic, size, CRC, version and flags (fork `tuya_mcu_ota.c:78-102`), so neither notices the new
fields.

### Reserved codes

| Field | Value | Meaning |
|---|---|---|
| version byte | 0x00-0x7F | Not ours: the ZTU asks for the identity only from major 2 (fork `tuya_mcu_ota.c:18, 197`), and the converter takes major < 2 for a stock MCU. Major stays at least 2 forever. |
| | 0x8E / 0x8F | DIAG / MEASURE default bytes (Makefile:38) |
| | 0xBF | SIM builds only (`src/tuya.h:31`) |
| | 0xFF | Never (an app with it could not take a byte-rule offer) |
| build | 0 | No build number (2.1.x and older). Valid and servable; never newer than anything in a build compare. |
| | 1-255 | Never (refused by `mkimage.py`, `embed_ztu.py` and the phase-3 ZTU) |
| | 256-0xFFFD | Normal builds. Releases: multiples of 100, each exactly 100 above the previous release (the first 300). Test builds: the previous release + 1..99. |
| | 0xFFFE | SIM builds: no image is newer, so nothing is ever offered to them |
| | 0xFFFF | Never |
| flags | 0x07 | The bench class (SIM / MEASURE / DIAG): never carried (`embed_ztu.py`, ZTU) and never taken by the bootloader in any mode. Installed, it is never replaced automatically (no probe, never counted, declines every offer); it leaves over SWD or a button rescue |
| CRC | 0 | Never (`mkimage.py` refuses it): the bootloader uses 0 for "no image" in its pin, skip and refuse words |

**Rules for humans** (enforced in section 11):
- `FW_BUILD` lives in `src/tuya.h` next to `FW_VERSION` and is the release build. A release
  sets it to the previous release + 100.
- Spare test builds: `make FW_TEST=n` (1..99) builds `FW_BUILD + n`. They stay below the next
  release, so a unit that ever ran a test build always takes the next release, and a typo cannot
  jump the build space.
- `FW_VERSION` is bumped only for releases (a tag is vX.Y.Z of the byte, so tags stay unique).
  It never decreases, and its increase per release is what keeps byte-only peers (2.1.x apps,
  phase-1 ZTUs) updating.
- Test builds keep the byte and bump only the build. That stops the byte from running out (item
  31: 34 codes are left before 0xBF, and today every spare test uses one).

### Identity answer 0xA3, v2 (app to ZTU, 10 bytes)

| Byte | 0-3 | 4 | 5 | 6 | 7-8 | 9 |
|---|---|---|---|---|---|---|
| Field | "ZDMS" | kind 1 (app) | version byte | protocol 1 | build, big-endian | bootloader version: `boot_id.version`, or 1 when there is no boot_id |

`OTA_PROTOCOL` stays 1: the ZTU declines to offer anything to another protocol value (fork
`tuya_mcu_ota.c:250-253`). An old ZTU checks only `len >= 7` and fixed offsets (fork
`tuya_mcu_ota.c:239`), so it ignores bytes 7-9.

### Offer 0xA0, v2 (ZTU to app, 11 bytes)

| Byte | 0 | 1-4 | 5-8 | 9-10 |
|---|---|---|---|---|
| Field | version byte | size (BE) | CRC (BE) | build of the carried image (BE) |

A 2.1.x app checks `len >= 9` and reads bytes 0-8 (`src/tuya.c:189-193`), so it ignores the
build. The app's answer stays 1 byte (0 = accepted).

### "Newer" on each side

| Who | Rule |
|---|---|
| v2 app (0xA0) | `len >= 11` (build-aware ZTU): `256 <= build <= 0xFFFD && build > FW_BUILD`. Shorter (phase-1 ZTU): `byte > FW_VERSION && byte != 0xBF && byte != 0xFF`, as today. Both also need `0xD0 <= size <= app_limit()`. SIM, MEASURE and DIAG builds decline every offer (SIM is new: one `#if`, so a SIM bench is safe under any ZTU). |
| 2.1.x app | `byte > FW_VERSION`, size within 0xC0..12 KB (unchanged, `src/tuya.c:189-193`) |
| phase-3 ZTU (build-aware, section 10) | Identity of 9 or more bytes: offer if `carried build > app build` (a carried build 0 is never offered to it). 7 bytes (2.1.x app): offer if `carried byte > app byte` (today's `tuya_mcu_ota.c:254`). |
| phase-1 ZTU | `carried byte > app byte` (unchanged) |
| v2 bootloader, probe | `carried build > installed build` (a header without one counts as build 0). Neither side looks at the byte. |
| v2 bootloader, rescue | Order does not matter: any CRC other than the installed one (section 6) |

### Compatibility matrix

| MCU app | Bootloader | ZTU | Offers | Probe | Counter rescue | Notes |
|---|---|---|---|---|---|---|
| 2.1.x | v1 | phase 1 | byte | - | - | The spare today |
| 2.1.x | v1 | phase 3 | byte (7-byte identity) | - | - | The spare in rollout step 3, until it takes the release's app |
| v2 app | v1 | phase 1 / phase 3 | byte / build | - | - | The spare between the app OTA and its SWD re-flash. `app_limit()` is 12 KB (no boot_id). The confirmation writes are ignored. |
| 2.1.x | v2 | any | byte | yes (any build counts as newer than build 0) | no (no CONFIRMS) | Only after a rescue downgrade, or bench test B6. Button rescue still works. |
| v2 app | v2 | phase 1 | byte | yes | yes | A module whose ZTU lags behind |
| v2 app | v2 | phase 3 | build | yes | yes | Target |
| stock 1.x | stock | any own ZTU | never asked | - | - | Unchanged |

---

## 3. RAM mailbox v2 (32 bytes at 0x20000BE0)

| Off | Word | v1 bootloader | v2 bootloader | 2.1.x app | v2 app |
|---|---|---|---|---|---|
| 0x00 | `magic` = 0x55504454 "UPDT" | reads, clears | reads, clears | writes after accepting an offer (`tuya.c:202`) | same |
| 0x04 | `magic_inv` = ~magic | reads, clears | reads, clears | writes | same |
| 0x08 | `restore_check` | - | never touched | read/write (`dimmer.c:111-117`) | same |
| 0x0C | `restore` | - | never touched | read/write | same |
| 0x10 | `expect_crc` | reads when asked | reads when asked | writes with the offer | same |
| 0x14 | `restored` | - | writes OTA_RESTORED_MAGIC when it opens a counter rescue window; else never touched | read/write (`dimmer.c:205-216, 240, 245`) | same |
| 0x18 | `boot` (was `reserved[0]`) | - | read and written at every boot | - | reads at start; clears bits 7..0 to confirm |
| 0x1C | `boot_check` (was `reserved[1]`) | - | written = ~boot | - | rewritten with a confirmation |

Boot word:

| Bits | Field |
|---|---|
| 31..24 | Tag 0xB2. The word is valid only when the tag matches and `boot_check == ~boot`. Random RAM matches with p ≈ 2^-40; that matters for the first boot after an SWD install, a software reset with random RAM. |
| 23..12 | Probe answer time in ms since the first probe request (1..4095), 0 = no answer or no probe. The probe starts ~50 ms after power-up (clock, watchdog, `app_valid()`). `PROBE_WINDOW_MS` < 4096 is a static assert. |
| 11..8 | Info, all since the last power-up: bit 8 RESCUE (the counter opened a rescue window), bit 9 BUTTON (the button forced one), bit 10 WRITTEN (an image was written), bit 11 STUCK (the button was still held 5 s after power-up). |
| 7..0 | `tries`: unconfirmed boots of a counted app (CONFIRMS and no bench flag), 0..3 |

Life cycle:
- **Cold boot** (section 4): the word is rebuilt as `0xB2000000`. Probe time, info and tries
  all count since the last power-up.
- **Warm boot**: the word is kept if it is valid, otherwise rebuilt.
- **Asked update, or no valid app**: tries = 0, because the app just proved its update path or
  there is no app.
- **Image written** (result OK, just before the reset): tries = 0, WRITTEN set. A new image gets
  3 fresh runs.
- **Before every jump**: if the app is counted and tries < 3, tries + 1. The word is stored
  together with its complement.
- **App confirmation** (section 7): tries = 0, the other bits are kept.
- **SWD**: a debugger reset is a software reset, so an old word would survive a re-flash. `make
  flash`, `install.sh` and `ota_swd_emu.py` therefore write 0 to `boot` and `boot_check` before
  every reset they issue; a freshly flashed build always starts with 3 tries.

Survival: the system reset keeps RAM, and both linker scripts end RAM at 0x20000BE0, so neither
startup touches the mailbox. After a power-up the RAM is random. The bootloader rebuilds the
boot word and clears the magic pair. The app ignores the restore words after a power-up
(`dimmer.c:206`).

Restore words: v2 never touches words 0x08 and 0x0C. The app's "restore once in a row" policy
keeps working, with one addition:
- **Counter rescue.** When the bootloader opens a counter rescue window it sets `restored` to
  OTA_RESTORED_MAGIC. The app then starts with the power-on behaviour after that window, and
  after the image the window installs. Without it a crash loop could bring back a light state
  that is hours old: the app sets `restored` only at `dimmer.c:215`, after `store_load()` and the
  record parsing, so an app that faults before that line (item 0's example: a fault in
  `store_load()` on one unit's older record) leaves it at 0, which the offered update wrote
  (`dimmer.c:240`). A fixed image installed by a window at 03:00 would then restore the 14:00
  state and switch the lamp on by itself. Three crashes in a row have also used up "once in a
  row". *Measured*: after a counter rescue `restored` = RST1; a plain warm boot leaves it alone.
- **Probe or button update**: the reset flags include PWRRSTF, so the new app uses the
  power-on behaviour.
- **Offered update**: the light is restored as today.
- Cost: under a ZTU that never confirms (section 7), every warm reset after the third also takes
  the power-on behaviour instead of the restore.

---

## 4. Boot flow

### Sequence from reset

```c
Reset_Handler:
    pins_safe();          /* PA0/PA1 gates low (BRR) + outputs, PA5 LED off, PA6 input with pull-up:
                             about 10 instructions after reset, as today (boot.c:63-69, 358-371) */
    copy .data (the RAM flash routine), zero .bss; main();

main:
    csr = RCC->CSR;       /* first: watchdog_start's LSION write is a read-modify-write of the same
                             register, which also holds RMVF; the bootloader never writes RMVF */
    clock_and_uart();     /* HSI 24 MHz with the factory trim, SysTick 1 ms polled, USART1 115200 8N1 */
    watchdog_start();     /* LSION, LSIRDY wait bounded; KR 0xCCCC; KR 0x5555, PR 3, RLR 1000 (~1 s),
                             IWDG_SR wait bounded: the period never depends on reset values or an app */
    cold  = !(csr & (SFTRSTF | IWDGRSTF));
    if (cold && held(BUTTON_MS)) {                 /* pressed before the power came */
        LED on;  stuck = held(BUTTON_RELEASE_MS);  button = !stuck;  LED off;
    }
    asked = mb->magic == UPDT && mb->magic_inv == ~UPDT;
    g_expect = asked ? mb->expect_crc : 0;   mb->magic = mb->magic_inv = 0;
    word  = (!cold && valid(mb->boot)) ? mb->boot : 0xB2000000;   if (stuck) info |= STUCK;
    h     = installed header;  ok = app_valid();  counts = (h->flags & (CONFIRMS | BENCH)) == CONFIRMS;
    limit = h->flags & RING_HIGH ? 14K : 12K;
    if (!ok || asked)                        { tries = 0; if (!ok) limit = 14K; mode = ok ? ASKED : NO_APP; run(GIVE_UP_MS); }
    else if (button)                         { info |= BUTTON; skip = h->crc; mode = BUTTON; run(RESCUE_WINDOW_MS); }
    else if (counts && tries >= 3)           { info |= RESCUE; mb->restored = RST1; skip = h->crc; mode = RESCUE; run(RESCUE_WINDOW_MS); }
    else if (cold && !(h->flags & BENCH))    { mode = PROBE; run(PROBE_WINDOW_MS); }
    if (counts && tries < 3) tries++;
    store word, ~word;  jump_app();
```

`run()` returns only to start an intact app. A finished image ends in `NVIC_SystemReset()`.

### Reset cause

The app clears the flags at every start (`src/main.c:192`), and a power reset clears them and
sets PWRRSTF (plus PINRSTF). So:

| RCC_CSR at the bootloader | Meaning | cold |
|---|---|---|
| PWR (+PIN) | Power-up or brown-out | yes |
| PIN or OBL only | NRST (debugger) or option reload | yes (harmless: one probe) |
| PWR + SFT | The bootloader's own reset after a probe or button update, before any app ran | no, so no second probe (*measured*: 0 probe requests on the boot after a probe update) |
| PWR + IWDG | Power-up, then the app died before it cleared the flags | no, the counter goes on (*measured*, crash-loop scenario) |
| SFT and/or IWDG | Update restart, fault, watchdog | no |

A brown-out that keeps RAM shows PWR only and counts as cold (tries 0, one probe), the same
view the app takes (`dimmer.c:206`: power-on behaviour). Two hardware facts are assumed: PWRRSTF
is set at power-up, and a power reset clears SFTRSTF and IWDGRSTF. The app's restore policy
already relies on the first one. Both are checked on the spare (bench test B8), together with
"the LSION write leaves the flags alone" (the copy is taken before it anyway).

### Watchdog

The bootloader starts the IWDG and sets its period at every boot: PR 3, RLR 1000, ~1 s at the
nominal LSI, the value the app uses (`main.c:160-175`). So it no longer matters whether a
software reset stops a running IWDG (the old comments say it keeps running, `boot.c:293`; never
verified, B8 records it), and an app that chose a short period cannot reset-loop the bootloader
before it counts. The waits for LSIRDY and the IWDG register update are bounded (~100 000
passes, ~25-40 ms; normally they end within a few LSI cycles); with an LSI that never starts
the watchdog simply does not run.

`tick()` refreshes the watchdog on every pass of every loop (probe, update, rescue, button). The
longest gaps between refreshes (*measured* in the byte-timed model, all scenarios and the fuzz):
- `app_valid()` over the image: ~43 ms for 11.6 KB, ~51 ms for 13 KB, ~54 ms for 14 KB (CRC
  ~3.75 µs per byte at 24 MHz);
- a page write: ~3.5 ms erase + ~1 ms program (`src/flash_drv.h:1-4`), twice when retried. The
  shared flash driver's busy wait is bounded at ~50 ms (section 8); with a controller that never
  finishes, one page costs at most 2 × 3 × 50 ms without a refresh.

The largest gap seen over every run was 51 ms, against the ~1 s period.

A 2.1.x app starts its own watchdog only at the end of init (`main.c:203`). With v2 the IWDG is
already running with the same period, as after every OTA today. Its init before that point
takes ~40 ms (the longest step is the 30 ms `LL_mDelay` in `controls.c:47`). Option byte IWDG_SW
stays 1 (software watchdog, OPTR 0xF9AA): no option-byte change.

### Button (PA6, active low, pull-up; `docs/stock-firmware-analysis.md` pin table)

The pull-up is set in `pins_safe()`. The button is read on cold boots only, right after the
watchdog start and before `app_valid()`, ~1 ms after reset. A rescue needs both:
1. PA6 low on every pass for `BUTTON_MS` = 100 ms (the loop ends at the first high read, so a
   released button costs nothing);
2. a release within `BUTTON_RELEASE_MS` = 5 s. Meanwhile the LED is on, which tells the user to
   let go; the rescue window (slow blink) starts at the release.

A button still held after 5 s counts as stuck (a failed switch, moisture, a weak pull-up): no
rescue, info bit STUCK, and the boot goes on as a normal cold boot with its probe. A stuck button
costs 5 s per power-up and never installs anything (*measured*: app after 5.15 s, nothing
written, STUCK set; released after 4.5 s: rescue).

Because the window opens only after the release, no app ever sees the rescue press. That
matters for 2.1.11 and older: they take a press held into the app as a new press, and a hold of
3 s or more sends TUYA_CMD_RESET_NETWORK (`controls.c:165-167`), which the ZTU answers with a
factory reset for a dimmer (fork `tuya_mcu_app.c:308`). 2.1.12 already ignores an input closed
at power-up (`g_boot_held`, `controls.c:58, 203-207`). For a press that comes too late, after
the bootloader looked, the v2 app ignores a module-button hold of 3 s or more that began in the
first 10 s after a cold start (section 9).

User steps for docs/INSTALL.md: press the module button and keep it pressed, then switch the
power on; when the LED lights, let go within 5 s. The LED then blinks slowly while the module
waits for an image and fast while it writes one; without a different image it starts the light
after 30 s. The board is not isolated from the mains (INSTALL.md, "Mains voltage"): press the
button only with an insulated tool, never with a finger on the open board.

### Timeouts

| Constant | Value | Why |
|---|---|---|
| `PROBE_EVERY_MS` | 100 | The light comes at most ~100 ms after the ZTU can answer. Never sent while a frame is half received (section 8). |
| `PROBE_WINDOW_MS` | 3000, frozen | Generous on purpose: the probe ends at the first answer, so the window costs light delay only when the ZTU cannot answer at all. The ZTU commits to 1500 ms, half of it (section 10). < 4096 (boot word field). Section 5. |
| `BUTTON_MS` | 100 | Rejects bounce and the pull-up settling |
| `BUTTON_RELEASE_MS` | 5000 | Long enough to let go after the LED lights; the cost of a stuck button per power-up |
| `REQUEST_EVERY_MS` | 500 | Unchanged (boot.c:34): the request pace when nothing comes |
| `REJECT_PACE_MS` | 50 | A stray block (wrong offset, duplicate, short) is asked for again 50 ms later instead of at once (review: ~210 requests/s in the deadlock) |
| `GIVE_UP_MS` | 10 000 | Unchanged value, now measured from the last accepted block (item 28) |
| `RESCUE_WINDOW_MS` | 30 000 | Section 6 |
| frame silence | 50 | Unchanged (boot.c:136-138) |
| result flush | 20 | Unchanged (boot.c:321) |

### What the light and LED do

The gates are driven low in `pins_safe()` and never touched again by the bootloader, so the
light is dark in every bootloader state. The app decides the light at its start: the power-on
behaviour after a cold boot or a counter rescue, or the restore-once rule after another warm
reset.

| State | LED (PA5) |
|---|---|
| Probe | Off |
| Button held, waiting for the release | On |
| Rescue or update, nothing taken yet | Slow blink, 512 ms on / 512 ms off |
| Writing (a block was taken) | Fast blink, 128 ms on / 128 ms off |

### Jump checks

`app_valid()` checks:
- the magic;
- `0xD0 <= size <= BOOT_APP_MAX`;
- the initial SP in 0x200xxxxx and the reset vector inside the image (as boot.c:184-186, with
  the new ceiling);
- the CRC.

The same vector rule is applied to page 0 of a fetched image before anything is written
(section 8), so the bootloader never erases a working app for an image it would refuse to run.
Then `jump_app()` runs as today (boot.c:198-209): interrupts off, SysTick off, USART1 reset,
VTOR = APP_BASE, MSP = SP, branch (through `uintptr_t`, so the host build passes `-Werror`).
Stricter checks (SP exactly 0x20000BE0, an odd reset vector) stay in the tools and the ZTU's
`image_info` (item 42, section 10).

### State machine

```
        +---------------------------------- reset ----------------------------------+
        | gates low; flags copied; clock; IWDG ~1 s; button (cold); mailbox; app_valid |
        +------------------------------------------------------------------------------+
 no valid app | asked     button released | tries>=3 & counted    cold & not bench        else
        v                       v                                       v                   |
  UPDATE (asked: that CRC)  RESCUE (not the installed CRC)        PROBE (newer build only)  |
        \_______________________|_______________________________________/                   |
                                v                                                           |
     HEADER: 0xA1 for 0xC0/16 every 500 ms (probe: 100 ms). Take an image that suits         |
     the mode: APDZ, no bench flag, size within the layout, not skipped or refused           |
                                | taken: pinned               probe: first answer refused,    |
                                v                               or 3 s silent -> JUMP        |
     FETCH from offset 0. Page 0's vectors checked before the first write                    |
     (bad: BAD_IMAGE result, refused for this run, back to HEADER)                           |
                                | image done                                                 v
                    result OK, tries 0, WRITTEN, reset                    JUMP (tries+1 if counted)

  No progress for the window (10 s after a block; 30 s rescue; 3 s probe):
  app valid -> JUMP; else NO_APP: unpin, unskip, unrefuse, limit 14 KB, keep asking.
```

---

## 5. Power-on probe protocol

**Request.** A header request in the v2 format (section 8), seq 0: offset 0x000000C0, max_len
16, boot version 2, mode 3 (probe), then the installed header's bytes 8..15. With an installed
build 300 (CRC 0x73B54038, byte 0x9A, flags 0x08):
`55 AA 02 00 00 A1 00 10 | 00 00 00 C0 00 10 02 03 38 40 B5 73 9A 08 2C 01 | F6`. It goes out
every 100 ms from the start of the window, never while a frame is half received.

**Answer** (fork `answer_block`, `tuya_mcu_ota.c:270-300`, unchanged): 38 bytes.
`55 AA 02 00 00 A1 00 1D | 00 00 00 C0 | size | crc | ver | 16 header bytes | cs`. Example, a
carried 11628-byte image with build 301: size `00 00 2D 6C`, then `41 50 44 5A 6C 2D 00 00
<crc LE> 9B 08 2D 01`.

The ZTU serves any offset without keeping state. Every own ZTU since phase 1 answers this
request: `answer_block()` checks only `len >= 6` and reads bytes 0-5 (`tuya_mcu_ota.c:273`), so
the v2 bytes are ignored. The `off == 0` side effect (`tuya_mcu_app_mcu_restarted()`,
`tuya_mcu_ota.c:279-283`) does not fire for 0xC0, so the probe leaves the ZTU's first-contact
logic alone. A real update afterwards starts at offset 0 and resets it as today.

Why offset 0xC0 and not offset 0 with max_len 0 (the project review's first sketch)? That answer
holds only the block header (size, CRC, version byte), without the build number, and it fires
the `off == 0` restart on the ZTU.

**Decision.** All of these must hold, else jump:
- `cmd == 0xA1`, `n >= 13 + 16` and `offset == 0xC0` (other frames are ignored and the probe
  keeps listening);
- carried header magic == "APDZ";
- `0xD0 <=` carried size (block header) `<= limit`, where limit = 14 KB if the installed header
  has RING_HIGH, else 12 KB;
- `(carried flags & 0x07) == 0`;
- carried build > installed build (an image with a larger build cannot have the installed
  CRC).

Then the image is pinned to the carried CRC and fetched in the same loop (pace 500 ms, give-up
10 s). The first block request is offset 0, so the ZTU resets its contact state and answers
whole blocks.

**When is the probe skipped?**
- On warm boots. A crash loop is the counter's job, and an asked update brings its own image.
- When the installed app is a bench build (flags & 0x07). DIAG and MEASURE builds decline every
  offer so that they stay (item 17, `src/tuya.c:179-185`). The probe must not undo that after
  each power cut, and SIM benches keep their build too.
- When the app is invalid or asked: those modes take their image anyway.

**When can the ZTU answer?** The ZTU starts its MCU UART in `tuya_mcu_app_init()` (`app.c:52`).
That runs after `handle_version_changes()` (NV migrations, `app.c:49`) and `parse_config()`,
which ends with the whole Zigbee stack init, `hal_zigbee_init()` (`config_parser.c:672`: NV
load, ZCL attribute restore, BDB init). Frames are handled only from the main loop (`app.c:65`,
`tuya_secondary_mcu_poll` at `tuya_secondary_mcu.c:663`). The ~2 s before the ZTU's first own
frame is a separate timer (`FIRST_CONTACT_DELAY_MS 2000`, `tuya_mcu_app.c:156, 371`). The first
0xA1 also makes the ZTU check its carried image once (`image_info`, one CRC over ~11.5 KB).

So the answer time depends on the ZTU firmware, which changes over the air, and on each unit's
NV content (bindings, groups, scenes): it is not a property of the spare alone, and it has not
been measured. The UART pins are parsed before `hal_zigbee_init()` (`config_parser.c:359`), so
the UART could start earlier, but without a poll during init that would only buffer requests,
not answer them sooner. That ZTU change stays an option for later (section 10).

**Window: 3000 ms, frozen.** The first version froze 1.5 × the spare's measured answer time.
That would make the probe an unwritten timing contract with every later ZTU release: a release
whose first answer comes after the window makes every probe on every unit time out and jump,
and escape (b) is gone without any alarm (DP 137 just shows 0). The probe ends at the first
answer, so a long window costs light delay only when the ZTU does not answer at all: during its
failure pause, without a usable image, or broken. So:
- `PROBE_WINDOW_MS` = 3000, below the 4095 the boot word field can hold;
- the ZTU commits to its first header answer within 1500 ms of power-up, including the first
  boot after a ZTU OTA (contract, section 10);
- every ZTU release is checked against that on the spare before it goes further (section 14,
  B2), and DP 137 shows a miss on any unit (a cold boot with probe time 0).

A window that the installed app's header could set was considered and left out: it would take
flag bits, and the escape's timing should not come from the app it escapes from.

**ZTU change for the probe: none needed to work.** A new "image info" command was rejected: it
would only work with ZTUs not yet built, and v2 would still need this request as a fallback.
The phase-3 ZTU adds the contract, its tests and a boot-time diagnostic (section 10).

**Light delay after a power cut** (*measured* in the byte-timed model, nothing newer carried):

| ZTU answers from (ms) | 0 | 100 | 300 | 600 | 1000 | 1400 | 2000 | 2900 | ≥3200 or silent |
|---|---|---|---|---|---|---|---|---|---|
| App starts at (s) | 0.05 | 0.15 | 0.35 | 0.66 | 1.06 | 1.46 | 2.07 | 2.98 | 3.12 |

With a newer build carried, the app started after the probe and the update: 2.82 s with the ZTU
answering from 300 ms (2.14 s with 128-byte blocks), 3.93 s from 1400 ms. The other probe cases
all behaved as intended: same image, older build, a carried 2.1.12 without a build, both without
a build, a 13 KB image over a 12 KB-ring app, a carried SIM image, a carried DIAG image with a
newer build, and a DIAG installed (no request at all): not taken. A 13 KB image over a
RING_HIGH app was taken, and so was build 301 over an installed 2.1.10. With the ZTU answering
20-201 ms after each request, all 20 of 20 power-ups found the newer build at every latency (the
first prototype found it in 3 of 20 at ~97 ms: a request sent while an answer arrived destroyed
it).

---

## 6. Rescue mode (3 unconfirmed boots, or the button)

**Requests.** Header requests (0xC0, 16 bytes, mode 4 or 5) every 500 ms until the ZTU carries
an image that suits; then that image from offset 0, the next request at once after each
accepted block.

**What is taken.** An image is pinned when its header is "APDZ", it has no bench flag, its size
is within the installed layout (12 KB, or 14 KB with RING_HIGH; the button too, after user
decision 2), its CRC differs from the installed one, and page 0 was not refused in this run.
**Older builds are included.** Reasons:
- The counter fires only when the installed app demonstrably cannot run 30 s and answer the
  ZTU. Any other image that passes the ZTU's and the bootloader's checks is at least as good a
  bet.
- The usual repair is to carry the last good image again (py32-bump with force). A newer-only
  rule would turn that into a re-release under a new build first.
- The button is the user's explicit request: it is the only way to go back to an older build
  without a new release, because the offer path and the probe take newer builds only.

*Measured*: in a crash loop with a SIM (flags 0x09, build 0xFFFE) or DIAG (0x0C, build 307)
image carried, nothing was installed; neither by the button, nor with no valid app (that unit
keeps asking until the ZTU carries a normal image). The first prototype installed both at boot
4, which locked the unit into the bench build: DIAG declines every offer, SIM is offered
nothing, and both confirm.

The installed CRC is skipped because re-installing it would rewrite nothing (identical pages are
not erased, `flash_drv.c:83-86`) and would end in the same crash. Because every mode asks for the
header first, a window with the same image carried sends header requests only (*measured*: 0
offset-0 requests in a 30 s window; the first prototype sent 61, each of which made the ZTU
treat the MCU as restarted).

**Window.** 30 s from the start, or from the last accepted block (then 10 s). With nothing new
the bootloader starts the app again. `tries` stays at 3, so every further warm reset opens
another window until a run confirms or the power is cut.

**Why not stay listening for ever (the literal "stays listening")?**
- For an app that crashes or hangs in init, the effect is the same: *measured* 96 % of the time
  listening (12 cycles of a 1 s watchdog hang + 30 s window).
- A false trigger would otherwise darken a working light until the next power cut.
- A window of 30 s also covers a ZTU that reboots into a new image (~2-3 s) during a window.

**Power cuts.**
- Before any page is written: the next power-up is cold, tries is 0 and the app gets 3 fresh
  runs (plus the probe).
- After a page is written: no valid app, so UPDATE takes any non-bench image. *Measured*: 12 of
  12 cut points recovered, with the ZTU then carrying a third image.

*Measured* crash loop (the installed build 302 confirms nothing; the ZTU carries 302, then from
boot 7 an older 299):
1. Boots 1-3 jump (tries 1, 2, 3).
2. Boots 4-6 open a 30.1 s window each (same CRC skipped), mark `restored`, and then jump.
3. Boot 7 installs 299 in 2.8 s (info RESCUE + WRITTEN); the new app runs with tries 1 and
   confirms.

Button runs (released 0.5 s after power-up):
- ZTU carries an older image: installed 3.3 s after power-up; on the next boot, with the button
  still held but warm, the app starts at once.
- Same image carried: the app started after 30.6 s.
- Not held: no delay. Held for good: section 4.

---

## 7. App confirmation

**Rule.** A v2 app confirms once both hold:
- its uptime is ≥ `BOOT_CONFIRM_MS` = 30 000;
- it has answered at least one 0xA3 identity question in this run.

It then clears bits 7..0 of a valid boot word and rewrites `boot_check`:

```c
static void boot_confirm(void) {                /* main loop, once per run */
    volatile ota_mailbox_t *mb = OTA_MAILBOX;
    uint32_t w = mb->boot;
    if ((w >> 24) == BOOT_TAG && mb->boot_check == ~w && (w & 0xFFu)) {
        w &= ~0xFFu;  mb->boot = w;  mb->boot_check = ~w;
    }
}
```

Why both conditions:
- The answered 0xA3 shows that UART RX/TX, framing and the identity handler work.
- The ZTU makes its offer right after that answer, so a confirmed app has already had its chance
  to take a newer image.
- The 30 s covers the start-up work: the first settings save (2 s after a change), the DP dump,
  the zero-cross lock and the first fades.
- A crash after 30 s needs no bootloader rescue: the app lives long enough to accept an offer.

**What the confirmation depends on in the ZTU.** The ZTU asks for the identity after it notices
an MCU start: the app's first network query has seq 0 (`tuya_mcu_app.c:208-221`), or a block
request for offset 0, or an OK result. Then come the version question and 4 identity questions
in 7.5 s, and 2 more at later network queries if all were lost (`tuya_mcu_ota.h:4-14`,
`tuya_mcu_ota.c:162-196`). Two gaps, both closed over the air:
- If the first query is lost, a ZTU that already knew the MCU as its own never asks in that run.
  The v2 app therefore numbers its network queries 0 until the ZTU has answered one (section 9).
- Today's ZTU asks only when it carries an image (`tuya_mcu_ota_may_be_own()` needs
  `secondary_mcu_image_size != 0`, `tuya_mcu_ota.c:197`). The phase-3 ZTU asks any MCU of major
  2 or later (section 10). Both obligations are part of the ZTU contract.

**No fallback "confirm after N minutes without 0xA3".** A stable app is never pushed into rescue
anyway (the counter moves only at resets). Without the fallback, an app with a broken UART that
also resets now and then is still rescued. An app whose UART is broken and that never resets is
not rescued automatically: it needs a power cut (the probe, if a newer build is carried) or the
button. An app-side link watchdog (a warm reset after ~10 min without a valid ZTU frame) would
need no frozen change, but it would also disturb a working light whenever the ZTU, not the app,
is the broken side: after three such resets the counter window opens and the light takes the
power-on behaviour. It is left out and can be added over the air later.

**Cost when nothing confirms.** Under a ZTU that never asks 0xA3, every warm reset after the
third unconfirmed boot opens a 30 s dark window, until a confirmed run or a power cut
(*measured*: 7 windows in 1 cold + 9 warm boots, `...WWWWWWW`), and the light takes the power-on
behaviour after it (section 3). Who is affected: a ZTU without own-MCU support (forbidden by the
STATUS migration order), a ZTU of today that carries no image (fixed by the phase-3 ZTU). Warm
resets are rare in a working app, so the cost is per watchdog or fault reset, not per day.

**CONFIRMS flag.**
- v2 apps set it in every build. Bench builds carry it too, but the bootloader never counts them
  (`counts = CONFIRMS and no bench flag`): a SIM bench without a ZTU, or a DIAG session, never
  sees a window (*measured*: DIAG with CONFIRMS, 8 warm resets, tries 0).
- 2.1.x apps lack it, so `tries` never moves for them: no rescue loops, ever (*measured*: 8
  watchdog resets of a 2.1.12 image, 0 slow boots, tries 0).
- A confirming app with a reset between confirmations never sees a window (*measured*, 10
  resets).
- Accepting an offer also counts as healthy: the bootloader sets tries to 0 when it finds the
  UPDT request.

Under the v1 bootloader the confirmation finds no valid word and does nothing.

---

## 8. Update-path changes

| Change | Detail | Evidence |
|---|---|---|
| Header first, in every mode | Every mode starts by asking for the carried header (0xC0, 16 bytes) and pins an image only when it suits the mode: asked = the mailbox CRC; probe = a newer build; rescue = not the installed CRC; no app = any. All modes: "APDZ", no bench flag, `0xD0 <= size <= limit`. Blocks of any other image are then ignored. | *Measured*: bench images never installed in any mode (section 6) |
| Page 0 checked before the first write | Page 0 (vectors) is the first page written. Before it is, SP and reset vector are checked with `app_valid()`'s rule. A failing image gets one BAD_IMAGE result, is refused for this run (until a give-up), and nothing is written. | *Measured*: a carried CRC-valid build 301 with SP 0x30000BE0 was refused at a power-up and in a crash loop, 0 pages written, the old app ran on. The first prototype erased the working app for it and stayed dark. |
| Progress-based give-up (item 28) | `progress = g_ms` at the start, at every taken header and at every accepted block. When `g_ms - progress > window`: if `app_valid()` start it, else drop pin, skip and refusal, set the limit to 14 KB and the mode to NO_APP, and go on with 10 s windows. | v1 harness: image change mid-transfer stuck after 300 s (63 214 requests); silence after an identical page 0 stuck after 120 s. *Measured* in v2: 14.6 s to the new image; 10.1 s back to the intact app. |
| Paced rejects | `on_block()` returns TOOK / STRAY / OTHER / DONE. TOOK: next request at once. STRAY (offset, length or duplicate): after 50 ms. OTHER (not the pinned image): stays at the 500 ms pace. | Noisy link (5 % drop, 5 % late duplicate, 2 % bit flip), 50 seeds: 0 failures, mean 14.6 s, max 19.1 s (128-byte blocks: 8.1 / 13.1 s) |
| Half-duplex requests | A request goes out only when no frame is half received (`g_rx_len == 0`). The USART has a one-byte data register and `send()` busy-waits ~2.2 ms for a 25-byte request without reading RX, so a request sent into an arriving answer destroyed it. | *Measured*: 20 of 20 power-ups at every ZTU latency 20-201 ms; before, 3 of 20 at ~97 ms |
| Image change mid-transfer | The pinned image's blocks stop coming (OTHER); after the give-up the intact old app starts, or, if pages were written, the new image is taken from offset 0. | as above |
| Request v2 (16 bytes) | offset (4, BE), max_len (2, BE), bootloader version (1), mode (1, `OTA_MODE_*`), the installed header's bytes 8..15 as in flash (CRC LE, version byte, flags, build LE; meaningless in mode 2). A phase-1 ZTU reads bytes 0-5 only (`tuya_mcu_ota.c:273`); a 6-byte request means a v1 bootloader. The frozen requests thus tell a future ZTU which bootloader asks, why, and what is installed: it can report a crash loop, or serve a last-good image to a rescue, without any bootloader change. | +24 bytes |
| Result v2 (4 bytes) | status, version byte, build of the written image (BE). Phase-1 ZTUs read bytes 0-1. | (in the line above) |
| Block size | Requests stay `min(left, room in page)` ≤ 128. Any 1..128-byte block that fits the page is taken. The ZTU stays at `BLOCK_MAX 64` (fork `tuya_mcu_ota.c:21`, `tuya_secondary_mcu.c:117`) until its main-loop blocking with 150-byte frames (~13 ms busy-wait per frame at 115200) has been measured; it can change later without SWD. | *Measured* with 128: 92 instead of 183 requests, asked update 1.84 instead of 2.52 s |
| Flash driver | The `BOOTLOADER` SR-flag shim goes (`flash_drv.c:54-58, 90-94`): flags are cleared before each operation, as in the app since phase 2. Each page is still tried twice before `OTA_RESULT_FLASH`. The busy wait is bounded at 150 000 passes, ~50 ms (it is 2 000 000, ~0.67 s at ~8 cycles a pass, `flash_drv.c:46`: a controller that never finished would reset-loop the bootloader before it could report). A write-range guard comes from linker symbols (section 9). Shared with the app. | `test_flash_drv`, built today with and without `BOOTLOADER`, then needs one variant plus the guard and a BSY-never-clears case |
| Receive | After a bad checksum the buffered bytes are rescanned before more are read (item 39(4)) | in the size table |
| NOTIFY in the bootloader | Dropped (boot.c:314-319). The ZTU offers only after an identity answer (`tuya_mcu_ota.c:235-268`), and the bootloader never gives one. | |
| SWD test path | The injection buffer `g_inject` stays; `ota_swd_emu.py` answers the header request first (section 11). | |
| Watchdog | `tick()` refreshes in every loop. Largest gap 51 ms (section 4). | |

Power cut at every step:

| Cut during | After power returns | Outcome |
|---|---|---|
| Probe, header phase, or a window with nothing written | App intact; cold; tries 0 | Normal boot (and a probe) |
| Any page erase or program (asked, probe, rescue) | App CRC invalid; mailbox random, so nothing is pinned | UPDATE takes any non-bench image the ZTU serves. *Measured*: 162 of 162 cut points (81 differing pages × after erase / after program) recovered; 12 of 12 during a rescue. |
| After the last page, before the result | New app valid | Cold boot, probe finds the same build, jump |
| Between the app's UPDT write and the bootloader | Mailbox lost | Cold: the probe takes the image if it is newer; otherwise the old app runs and the ZTU offers again at first contact |
| Ring move (M's copy) | Old record intact | M copies again |

Real update time: in the byte-timed model an asked update took 2.52 s with 64-byte blocks
(including `app_valid()` before and after), 1.84 s with 128. The ZTU's own latency comes on
top; B9 measures it.

---

## 9. App-side changes (ship over the air)

*Measured* for items 1-7 of the first version: +256 bytes on 2.1.12's 11 472. Items marked
"new" came with the review; estimated ~60 bytes together.

1. **Header.** `{ APP_MAGIC, 0, 0, FW_VERSION, APP_FLAG_CONFIRMS | (RING_HIGH ? APP_FLAG_RING_HIGH
   : 0) | bench flags, FW_BUILD }` (`src/main.c:27-30`). `FW_BUILD` sits in `src/tuya.h`; `make
   FW_TEST=n` adds 1..99 for spare test builds.
2. **Watchdog first.** `watchdog_start()` directly after the RCC flags are read and cleared
   (`main.c:188-192`), before `clock_init()`, with the bootloader's period. The HardFault/NMI
   reset handler is already there (`main.c:180-185`).
3. **Confirmation** (section 7). The 0xA3 handler sets `g_ident_answered`, and the main loop
   calls `boot_confirm()` once both conditions hold.
4. **Identity v2** (10 bytes, section 2).
5. **Offer rule** (section 2), with `app_limit()` from `_settings_start` and `boot_id`, and the
   lower bound aligned with the bootloader (`size >= APP_HEADER_OFFSET + sizeof(app_header_t)`,
   item 42). New: the 11-byte rule also needs `256 <= build <= 0xFFFD`, and SIM builds decline
   every offer as DIAG and MEASURE builds do.
6. **Bootloader identity.**
   - DP 136: CRC-32 of the whole 4 KB boot sector, computed once at start (~15 ms). The released
     v1 bootloader (2136 bytes, 0xFF-padded) gives **0xEAC45031**; each v2 build's value goes
     into STATUS.md with the release.
   - DP 137: the boot word at start, low 24 bits (probe time, rescue info, tries).
   - Both are sent with the health report (`src/health.c:34-41`, first at 60 s). Like DPs
     130..135 they are only logged by the ZTU's MCU link diagnostics: no converter change. The
     bootloader version also goes out in identity byte 9.
7. **Flash write-range guard.** `flash_write_page()` refuses addresses outside
   `[_flash_write_start, _flash_write_end)`: the settings region in the app linker script, the
   app area 0x08001000..0x08004800 in `boot.ld`. That replaces the `#ifdef BOOTLOADER` in
   `src/flash_drv.c`. The bounded busy wait (section 8) is in the same shared file.
8. **New: late button press.** A module-button hold of 3 s or more that began in the first 10 s
   after a cold start is ignored (no rejoin, no factory reset): a rescue press that came after
   the bootloader looked must not remove the module from the network (section 4).
9. **New: first query.** Network queries keep seq 0 until the ZTU has answered one, so a lost
   first query still makes the ZTU see the restart and ask for the identity (section 7).
10. **Later, release M only:** the ring move of section 1.

All of this works under the v1 bootloader (compatibility matrix). The changes ship in the same
release as v2, because the full image that `install.sh` writes must contain a counted app. The
release's app goes to the spare over the air first, under v1 (section 14).

---

## 10. ZTU-side changes (fork, phase-3 ZTU release; over Zigbee)

The phase-3 ZTU release must stay correct with the v1 bootloader on the spare and with stock
MCUs.

### Frozen ZTU contract

What the frozen bootloader needs from every future ZTU. Written into `src/ota_proto.h` (MCU
side) and fork `tuya_mcu_ota.h` (ZTU side), pinned by the tests below, and checked for every ZTU
release on the spare (section 14, B2):
1. **0xA1 v1 semantics, for ever.** A request is offset (4) + max_len (2) + any number of
   further bytes, which the serving rule ignores. The answer is offset, size, CRC, version byte
   and up to min(max_len, 128) bytes of the image, without state, for any offset; the header
   sits at image offset 0xC0. Serving is gated only by the image check (`image_info`) and the
   failure pause, never by build or version rules.
2. **Robust.** No crash and no stall on any request the bootloader can send: offset
   0..0xFFFFFFFF, max_len 0..0xFFFF, 6 to 16 bytes, at up to one request per 50 ms for days (a
   unit with no app asks every 500 ms until it gets an image).
3. **Timely.** The first header answer within 1500 ms of power-up (half of `PROBE_WINDOW_MS`),
   including the first boot after a ZTU OTA.
4. **Identity after every MCU start.** After a seq-0 network query, a block request for offset
   0, or an OK result, the ZTU asks for the identity (0xA3) of any MCU of major 2 or later.
5. **Results.** 0xA2 status 0 = written and valid; anything else a failure that counts toward
   the pause; the pause doubles up to a day and ends with a ZTU restart (item 41, phase 1).

### Changes

1. **`on_ident`** (`tuya_mcu_ota.c:235-268`):
   - Identity `len >= 9`: the app's build = bytes 7-8 BE; offer when the carried header's build
     (image bytes 0xCE-0xCF LE) is greater. A carried build 0 is never offered to it.
   - Identity of 7 bytes: today's byte rule.
   - Store byte 9 (bootloader version; 0 when the identity is shorter) in `tuya_mcu_diag` and
     log it with the version.
   - `IDENT_LEN 7` stays the minimum; protocol 1 is unchanged.
2. **Identity asked without an image too.** `tuya_mcu_ota_may_be_own()` drops the condition
   `secondary_mcu_image_size != 0` (`tuya_mcu_ota.c:197`): every build with own-MCU support
   asks a major-2 MCU who it is; only the offer needs an image (contract item 4).
3. **0xA0** gains the build: 11 bytes (section 2).
4. **`image_info`** (`tuya_mcu_ota.c:78-102`) mirrors the bootloader's `app_valid()` and adds the
   layout and build rules:
   - magic, `size == carried size`, `0xD0 <= size <= IMAGE_MAX`, size > 12 KB only with
     RING_HIGH, CRC (as today);
   - SP == 0x20000BE0, reset vector odd and in `[APP_BASE, APP_BASE + size)`;
   - flags & 0x07 == 0 (adds DIAG 0x04, as `embed_ztu.py` already does);
   - build not in 1..255 and not 0xFFFE or 0xFFFF. **Build 0 (2.1.x) is valid and served**: the
     ZTU refuses to offer, never to serve, an image the bootloader may install. It is offered
     only by the byte rule to a 7-byte identity, and the probe never takes it (0 is never
     newer). That keeps 2.1.12 carriable for a rollback, and lets a unit with no app recover
     from it.
   - `IMAGE_MAX` stays 12 KB in phase 3. It goes to 14 KB only in the ZTU release that carries
     the first >12 KB app, after the ring move (section 1).
5. **Requests v2.** `answer_block()` keeps reading bytes 0-5. When the request has 16 bytes, the
   mode, the bootloader version and the installed build go to the link diagnostics; a 6-byte
   request is logged as a v1 bootloader. Counter-rescue and button requests are logged as such
   ("MCU in rescue"), so a crash loop shows in the ZTU log. Reporting it to Z2M, or serving a
   second image to a rescue, is a later ZTU change that needs nothing from the bootloader.
6. **Boot timing diagnostic.** `hal_millis()` at the end of `tuya_mcu_app_init()` (UART up) and
   at the first answered 0xA1 go into the link diagnostics. B2 and every ZTU release check them
   against the contract. If the margin gets thin, the ZTU starts its MCU UART before
   `hal_zigbee_init()` and polls it once between the init steps; that is a ZTU change over the
   air, not a bootloader change.
7. **Stock MCUs.** They are never asked for their identity (major 1, `tuya_mcu_ota.c:197`) and
   never send 0xA1, so nothing changes for them.
8. **Display.** `mcu_version` keeps the exact "x.y.z own" format. The converter parses it, and a
   changed format risks the 2026-11-17 class of entity loss. The build and bootloader version
   go to the link diagnostics only; a Basic attribute for them is a later, separate converter
   change.
9. **Tests** (`tests/test_mcu_ota.py` and the stub build):
   - build-aware offers to a 10-byte identity; byte rule for a 7-byte identity; 11-byte 0xA0;
   - `image_info`: DIAG, SIM, MEASURE, builds 1..255, 0xFFFE, 0xFFFF, a bad SP, a reset vector
     outside the image or even, and >12 KB without RING_HIGH are neither offered nor served;
     build 0 is served, and offered only by the byte rule;
   - a 0xA1 at offset 0xC0 with max_len 16 (6 and 16 request bytes) answered with exactly the
     16 header bytes and no first-contact reset;
   - the same request during the failure pause gets no answer;
   - a fuzz over the contract's request space (offsets, lengths, 6..16 bytes, random tails):
     no crash, every answer well formed;
   - identity asked after a seq-0 query, after an offset-0 block request and after an OK
     result, with and without a carried image;
   - stub timing: the first 0xA1 answer within the contract from the stub's boot.
10. **py32-bump** (fork workflow): compare builds instead of version strings. Refuse a carried
    build ≤ the one `home-dimmer` carries unless `force` (force is the rollback path of section
    6). Print the OTA file size against the 212 992-byte limit (section 12).

---

## 11. Tools and CI

| Tool | Change |
|---|---|
| `tools/mkimage.py` | Read and print the build. Refuse: build 0 or 1..255, 0xFFFF for an image it creates; 0xFFFE unless SIM; SIM without 0xFFFE; an image CRC of 0; size > 12 KB without RING_HIGH; size > 14 KB; flags 0x20..0x80; a build ≥ 256 without CONFIRMS. Keep today's version-byte checks. `--full`: refuse a boot.bin without `boot_id` at 0x10 unless `--old-boot`; accept an existing 2.1.x app image (build 0, no CONFIRMS) only with `--legacy-app` (bench test B6). (That RING_HIGH is set exactly when `_settings_start` is 0x08004800 is a CI check on the ELF with `arm-none-eabi-nm`.) |
| `tools/embed_ztu.py` | All `assert`s become `sys.exit` checks (item 42), so `python3 -O` cannot skip them. Mirror the phase-3 `image_info` (section 10): flags & 0x07; SP == 0x20000BE0; reset vector odd and inside the image; size ≤ 14 KB and > 12 KB only with RING_HIGH. Build 256..0xFFFD with CONFIRMS; build 0 only with `--legacy` (version byte ≤ 0x9C, no CONFIRMS, no RING_HIGH: a 2.1.x rollback). |
| `tools/release/release.sh` | `FW_BUILD` == the previous tag's + 100 (`git show <tag>:src/tuya.h`), so every release is above every test build before it. Version byte > the previous tag's (tags are unique per byte). The first release over 12 KB is refused unless an earlier tag has RING_HIGH (the ring mover). The bootloader size and the boot sector CRC (DP 136 value) go in the release notes. |
| `tools/swd/install.sh` | Show the image's bootloader version (boot_id) and boot sector CRC. Refuse a full image without boot_id unless `OLD_BOOT=1`, and one over 12 KB unless `ERASE_SETTINGS=1`. `IMAGE=release` checks `SHA256SUMS` (item 53). Optional `WRP=1` (section 1). Write 0 to the boot word pair (0x20000BF8, 0x20000BFC) before every reset it issues. Tools that halt the core must set DBGMCU `DBG_IWDG_STOP` first: v2 starts the watchdog at every boot (install.sh:124 and `make flash` already do). |
| `tools/ota_swd_emu.py` | Answer the header request (0xC0, 16) before the blocks, accept 16-byte requests, send the 11-byte 0xA0 (build) so that build-only test images are accepted, and clear the boot word pair before its resets. |
| `Makefile` | `FW_TEST=` (a new BUILD dir like `VERSION_BYTE`). `make flash` clears the boot word pair before its reset. `make test-boot`, part of `make test`. Report the bootloader size from the ELF; warn above 3584 bytes. |
| `.github/workflows/ci.yml` | The host test job runs `make test`, which includes `test-boot`: plain `cc`, no ARM toolchain and no zlib (the harness has its own CRC), Linux ≥ 4.17 for `MAP_FIXED_NOREPLACE` (ubuntu-24.04 has it). The build job checks `boot.elf` with `arm-none-eabi-nm`/`objdump`: `k_boot_id` at 0x08000010 and 16 bytes; `_flash_write_start`/`_end` exactly 0x08001000/0x08004800; `boot.bin` ≤ 4096; `_ebss` + 512 bytes of stack ≤ 0x20000BE0. The ZTU's CI prints its OTA file size with the carried image (section 12). |

### `make test-boot` (tests/boot/, grown from the review harness)

**Build.**
- `boot/boot.c` is compiled unmodified with `#define main boot_main`, `-Wall -Wextra -Werror`.
- A fake `py32f0xx.h` routes USART1, SysTick, IWDG and GPIOA through hook functions; RCC and SCB
  are plain structs. `__set_MSP` and `NVIC_SystemReset` end the forked child.
- Flash (0x08000000, 20 KB), RAM (0x20000000, 3 KB) and the factory trim page (0x1FFF0000) are
  mapped at their chip addresses. Every boot is a `fork()`, so `.bss` is fresh as after
  `Reset_Handler`.

**Flash model** (a fake `flash_write_page()`, used by most scenarios):
- identical page → no erase;
- 4.5 ms per write;
- power-cut injection after the erase or after the program;
- asserts page alignment and the write range 0x08001000..0x080047FF;
- records the highest address written (for the ring-limit cases).

Its contract (returns 1 iff the page reads back equal; an identical page is not erased) is the
same list `tests/test_flash_drv.cpp` checks on the real `src/flash_drv.c` against
`tests/host/flash_model.hpp`. One scenario links the real `src/flash_drv.c` and its guard
against a fake FLASH peripheral instead, including a controller whose BSY never clears (FLASH
result, no watchdog reset).

**USART and time model** (the review's harness delivered whole frames into the SWD injection
buffer, which never ran the USART path and had no wire time):
- the ZTU model's bytes reach a fake RX register one every 87 µs: one-byte DR, RXNE, ORE, and
  the byte lost when DR is not read in time; TX busy 87 µs per byte;
- `crc32_update` costs 3.75 µs per byte, a page write 4.5 ms, a SysTick read 20 µs;
- every IWDG key write is recorded; any scenario whose largest gap between refreshes exceeds
  250 ms fails;
- `g_inject` keeps one dedicated test (the SWD path).

**ZTU model.** A C port of `answer_block`: stateless; `BLOCK_MAX` 64 or 128; a ready time after
power-up; an answer latency; failure counting and pause; serve-below-offset; image switch at
page k; drop, late duplicate and bit-flip noise; unsolicited frames; it records the mode byte
and counts header and offset-0 requests.

**Reset model.** `RCC_CSR` per boot: cold PWR|PIN with random RAM; warm SFT/IWDG; stale PWR
after the bootloader's own reset. App model: confirms or not, crashes or not, clears the flags.
The module button: held from power-up, released at a given time or never.

**Register trace.** The first GPIOA write is BRR of PA0/PA1; HSI_FS 4; USART BRR 208; IWDG PR 3
and RLR 1000.

**Images.** Synthetic, from `tests/boot/mkimages.py`: a valid vector table, header and CRC,
chosen builds and flags, and controlled identical pages (for example page 0). No SDK needed.

**Scenarios** (each with a pass condition; the prototype passed all of them in the byte-timed
model):

| Group | Scenarios |
|---|---|
| Update | Asked normal (done; next boot tries 1, no probe); asked + ZTU silent (back to the app after 10 s); power cut after erase and after program at every page (all recover); image change mid-transfer (new image within ~15 s); ZTU answers only an identical page 0 (old app after 10 s); noisy link, 50 seeds (all succeed); 128-byte blocks; unsolicited ZTU frames every 1000/300/100/50 ms; FLASH result path (a page that never verifies: the ZTU sees 3 FLASH results and pauses; 362 requests in 60 s, no tight loop); carried image with a bad SP: refused at page 0, nothing written |
| Probe | Newer build taken, no second probe after its reset; same image, older build, old image without build, both without build: not taken; installed 2.1.x without build: taken; ZTU silent: app at the window; ZTU ready at 1.4 s and 2.9 s: taken; after the window: not; 13 KB over a 12 KB-ring app refused; over a RING_HIGH app taken; DIAG / MEASURE / SIM installed: no request at all; carried SIM and DIAG refused; wrong magic refused; answer latency sweep 20-201 ms: always found; the B3 case (the probe does not depend on the app's offer path): newer build taken after a power cut |
| Rescue | 3 unconfirmed boots then windows; same CRC skipped, 0 offset-0 requests; older image taken; window length 30 s; `restored` marked; 13 KB on a 12 KB-ring app refused; bench images refused (counter, button, no app); power cut during a rescue transfer; duty cycle of an app that hangs in init |
| Button | Held and released at a cold boot: rescue, older taken; same image: app after ~30 s; released after 4.5 s: rescue; never released: app after 5.1 s, STUCK, nothing written; held at a warm boot: ignored; not held: no delay |
| Counter | 2.1.x app (no CONFIRMS) with 8+ watchdog resets: never a window; bench build with CONFIRMS: never counted; confirming app with resets between confirmations: never a window; never confirmed: every warm reset after the third opens a window; asked update resets tries; written image resets tries; stale PWRRSTF + IWDG counts as warm |
| Mailbox | Random RAM at power-up, 200 boots: never asked, word rebuilt, restore words untouched (byte compare); `restored` written only by a counter rescue; mode byte of every request |
| Fuzz | 300 boots in every mode with mutated and random ZTU frames that pass the frame checksum (bytes, lengths, offsets, sizes, CRCs, other commands), each followed by a clean ZTU: no fault, no write outside the app area, largest watchdog gap < 250 ms, every unit ends with an intact app (*measured*: 0 of 300 failed, ~43 000 answers) |

CI runs it with every push. It is the gate before any SWD install on a module.

---

## 12. Size and RAM budget

*Measured* on the prototype: arm-none-eabi-gcc 13.2.1, `-Os`, the flags of the Makefile's
bootloader build, with the shared flash driver as it will be (no shim, bounded wait, range
guard). Each fix's cost is measured by adding it alone to a build with all of them off; some
overlap (the parts add up to 3204).

| Part | Bytes |
|---|---|
| v1 as released | 2136 |
| First prototype (before the review): one loop for all modes, watchdog start, try counter and boot word, button, probe, boot info, progress give-up, paced rejects, receive rescan, `boot_id_t`, LED patterns | 2780 |
| Header first in every mode (one header rule for all modes, always pinned, the vector rule shared with `app_valid()`) | +128 |
| Page-0 vector check before the first write | +60 |
| Request v2 and result with the build | +24 |
| Button: release within 5 s, LED on, STUCK | +52 |
| Counter rescue marks `restored` | +16 |
| Watchdog period, bounded LSI and IWDG waits | +44 |
| No request while a frame is half received | +52 |
| WRITTEN / STUCK info bits | +12 |
| Bench builds not counted | 0 |
| Write-range guard (flash driver) | +36 |
| **Prototype total** | **3216 of 4096 (880 free)** |

The three user-requested paths (counter with watchdog, button, probe) took 464 bytes in the
first prototype, above the ~300-byte estimate. The review's fixes add 436 bytes more, mostly
for the shared update path (header first, page-0 check, half-duplex, watchdog period) rather
than for the three paths. The slot is the bootloader's alone, so the extra costs nothing else.
The implementation re-measures; the half-duplex guard's 52 bytes for one condition look like a
code-layout effect. If it must shrink, the order is: info bits (-12), LED blink pattern (-20),
boot info (-44). This design keeps all of them.

RAM: `.data` 48 bytes (the RAM flash routine), `.bss` 520 bytes (RX buffer 150, injection
buffer 150, page buffer 128, request 16, state), stack 512. That is ~1.08 KB of the 3040 bytes
below the mailbox.

**The app ceiling in practice: the ZTU's OTA slot.** A ZTU OTA file may be at most
`FLASH_OTA_IMAGE_MAX_SIZE` 0x34000 = 212 992 bytes (fork `src/telink/Makefile:253-266`,
`make_ota.py` `check_ota_size`). The ZDMS16 build of 2026-11-17, carrying a 11 628-byte app,
was 211 554 bytes (project review measurement): 1 438 bytes were left. A 12 KB app needs ~660
of them, a 14 KB app ~2 700, before any phase-3 ZTU code. So within this design apps stay near
12 KB until the ZTU build frees space; that is the first precondition of the ring move (section
1), and CI prints the ZTU OTA size with each new carried image. The frozen bootloader can never
take a compressed image, so this limit does not move from the MCU side.

---

## 13. Failure modes: what could still need SWD

| # | Failure | v1 today | v2 | Residual |
|---|---|---|---|---|
| 1 | CRC-valid app crashes or hangs in init (item 0) | Endless loop, SWD | IWDG from the bootloader; 3 unconfirmed boots, then rescue windows (~96 % listening); the ZTU carries any other image; the light uses the power-on behaviour afterwards | The ZTU must get another image (Zigbee OTA) |
| 2 | App runs, but its update path is broken (RX, parser, 0xA3/0xA0, version compare, mailbox) | SWD | Power cut, then the probe takes a newer build; or the button | Needs a power cut or the button, and a ZTU that keeps the contract (section 10) |
| 3 | App runs but misbehaves; a rollback is wanted | Re-release as newer | Re-release under a new build (cheap now), or the button with the older image carried | - |
| 4 | Power cut during any update page | Recovers (review: 182 cases) | Recovers (162 + 12 cut points) | - |
| 5 | ZTU image changes mid-transfer (item 28) | Dark until a power cut | Unpinned after 10 s, new image | - |
| 6 | ZTU silent after an identical page 0 (item 28) | Dark until a power cut | Old app after 10 s | - |
| 7 | ZTU pauses after failed results (item 41) | Dark during the pause (if the app is gone) | Same | Up to 24 h dark in the worst back-off. A ZTU restart (mains cycle) clears the pause. |
| 8 | ZTU carries a broken but valid image | Installed, then case 1 | Installed, then case 1; the same image is skipped in rescue | Needs a ZTU OTA with a fixed image |
| 9 | ZTU carries a CRC-valid image with bad vectors | Installed, app gone, dark | Refused before any write (BAD_IMAGE, ZTU pause after 3); also refused by the phase-3 ZTU and `embed_ztu.py` | - |
| 10 | ZTU's own firmware broken (link dead) | MCU fine, no updates | Same | Fixed through the ZTU's Zigbee OTA; if that is broken too, the ZTU needs its own serial flashing (out of scope) |
| 11 | ZTU firmware boots slower than the probe window | - | 3 s window, ZTU contract 1.5 s, checked per ZTU release (B2) and visible in DP 137 | A ZTU release that breaks the contract and slips through the checks disables escape (b) until a fixed ZTU release |
| 12 | Image too large for the installed ring layout | Refused (12 KB) | Refused by the offer path and every bootloader mode with an installed app | Only "no valid app" ignores the layout (no code damage, possible older settings) |
| 13 | Version space runs out (item 31) | Byte 0xFF ends OTA | Build numbers 256..0xFFFD, +100 per release (~650 releases); the byte bumps only for releases | - |
| 14 | Bench image carried by mistake | ZTU refuses SIM / MEASURE | ZTU refuses all three; the bootloader refuses them in every mode | - |
| 15 | App writes into sector 0 by a bug | Bootloader gone, SWD | Range guard in the app's flash driver; WRP available | Two bugs needed; WRP off (section 1) |
| 16 | LSI never ready | - (the app would hang in `watchdog_start`) | Bounded waits; the watchdog then does not run | Hardware fault |
| 17 | Button stuck pressed | - | 5 s per power-up, then the normal boot; nothing installed; STUCK in DP 137 | 5 s delay per power cut |
| 18 | ZTU that never asks 0xA3 above an Open Dimmer MCU | Never updated | Every warm reset after the third: a 30 s window, then the power-on behaviour | A ZTU without own-MCU support is forbidden (STATUS); a ZTU without an image asks from phase 3 on |
| 19 | App whose UART is dead and which never resets | SWD | Power cut (probe, if newer is carried) or the button | No automatic path; an app-side link watchdog is possible later (section 7) |
| 20 | Defect in v2 itself | - | Harness gate (section 11), spare trial (section 14) | Frozen: SWD, or a boot-updater app (~150 ms brick window) while WRP is off |
| 21 | Brown-outs in a row | Each boots the app | Each is a cold boot with one probe (the ZTU usually answers at once if it did not reset) | ≤ window delay per dip |
| 22 | Flash page that never verifies, or a controller that stays busy | FLASH results, ZTU pause (busy: watchdog loop) | FLASH results, ZTU pause; the busy wait is bounded | Hardware: SWD cannot fix it either |
| 23 | Wrong assumption about the reset flags (B8) | - | Worst case: a cold boot read as warm skips one probe and keeps counting tries | Verified on the spare before the modules |

---

## 14. Rollout

1. **Implementation** on a branch from this design:
   - `boot/boot.c`, `boot/boot.ld`, `src/ota_proto.h` (with the ZTU contract), `src/flash_drv.c`;
   - the app changes of section 9;
   - `tests/boot/` and `make test-boot`;
   - the tools of section 11; STATUS.md; an INSTALL.md section on the button rescue.

   CI green, then a review of the bootloader diff against this document.
2. **Release R** vX.Y.Z (build 300) with v2 and the counted app. Record the boot sector CRC.
3. **ZTU phase-3 release** (section 10), carrying R's app. OTA to the spare's ZTU first. The
   spare still has v1 + 2.1.12: the ZTU offers R's app by the byte rule (7-byte identity), and
   the spare takes it under v1. That runs the matrix rows "2.1.x | v1 | phase 3" and "v2 app |
   v1 | phase 3". Then the other modules' ZTUs one at a time: with their stock MCUs nothing
   changes, and the check is the same as phase 1.
4. **Spare** (mains off, probe on CN2): `IMAGE=<R's zdms16_py32_full.bin> tools/swd/install.sh`.
   This also exercises install.sh's re-flash path (item 8 / STATUS hardware gate). The settings
   in sector 4 stay untouched.
5. **Bench tests on the spare** (mains rules apply; the button and SWD steps with mains off):

| # | Test | Pass |
|---|---|---|
| B1 | After the install: read-back equal; DP 136 = the release's CRC; ZTU log shows bootloader version 2 and requests of 16 bytes | as stated |
| B2 | 20+ mains power cycles, nothing newer carried: light delay (timed), DP 137 probe time each run, the ZTU's boot timing diagnostic; then 3 more right after a ZTU OTA (its first boot) | Every probe time ≤ 1500 ms (the contract), delay = probe time + ~50 ms, no update. A probe time of 0 while the ZTU serves means > 3 s: rebuild once with a 4000 ms measurement window to see the real time. If the contract is missed, the ZTU is fixed (section 10, item 6) before any further module; the frozen window stays. |
| B3 | Broken update path: a test build in the range below R (e.g. 299), normal flags with CONFIRMS, its offer handler patched to decline (SWD); the ZTU carries R | The offer is declined (ZTU log); after a power cut the probe installs R (DIAG and MEASURE builds cannot be used here: the probe skips bench builds) |
| B4 | Crash loop: a test build R+1 that faults 1 s after start (SWD), ZTU carries R | 3 crashes, then a window installs R; DP 137 shows RESCUE and WRITTEN; the light uses the power-on behaviour |
| B5 | Button held through power-up and released when the LED lights; carried image different / same; then held for > 5 s | Different: installed, DP 137 BUTTON. Same: app after ~30 s. Held: app after ~5 s, STUCK, nothing written. Never a rejoin or factory reset. |
| B6 | 2.1.12 app under v2 (full image from the v2.1.12 app with `--legacy-app` and v2 boot), watchdog resets via a halt without `DBG_IWDG_STOP` | No window; a power cut lets the probe take the carried build |
| B7 | Mains cut at ~50 % of a probe-started update (fast LED blink), 3 times | Each recovers on the next power-up |
| B8 | Reset flags: DP 131 after a power cycle that follows a software reset; flags unchanged by the bootloader's LSION write; IWDG running or not after `NVIC_SystemReset` (SWD) | PWR only (cold); results recorded in STATUS |
| B9 | Normal offered OTA with both channels lit | Light restored; dark time measured |
| B10 | 1-2 weeks trial: health DPs 131-137 | No unexpected resets; DP 137 shows tries 1 at every start (each run before it confirmed) |

6. **Further modules**, each at its migration slot (STATUS migration order): install.sh stock
   path with the same release full image, then record DP 136 and 137 per module. From then on
   their bootloader never changes. App and ZTU updates go over Zigbee, the spare first, as the
   process rule says. After every ZTU rollout, a cold boot with probe time 0 in DP 137 on any
   module is a contract miss.
7. **Later, only when the app approaches 12 KB:** the ring-move release M (section 1), its
   preconditions, spread and verified everywhere, before any larger app.

**Open measurement before freezing v2:** the ZTU's answer time after a power-up (B2), checked
against the 1500 ms contract. It no longer sets the window. Within the contract the light delay
after a power cut (answer time + ~50 ms) stays inside the 1-2 s the user accepted.

---

## 15. Review resolution

Three reviewers attacked the first version of this design (commit f1a5ddd): recovery and
bricking (R1), protocol and compatibility (R2), size, timing and implementability (R3). No
blocker was found. Every major and minor finding is listed with its resolution. Findings that
several reviewers made are resolved once and cross-referenced.

| ID | Sev. | Finding | Resolution |
|---|---|---|---|
| R1-1 | major | A counter rescue after a crash before `dimmer.c:215` leaves `restored` at 0, so the image installed hours later restores a stale light state | Adopted in frozen code: the bootloader sets `restored` = RST1 when it opens a counter rescue window (+16 B). Sections 3, 4; harness scenario `restored`. |
| R1-2 | major | The frozen probe window is an unwritten timing contract with a ZTU that changes over the air | Window frozen at 3000 ms instead of 1.5 × the spare; ZTU contract (0xA1 semantics, robustness, 1500 ms, identity after every start, results) in `ota_proto.h` and `tuya_mcu_ota.h`; fuzz and timing tests; B2 on every ZTU release incl. its first boot after an OTA; DP 137 watch after each rollout. Sections 5, 10, 14. |
| R1-3 | minor | Rescue and no-app mode install SIM/DIAG images carried by mistake | Adopted: every mode asks for the header first and refuses bench flags, a wrong magic and a size beyond the layout, before anything is written (no-app mode too). Sections 6, 8; scenario `benchrescue`. |
| R1-4 | minor | The bootloader leaves the IWDG period to reset values or the last app | Adopted: PR 3 / RLR 1000 with bounded waits (+44 B). Section 4. |
| R1-5 | minor | `tries` saturates: every warm reset after the third opens a window, not every third; a lost first query means no confirmation | Text corrected (sections 6, 7, 13 #18); *measured* 7 windows in 10 boots. App over the air: seq 0 until the ZTU answers (section 9, item 9). |
| R1-6 | minor | Section 10 serves build 0 in one item and forbids it in another | One rule: the ZTU refuses to offer, never to serve, what the bootloader may install; build 0 is served. Same as R2-1. |
| R1-7 | minor | Ring move: a post-M app without the lower-half scan loses settings after a downgrade; step 2 checks the version, not the copy; 16 pages halve endurance | Adopted: a generic scan rule for every app from M on; M reports "ring moved" and step 2 checks it; endurance and save rate are a precondition of M. Section 1. |
| R1-8 | minor | Counter rescue ignores the ring layout and can overwrite live settings | Adopted for every mode with an installed app, the button included (user decision 2: an image never overwrites settings); only "no valid app" stays ungated. Sections 1, 6; scenario `bigrescue`. |
| R1-9 | minor | A stuck button gives a 30 s window and an unintended install at every power cut | Adopted: held 100 ms, then released within 5 s; a stuck button costs 5 s and installs nothing (+52 B with R2-6). Section 4; scenario `stuck`. |
| R1-10 | minor | The harness cannot see IWDG gaps, parser faults or a wrong guard range | Adopted: IWDG key model with a 250 ms bound, CRC time, frame fuzz, the real flash driver against a fake FLASH peripheral, `nm` checks of the guard range. Section 11; the prototype ran the fuzz (0 of 300 failed). |
| R1-11 | minor | No tool requires CONFIRMS; no build wrap rule; test builds can outnumber the next release | Adopted: `mkimage`/`embed_ztu` require CONFIRMS for builds ≥ 256; releases exactly +100; test builds +1..99 (`FW_TEST`). Sections 2, 11. |
| R1-12 | minor | An app with a dead UART that never resets is never rescued automatically | Not adopted in frozen code (none needed). An app-side link watchdog would disturb a working light whenever the ZTU is the broken side; left for a later OTA decision. Sections 7, 13 #19. |
| R2-1 | major | Build-0 rules contradict each other; rollout step 2 left the phase-3 ZTU nothing to carry | One rule (R1-6); rollout reordered: release R first, the phase-3 ZTU carries R's app, the spare takes it under v1; matrix row "2.1.x / v1 / phase 3" added. Sections 2, 10, 14. |
| R2-2 | major | Frozen requests say nothing about the bootloader or its mode, so no future ZTU can tell a crash loop from an update | Adopted: 16-byte request (bootloader version, mode, installed header bytes 8..15) and a 4-byte result with the build (+24 B); a 6-byte request means v1. Header-first also cut a rescue window's offset-0 requests from 61 to 0. Section 8. |
| R2-3 | major | ZTU timing and 0xA3 behaviour are not written down; "the UART cannot start earlier" is wrong | Contract and tests as R1-2; identity asked without a carried image (phase 3); boot timing diagnostic; the claim corrected (the pins are parsed at `config_parser.c:359`, but an earlier start only buffers without a poll during init; kept as a ZTU-side option). Sections 5, 7, 10. |
| R2-4 | minor | `image_info` checks less than `app_valid()`; a bad-SP image replaced a working app | Adopted twice: the phase-3 `image_info` mirrors `app_valid()` plus layout and build rules, and the bootloader checks page 0 before the first write (+60 B). Sections 8, 10; scenario `badsp`. |
| R2-5 | minor | Saturating `tries` (as R1-5); a ZTU without a carried image never asks 0xA3; SWD benches fall into windows | As R1-5; phase-3 ZTU asks regardless (R3-9); bench builds are never counted; tools clear the boot word before their resets. Sections 3, 7, 11. |
| R2-6 | minor | A button rescue to 2.1.11 or older with the button still held can factory-reset the module | Adopted: the window opens only after the release, so no app sees the press. Section 4. |
| R2-7 | minor | The 11-byte offer rule drops today's guards; a SIM app takes byte offers | Adopted: `256 <= build <= 0xFFFD`; SIM builds decline every offer; phase-3 `image_info` refuses 0xFFFE. Sections 2, 9, 10. |
| R2-8 | minor | Spare test builds are not recorded, so a release can be numbered below one | As R1-11 (ranges of 100). |
| R2-9 | minor | The ZTU's OTA slot, not `BOOT_APP_MAX`, caps the app (1 438 B headroom) | Adopted: section 12 states it; a precondition of the ring move; `IMAGE_MAX` stays 12 KB in phase 3; CI and py32-bump print the OTA size. |
| R2-10 | minor | The probe's bench mask is hard-coded to 0x07; future build kinds need bits | Not adopted. The three bits form one bench class for the bootloader, and a new kind sets one of them; keeping 0x20..0x80 ignored by frozen code leaves them free for meanings the bootloader must never act on. Section 2. |
| R2-11 | minor | B3 cannot use DIAG/MEASURE builds, and its build must be below the carried one | Adopted: B3 specified (normal flags, declining offer path, build below R) and the same case in `make test-boot`. Sections 11, 14. |
| R3-1 | major | B2 cannot measure answers beyond the window; freezing 1.5 × the spare is fragile | As R1-2: 3000 ms frozen; B2 checks the 1500 ms contract and uses a 4000 ms measurement build only if an answer is missed; ZTU diagnostic. A window set from the app's header flags was considered and not adopted: it costs flag bits, and the escape's timing should not come from the app it escapes from. Sections 5, 14. |
| R3-2 | major | The harness skips the USART path, wire time, CRC time and IWDG refreshes, so its times are 2-2.6× too short | Adopted in the `make test-boot` spec, and every number in this document re-derived from a byte-timed model of the revised prototype. Section 11. |
| R3-3 | minor | A request sent while an answer arrives destroys it (17 of 20 probe misses at ~97 ms latency) | Adopted: no request while a frame is half received (+52 B); 20 of 20 at every latency. Section 8. |
| R3-4 | minor | `watchdog_start()`'s LSION read-modify-write of RCC_CSR comes before the flags are read | Adopted: the flags are copied first (0 B); B8 also checks them. Section 4. |
| R3-5 | minor | Test builds and typos in `FW_BUILD` | As R1-11. |
| R3-6 | minor | Build 0 rules | As R2-1. |
| R3-7 | minor | pyOCD's reset is a software reset, so an old boot word survives a re-flash; bench sessions fall into windows | Adopted: `make flash`, `install.sh`, `ota_swd_emu.py` clear the boot word pair; bench builds never counted. Sections 3, 11. |
| R3-8 | minor | The button is read late (after `app_valid()`), and a late press held 3 s factory-resets the module | Adopted: read before `app_valid()`, ~1 ms after reset; the app ignores a ≥ 3 s hold that began in the first 10 s after a cold start; INSTALL wording "press before switching on". Sections 4, 9. |
| R3-9 | minor | The ZTU asks 0xA3 only when it carries an image | Adopted in the phase-3 ZTU and in the contract. Section 10. |
| R3-10 | minor | The flash busy wait (~0.67 s) outlasts the cold-boot watchdog | Adopted: bounded at ~50 ms in the shared driver; a BSY-never-clears test. Section 8. |
| R3-11 | minor | `-Werror` fails on the jump cast; the harness links zlib; wrong `flash_drv.c` line references | Fixed: cast through `uintptr_t`; the harness has its own CRC; references corrected (`flash_drv.c:46, 54-58, 83-86, 90-94`). Sections 4, 8, 11. |

Measured corrections that came with the review: `app_valid()` takes ~43-54 ms, not ~25 ms; the
DP 136 CRC takes ~15 ms, not ~7 ms; an asked update takes ~2.5 s in the byte-timed model, not
0.9 s.
