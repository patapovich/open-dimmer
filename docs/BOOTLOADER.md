# Bootloader v2: design (fix plan phase 3)

Status: design, 2026-11-18, revised the same day after an adversarial review of this design
(three reviewers), again after two factory-firmware compatibility reviews (section 15), and
once more with the user's decisions of the same day (the link-watchdog hook among them).
Every finding, decision and its resolution is in section 16. Implemented and integrated on
2026-11-19 (bootloader, app side, tools; release R = 2.1.13, build 300), tested on the PC only:
the readings the implementation took where this text left a choice are listed in section 17.
Nothing of it has run on hardware yet (bench tests, section 14). The code references below are
the design's: to this repo at 421685a and to the fork `patapovich/tuya-zigbee-switch`,
branch `home-dimmer` at 51f5c95a (paths under `src/zigbee/` there are marked "fork").
Addresses marked "stock ZTU" are in the public dump of the `_TZE284_jtbgusdc` ZTU (sha256
f4afb7a8..., named in `docs/stock-firmware-analysis.md`), and "WiFi 1.0.3" ones in the WiFi PY32
dump of the same document. Item numbers refer to the project review of 2026-11-18, which is
kept privately (see STATUS.md).

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
  2-2.6 times shorter times;
- the compatibility reviews' runs (section 15): the fork's stub build of 51f5c95a against a
  stock-like MCU model, and static disassembly of the WiFi 1.0.3 and stock ZTU dumps. Nothing
  ran on hardware.

## Decisions in short

| # | Topic | Decision |
|---|---|---|
| 1 | Layout | Bootloader alone in sector 0 (4 KB). It takes apps up to 14 KB (`BOOT_APP_MAX` 0x3800). Every mode with an installed app keeps a new image inside that app's settings layout (12 KB, or 14 KB with the header flag RING_HIGH); only "no valid app" takes up to 14 KB. Settings ring stays at 0x08004000 (4 KB) until an app needs more than 12 KB; a "ring-move" release first moves it to 0x08004800 (2 KB). Today the ZTU's OTA slot, not the bootloader, caps the app near 12 KB (section 12). Sector 0 is not write-protected in phase 3. |
| 2 | Versions | 16-bit build number in the app header's reserved bytes (0xCE..0xCF). Releases are multiples of 100 (the first 300), test builds take the 99 numbers after a release. The version byte stays for display, the converter and old peers. Identity (0xA3) and offer (0xA0) get the build appended; old peers ignore the extra bytes. Build 0 (2.1.x) images stay valid and servable. |
| 3 | Mailbox | The two reserved words become the boot word (tries, rescue info, probe answer time, the app's probe request) and its complement. The bootloader writes `restored` only when it opens a counter rescue, so a crash loop never brings back a stale light state. |
| 4 | Boot flow | Gates low first; the reset flags are copied before any RCC write; the bootloader starts the watchdog with its own ~1 s period. Mode order: update (asked, or no valid app) > rescue (button, or 3 unconfirmed boots) > probe (a cold reset, or a warm reset the app asked one for) > jump. |
| 5 | Probe | After a cold reset, or a warm reset whose boot word carries the app's probe request (row 14): header-only 0xA1 every 100 ms for up to 3 s (frozen, generous on purpose; user decision U1). The ZTU commits to answering within 1.5 s of power-up (contract, section 10). The light comes at the ZTU's answer time + ~50 ms; the full 3.1 s only when the ZTU cannot answer (always under a stock Tuya ZTU, which is not supported). Takes only a valid, larger build number that fits the installed settings layout. |
| 6 | Rescue | 30 s listening windows (user decision U3). Takes any non-bench image the ZTU serves whose CRC differs from the installed one and that fits the layout, older ones included. After a counter rescue the light takes the power-on behaviour (U3). Button: held when the power comes, released within 5 s; the module sits in an insulated case, the button is pressed through it, and the case is never opened on mains (U2). |
| 7 | Confirm | The app confirms after 30 s of uptime once it has answered a 0xA3 in this run, or when no 0xA3 but at least two 0x01 product queries came (a stock Tuya ZTU). A confirmation sets the tries to 0 and keeps every other bit of the boot word. Only apps with the header flag CONFIRMS and no bench flag are counted, so 2.1.x apps and bench builds never fall into rescue loops. |
| 8 | Update path | Every mode asks for the carried header first and pins the image it takes; page 0's vectors are checked before anything is written. Give-up is based on progress (item 28). No request while a frame is half received. Requests carry the bootloader version, the mode and the installed image (16 bytes). The SR-flag shim goes; the flash wait is bounded at ~50 ms. |
| 9-11 | App, ZTU, tools | Listed in sections 9-11. Under an own ZTU every change except the bootloader ships over the air. Under a stock Tuya ZTU nothing reaches the MCU: the app installed over SWD stays until the ZTU is converted (section 15). |
| 12 | Size | *Measured* prototype with the link hook: 3252 of 4096 bytes (+1116 over v1's 2136); 844 bytes stay free. |
| 13 | Factory firmware | A hard requirement (user decision U5). A stock MCU behind the phase-3 ZTU gets exactly what phase 1 sends it, apart from one retry of a lost DP dump. Own commands go only to an MCU identified as Open Dimmer. Unidentified MCUs get bounded identity questions and short frames (contract items 4 and 6). Open Dimmer under a stock Tuya ZTU is not supported but safe, with its costs listed. Section 15. |
| 14 | Link watchdog | User decision U4. Frozen hook: boot word bit 7 (`BOOT_PROBE_REQ`), set by the app in a valid word right before a software reset, cleared by the bootloader at every boot. On a warm boot it runs the power-on probe with the same rules (newer build only, fits the layout, no bench build, 3 s window, ends at the first answer), counts as a probe and not a rescue (no window, no `restored` mark, no older image), sends mode 6 and sets bit 6 (`BOOT_INFO_LINK`). *Measured*: +36 bytes. The first v2 app (release R) ships the watchdog with defaults: no well-formed frame from the ZTU for 30 min, every channel off, the run at least 4 h old (at most one restart per ~4 h); then the planned restart with the request and tries 0. The light state comes back by the restore rule; the wall switch is not read for ~3.1 s at most when nothing newer is carried. Sections 3, 5, 9 (item 11). |

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

Constants in `src/ota_proto.h` v2. The header holds every number of this document that two
sides share, with the frame formats, the boot word, the app's link-watchdog defaults and the ZTU
contract, and says for each field which side reads it. `APP_MAX_SIZE` goes away: its two users
each get their own limit (both use `APP_LIMIT_LEGACY` until their v2 code lands).

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
    uint8_t  features;   /* bit 0 try counter, 1 power-on probe, 2 button, 3 link probe request */
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
#define OTA_MODE_LINK      6             /* probe asked by the app's link watchdog, and its update */
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
1. **Release M (the ring mover).** Implemented as 2.1.14 (build 400, `RING_HIGH ?= 1` in the
   Makefile, `_store_scan_start` in the linker script, `store_load` in src/store.c, health DP
   137 bits 24 / 25). Same features plus:
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
12 KB therefore erases sector 4, the ring, unless the tool keeps it: `install.sh` reads sector 4
first and writes it back with the image's start merged in (the image ends below the ring at
0x08004800), as an update over the UART would; `ERASE_SETTINGS=1` erases instead; `make
flash-all` refuses such an image. docs/INSTALL.md says so.

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
- Bench builds below the release (B3, B11): `make FW_PREV=1 FW_TEST=n` builds
  `FW_BUILD - 100 + n` (e.g. 299 below release R). `FW_BUILD` itself is a literal that no make
  variable overrides, and B11's `LINK_WDG_TEST=1` builds only below the release, so a unit on the
  release never takes such an image. Anything else is a compile error (`src/tuya.h`,
  `tests/test_build_rules.py`).
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
| v2 app | v1 | phase 1 / phase 3 | byte / build | - | - | The spare between the app OTA and its SWD re-flash. `app_limit()` is 12 KB (no boot_id). The confirmation writes are ignored. The link watchdog stays off (no `BOOT_FEAT_LINK`). |
| 2.1.x | v2 | any | byte | yes (any build counts as newer than build 0) | no (no CONFIRMS) | Only after a rescue downgrade, or bench test B6. Button rescue still works. |
| v2 app | v2 | phase 1 | byte | yes | yes | A module whose ZTU lags behind |
| v2 app | v2 | phase 3 | build | yes | yes | Target |
| stock 1.x | stock | own ZTU, phase 1 / 3 | never asked | - | - | Phase 3 is UART-identical to phase 1 towards it (section 15) |
| Open Dimmer, any | v1 / v2 | stock Tuya ZTU | never (no 0xA0 / 0xA3 there) | v2: never answered, 3.1 s per power-up | none: the v2 app confirms by the stock-ZTU rule (section 7), 2.1.x has no CONFIRMS | NOT SUPPORTED, but safe (section 15) |

Section 15 has the full matrix, factory firmware included.

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
| 0x18 | `boot` (was `reserved[0]`) | - | read and written at every boot | - | reads at start; sets the tries to 0 to confirm; sets bit 7 (with tries 0) before a link-watchdog restart |
| 0x1C | `boot_check` (was `reserved[1]`) | - | written = ~boot | - | rewritten with every change of `boot` |

Boot word (`BOOT_*` in `src/ota_proto.h`):

| Bits | Field |
|---|---|
| 31..24 | Tag 0xB2. The word is valid only when the tag matches and `boot_check == ~boot`. Random RAM matches with p ≈ 2^-40; that matters for the first boot after an SWD install, a software reset with random RAM. |
| 23..12 | Answer time of the last probe since the power-up, in ms since its first request (1..4095), 0 = no answer or no probe. The power-on probe starts ~50 ms after power-up (clock, watchdog, `app_valid()`). A link probe (bit 6) clears the field before it starts, so with bit 6 set the field holds the link probe's time. `PROBE_WINDOW_MS` < 4096 is a static assert. |
| 11..8 | Info, all since the last power-up: bit 8 RESCUE (the counter opened a rescue window), bit 9 BUTTON (the button forced one), bit 10 WRITTEN (an image was written), bit 11 STUCK (the button was still held 5 s after power-up). |
| 7 | `BOOT_PROBE_REQ`, app to bootloader: probe at the next boot (the link-watchdog hook, section 5). The v2 app sets it only in a valid word, right before a software reset; the bootloader clears it at every boot, honoured or not, so a stored word never has it. |
| 6 | `BOOT_INFO_LINK`: a probe asked by bit 7 ran since the last power-up (set by the bootloader). |
| 5..4 | 0, reserved. |
| 3..0 | `tries`: unconfirmed boots of a counted app (CONFIRMS and no bench flag), 0..3. Every reader masks it (`boot & BOOT_TRIES_MASK`, 0x0F): bits 7..4 are not part of the count, and an unmasked compare would read bit 6 as 64 tries. |

Life cycle:
- **Cold boot** (section 4): the word is rebuilt as `0xB2000000`. Probe time, info and tries
  all count since the last power-up; a probe request from before the power cut is gone (the
  power-on probe runs anyway).
- **Warm boot**: the word is kept if it is valid, otherwise rebuilt. Bit 7 is read and cleared.
- **Asked update, or no valid app**: tries = 0, because the app just proved its update path or
  there is no app.
- **Image written** (result OK, just before the reset): tries = 0, WRITTEN set. A new image gets
  3 fresh runs.
- **Link probe** (bit 7 honoured, section 5): bits 23..12 cleared, LINK set.
- **Before every jump**: if the app is counted and tries < 3, tries + 1. The word is stored
  together with its complement.
- **App confirmation** (section 7): tries = 0, the other bits are kept.
- **Link-watchdog restart** (section 9, item 11): the app writes tries = 0 and bit 7 in one
  word (and its complement), then resets. So a link restart never moves the counter: after it,
  tries reads 1, as after a power-on probe.
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
- **Link-watchdog restart**: the app's planned restart (`dimmer_prepare_restart()`, `restored`
  = 0), as before an offered update. The light state comes back after it, also after the image
  a link probe installs (only SFTRSTF is set then): every channel off, since the watchdog fires
  only then, with the levels kept. The power-on behaviour is never used for it, so a lamp with
  power-on "on" never switches on by itself (user decision U4: "restores the state per the
  existing restore logic").
- Cost: when no run confirms (section 7), every warm reset after the third also takes the
  power-on behaviour instead of the restore. With the default "off" a lamp that was on stays
  off. With "on" a lamp that was off switches on by itself. Under a stock Tuya ZTU the app
  confirms by the 0x01 rule (section 7), so this cost does not arise there.

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
                             IWDG_SR wait bounded; KR 0xAAAA: the reload puts the new RLR in the
                             counter, so the period never depends on reset values or an app */
    cold  = !(csr & (SFTRSTF | IWDGRSTF));
    if (cold && held(BUTTON_MS)) {                 /* pressed before the power came */
        LED on;  stuck = held(BUTTON_RELEASE_MS);  button = !stuck;  LED off;
    }
    asked = mb->magic == UPDT && mb->magic_inv == ~UPDT;
    g_expect = asked ? mb->expect_crc : 0;   mb->magic = mb->magic_inv = 0;
    word  = (!cold && valid(mb->boot)) ? mb->boot : 0xB2000000;   if (stuck) info |= STUCK;
    link  = word & PROBE_REQ;  word &= ~PROBE_REQ;        /* the app's request: read, cleared always */
    tries = word & TRIES_MASK;                            /* bits 3..0 only, every read and write */
    h     = installed header;  ok = app_valid();  counts = (h->flags & (CONFIRMS | BENCH)) == CONFIRMS;
    limit = h->flags & RING_HIGH ? 14K : 12K;
    if (!ok || asked)                        { tries = 0; if (!ok) limit = 14K; mode = ok ? ASKED : NO_APP; run(GIVE_UP_MS); }
    else if (button)                         { info |= BUTTON; skip = h->crc; mode = BUTTON; run(RESCUE_WINDOW_MS); }
    else if (counts && tries >= 3)           { info |= RESCUE; mb->restored = RST1; skip = h->crc; mode = RESCUE; run(RESCUE_WINDOW_MS); }
    else if ((cold || link) && !(h->flags & BENCH)) {
        if (!cold) { probe_ms = 0; info |= LINK; }        /* request byte 7: 6 instead of 3 */
        mode = PROBE; run(PROBE_WINDOW_MS);
    }
    if (counts && tries < 3) tries++;
    store word, ~word;  jump_app();
```

The hook adds one operand to the probe's condition and nothing to `run()`: a link probe is the
power-on probe on a warm boot. It sits below the asked update and the counter rescue, so it can
never take their place, and a request the bootloader does not honour (asked, no valid app, a
rescue, a bench build) is dropped all the same.

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
nominal LSI, the value the app uses (`main.c:160-175`), and reloads it right after the update
wait (KR 0xAAAA, as the app's `watchdog_start()` does). A new RLR reaches the counter only at a
reload, and 0xCCCC does not reload an IWDG that is already running: without the reload, a warm
boot that only jumps (no mode, so no `tick()`) would keep the countdown a previous app left,
across `app_valid()` and before the boot counts. With it, it no longer matters whether a
software reset stops a running IWDG (the old comments say it keeps running, `boot.c:293`; never
verified, B8 records it), and an app that chose a short period cannot reset-loop the bootloader
before it counts (`make test-boot` scenario `wdg-carry`: a crash loop under an IWDG kept at
~4.9 ms over every reset opens its window at boot 4; *measured* without the reload in the same
model: every other boot ran out in `app_valid()`, `.R.R.RW`). The waits for LSIRDY and the IWDG
register update are bounded (~100 000 passes, ~25-40 ms; normally they end within a few LSI
cycles); with an LSI that never starts the watchdog simply does not run.

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

User steps for docs/INSTALL.md (user decision U2): the module sits in an insulated case, the
button is pressed through the case, and the case is never opened while the mains is connected
(INSTALL.md, "Mains voltage": the board is not isolated from the mains). Press the module button
through the case and keep it pressed, then switch the power on; when the LED lights, let go
within 5 s. The LED then blinks slowly while the module waits for an image and fast while it
writes one; without a different image it starts the light after 30 s.

### Timeouts

| Constant | Value | Why |
|---|---|---|
| `PROBE_EVERY_MS` | 100 | The light comes at most ~100 ms after the ZTU can answer. Never sent while a frame is half received (section 8). |
| `PROBE_WINDOW_MS` | 3000, frozen (user decision U1) | Generous on purpose: the probe ends at the first answer, so the window costs light delay only when the ZTU cannot answer at all (failure pause, no usable image, broken, or a stock Tuya ZTU). The ZTU commits to 1500 ms, half of it (section 10). < 4096 (boot word field). The link probe uses the same window. Section 5. |
| `BUTTON_MS` | 100 | Rejects bounce and the pull-up settling |
| `BUTTON_RELEASE_MS` | 5000 | Long enough to let go after the LED lights; the cost of a stuck button per power-up |
| `REQUEST_EVERY_MS` | 500 | Unchanged (boot.c:34): the request pace when nothing comes |
| `REJECT_PACE_MS` | 50 | A stray block (wrong offset, duplicate, short) is asked for again 50 ms later instead of at once (review: ~210 requests/s in the deadlock) |
| `GIVE_UP_MS` | 10 000 | Unchanged value, now measured from the last accepted block (item 28) |
| `RESCUE_WINDOW_MS` | 30 000 (user decision U3) | Section 6 |
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
        | (bit 7 of the boot word read and cleared)                                    |
        +------------------------------------------------------------------------------+
 no valid app | asked     button released | tries>=3 & counted  (cold | bit 7) & not bench  else
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
- On warm boots, unless the app asked for one (below). A crash loop is the counter's job, and
  an asked update brings its own image.
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
not answer them sooner. That ZTU change stays an option for later, and only under the
stock-MCU rules of section 10, change 6.

**Window: 3000 ms, frozen.** The first version froze 1.5 × the spare's measured answer time.
That would make the probe an unwritten timing contract with every later ZTU release: a release
whose first answer comes after the window makes every probe on every unit time out and jump,
and escape (b) is gone without any alarm (DP 137 just shows 0). The probe ends at the first
answer, so a long window costs light delay only when the ZTU does not answer at all: during its
failure pause, without a usable image, broken, or a ZTU that does not speak the protocol (a
stock Tuya ZTU, on every power-up). So:
- `PROBE_WINDOW_MS` = 3000, below the 4095 the boot word field can hold;
- the ZTU commits to its first header answer within 1500 ms of power-up, including the first
  boot after a ZTU OTA (contract, section 10);
- every ZTU release is checked against that on the spare before it goes further (section 14,
  B2), and DP 137 shows a miss on any unit under an own ZTU of phase 3 or later (a cold boot
  with probe time 0 and LINK clear). Under a stock Tuya ZTU every cold boot shows 0.

A window that the installed app's header could set was considered and left out: it would take
flag bits, and the escape's timing should not come from the app it escapes from.

**ZTU change for the probe: none needed to work.** A new "image info" command was rejected: it
would only work with ZTUs not yet built, and v2 would still need this request as a fallback.
The phase-3 ZTU adds the contract, its tests and a boot-time diagnostic (section 10).

**Light delay after a power cut** (*measured* in the byte-timed model, nothing newer carried):

| ZTU answers from (ms) | 0 | 100 | 300 | 600 | 1000 | 1400 | 2000 | 2900 | ≥3200 or silent |
|---|---|---|---|---|---|---|---|---|---|
| App starts at (s) | 0.05 | 0.15 | 0.35 | 0.66 | 1.06 | 1.46 | 2.07 | 2.98 | 3.12 |

Under a stock Tuya ZTU the last column applies to every power-up and brown-out, and wall-switch
presses in those 3.1 s are lost (v1 jumps at once, `boot/boot.c:343-345`). Section 15.

With a newer build carried, the app started after the probe and the update: 2.82 s with the ZTU
answering from 300 ms (2.14 s with 128-byte blocks), 3.93 s from 1400 ms. The other probe cases
all behaved as intended: same image, older build, a carried 2.1.12 without a build, both without
a build, a 13 KB image over a 12 KB-ring app, a carried SIM image, a carried DIAG image with a
newer build, and a DIAG installed (no request at all): not taken. A 13 KB image over a
RING_HIGH app was taken, and so was build 301 over an installed 2.1.10. With the ZTU answering
20-201 ms after each request, all 20 of 20 power-ups found the newer build at every latency (the
first prototype found it in 3 of 20 at ~97 ms: a request sent while an answer arrived destroyed
it).

### Probe asked by the app (the link-watchdog hook)

User decision U4: the frozen bootloader gives the app a way to ask for the power-on probe
without a power cut. Its user is the app's link watchdog (section 9, item 11), which restarts an
app that has heard nothing from the ZTU for 30 min. The policy (when to ask) is app code and
changes over the air; the frozen part is only this:

- **The bit.** Boot word bit 7, `BOOT_PROBE_REQ` (section 3). The app sets it only in a valid
  word (tag and complement), together with tries = 0, and resets with `NVIC_SystemReset()`.
  `boot_id.features` bit 3 (`BOOT_FEAT_LINK`) tells the app that the bootloader honours it.
- **Read and cleared at every boot.** On a warm boot with a valid word the bootloader copies bit
  7 and clears it before anything else uses the word, whatever happens next. A cold boot rebuilds
  the word, so a request never survives a power cut (the power-on probe runs then anyway), and a
  request in an invalid word is never seen.
- **Same rules as the power-on probe.** It runs only where the power-on probe would: a valid,
  not asked app, no rescue due, no bench build. Then it is the power-on probe: header requests
  every 100 ms for up to `PROBE_WINDOW_MS` (3 s), never while a frame is half received; it ends
  at the first header answer; it takes only an image with a larger build than the installed one
  that fits the installed layout, with "APDZ" and no bench flag; page 0 is checked before the
  first write; give-up 10 s after the last accepted block; the LED stays off.
- **A probe, not a rescue.** No 30 s window, no `restored` mark, the installed CRC is not
  skipped and no older image is taken; RESCUE and BUTTON stay clear. For the counter it is an
  ordinary warm boot (tries + 1 at the jump for a counted app); because the app wrote tries = 0
  with the request, the app then runs with tries 1, as after a power-on probe.
- **Visible.** Request byte 7 is `OTA_MODE_LINK` (6) instead of 3, also for the update it
  starts, so the ZTU can log a link probe as such. Bits 23..12 are cleared first and then hold
  the link probe's answer time; bit 6 (`BOOT_INFO_LINK`) is set and stays until the power-up.
  DP 137 then tells the two broken sides apart: LINK with a probe time means the ZTU answered
  0xA1 (its UART works: the silence was on the app's side, or above the ZTU's UART), LINK with 0
  means the ZTU did not answer at all.
- **Order.** Update (asked, or no valid app) > rescue (button, counter) > probe (cold, or asked)
  > jump, unchanged. The request never displaces an asked update or a counter rescue.
- **Size.** *Measured* on the prototype: +36 bytes, of which +24 for the hook and +12 for
  reading the tries through `BOOT_TRIES_MASK`, which bits 7 and 6 now require (3216 → 3252
  bytes).

*Measured* in the byte-timed model (a copy of the prototype with the hook, all earlier
scenarios still passing with 64- and 128-byte blocks, largest watchdog gap 51 ms). A unit that
ran, confirmed and then asked for a link probe:

| ZTU carries | Result |
|---|---|
| Same image, older build, 2.1.12 without a build, a SIM image, 13 KB over a 12 KB-ring app | Not taken; app after 0.05 s (answer after 9 ms); nothing written; tries 1, LINK set |
| Newer build (301 over 300) | Taken: reset after 2.52 s (1.84 s with 128-byte blocks), next boot jumps at once without a second probe; tries 1, WRITTEN and LINK set |
| 13 KB over a RING_HIGH app | Taken (2.86 s) |
| Anything, but the ZTU does not answer 0xA1 (silent, or a stock Tuya ZTU) | App after 3.12 s, probe time 0, LINK set, nothing written |
| Installed DIAG build | No request at all, bit 7 cleared, app after 0.04 s |

Also *measured*: a request together with an asked update gives the asked update (mode 1 only);
a request with tries 3 gives the counter rescue (no mode-6 request, `restored` marked); a cold
boot with a stale request in RAM probes with mode 3 and leaves LINK clear; a request in a word
with a wrong complement gives no probe; with LINK set, a crash loop still opens its first window
at the 4th unconfirmed boot (`..WWWW`); 10 link restarts in a row under a ZTU that never answers
0xA1, with foreign frames every 1 s and no confirmation, gave 0 windows, every boot ≤ 3.12 s,
nothing written, `restored` untouched.

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

**Window.** 30 s from the start, or from the last accepted block (then 10 s); user decision U3
keeps 30 s. With nothing new the bootloader starts the app again. `tries` stays at 3, so every
further warm reset opens another window until a run confirms, a run lasts long enough for its
link watchdog (4 h; its restart writes tries 0), or the power is cut. Under a stock Tuya ZTU a
window can install nothing, because that ZTU drops every 0xA1 (section 15). There the app's
stock-ZTU confirmation (section 7) keeps the counter from reaching 3.

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

**Rule.** A v2 app confirms once its uptime is ≥ `BOOT_CONFIRM_MS` = 30 000 and one of these
holds:
- it has answered at least one 0xA3 identity question in this run (an own ZTU);
- no 0xA3 came in this run, and at least 2 well-formed 0x01 product queries did (`55 AA 02 seq
  01 00 00 cs`: a stock Tuya ZTU, see "Stock-ZTU confirmation" below).

It then sets the tries (bits 3..0) of a valid boot word to 0, keeps every other bit (LINK, the
info bits, the probe time) and rewrites `boot_check`:

```c
static void boot_confirm(void) {                /* main loop, once per run */
    volatile ota_mailbox_t *mb = OTA_MAILBOX;
    uint32_t w = mb->boot;
    if (BOOT_WORD_VALID(w, mb->boot_check) && (w & BOOT_TRIES_MASK)) {
        w &= ~BOOT_TRIES_MASK;  mb->boot = w;  mb->boot_check = ~w;
    }
}
```

Why both conditions on the first path:
- The answered 0xA3 shows that UART RX/TX, framing and the identity handler work.
- The ZTU makes its offer right after that answer, so a confirmed app has already had its chance
  to take a newer image.
- The 30 s covers the start-up work: the first settings save (2 s after a change), the DP dump,
  the zero-cross lock and the first fades.
- A crash after 30 s needs no bootloader rescue: the app lives long enough to accept an offer.

**Stock-ZTU confirmation (second path).** A stock Tuya ZTU never asks 0xA3: every command byte
it writes at frame offset 6 is ≤ 0x3C (stock ZTU, 0xAFCA-0xD926). Without this path, under a
stock ZTU every warm reset from the third after a power-up would open a 30 s window and then
take the power-on behaviour. Such a window can install nothing, because the stock ZTU drops
every 0xA1 (section 15), so it is pure cost. The stock ZTU sends a 0x01 query every 1 s while it
gets no answer (stock ZTU: timer event 0x27, 0xBAFE-0xBB4C, calls the 0x01 sender 0xC44C and
re-arms after 1000 ms), and Open Dimmer never answers it. An own ZTU never sends 0x01: fork
`tuya_secondary_mcu.h:34-43` has no 0x01, and the only `queue_cmd` callers are `dp_attr.c:230`
(0x28), `tuya_mcu_ota.c:171` (0xA3), `:267` (0xA0), `tuya_mcu_app.c:119` (0x0B) and `:302`
(0x02). Contract item 7 keeps it that way. So under an own ZTU nothing changes, and a crash
before 30 s still counts as unconfirmed under either ZTU. The path is app code, but it must be
in release R's app: a unit under a stock ZTU never gets another app (no offers there), so the
app installed over SWD keeps its policy until the ZTU is converted.

**What the confirmation depends on in an own ZTU.** The ZTU asks for the identity after it
notices an MCU start: the app's first network query has seq 0 (`tuya_mcu_app.c:208-221`), or a
block request for offset 0, or an OK result. Then come the version question and 4 identity
questions in 7.5 s, and 2 more at later network queries if all were lost (`tuya_mcu_ota.h:4-14`,
`tuya_mcu_ota.c:162-196`). Contract item 4 (section 10) sorts MCUs into three classes. Three
points matter here:
- If the first query is lost, a ZTU that already knew the MCU as its own never asks in that run.
  The v2 app therefore numbers its network queries 0 until the ZTU has answered one (section 9).
  This is closed over the air.
- The ZTU asks only in builds with Open Dimmer support (`#ifdef SECONDARY_MCU_IMAGE`), and every
  such build carries an image: `tools/embed_ztu.py` has no empty mode, so
  `secondary_mcu_image_size != 0` (`tuya_mcu_ota.c:197`) always holds there. The first version
  of this design claimed a gap "without an image". No such build exists, and the question stays
  tied to Open Dimmer boards (section 10, change 2).
- An MCU of major ≥ 2 that has not said who it is is treated as restarted at most 3 times per
  ZTU run (`UNIDENTIFIED_RESTARTS_MAX`, `tuya_mcu_app.c:203-221`), because a stock MCU may
  number every query 0. An Open Dimmer app that stays unidentified through more than 3 of its
  own restarts in one ZTU run (every identity answer lost each time) is not asked again until
  the ZTU restarts (a power cut or a ZTU OTA). Its counter then opens windows, which is the
  intended reaction to a link that loses every identity answer.

**No fallback "confirm after N minutes without 0xA3".** A stable app is never pushed into rescue
anyway (the counter moves only at resets). Without the fallback, an app with a broken UART that
also resets now and then is still rescued.

**Link watchdog (user decision U4).** An app whose UART is broken and that never resets used to
need a power cut (the probe, if a newer build is carried) or the button. The first version of
this design left an app-side link watchdog out, because it would disturb a working light
whenever the ZTU, not the app, is the broken side: after three such resets the counter window
opens and the light takes the power-on behaviour. The adopted form (section 9, item 11, with the
frozen hook of section 5) removes both costs:
- it restarts only while every channel is off, after 30 min without any frame from the ZTU, in a
  run at least 4 h old, and the restore rule brings the state back (off);
- its request writes tries = 0, so link restarts never open a counter window (*measured*: 10
  link restarts under a ZTU that never answers, 0 windows);
- the restart asks for a probe, which takes a newer build if the ZTU carries one, without a
  power cut.

What remains when the ZTU is the broken side: ~3.1 s without wall-switch input at most once per
4 h while the light is off. The link restart is not a confirmation. Its tries = 0 changes one
thing for the counter: an app with a dead link is counter-rescued only by unconfirmed boots
less than 4 h apart (crashes); one that crashes more rarely gets the probe at each link restart
(newer builds) instead of the counter's older-image path (the button keeps that path).

**Cost when nothing confirms.** When no run confirms, every warm reset after the third
unconfirmed boot opens a 30 s dark window, until a confirmed run or a power cut (*measured*: 7
windows in 1 cold + 9 warm boots, `...WWWWWWW`), and the light takes the power-on behaviour after
it (section 3). Who is affected:
- an own ZTU without Open Dimmer support (a fork build from before it, forbidden by the STATUS
  migration order). It never sends 0x01 either, so the second path does not help there;
- an app whose identity answers are lost in every run.

A ZDMS16 ZTU build without an image does not exist (above). Under a stock Tuya ZTU the second
path confirms. Without it the same cost would apply there, with windows that can install
nothing. Warm resets are rare in a working app, so the cost is per watchdog or fault reset, not
per day.

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

Under the v1 bootloader the confirmation finds no valid word and does nothing, and the link
watchdog stays off (no `boot_id`, so no `BOOT_FEAT_LINK`).

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
| Request v2 (16 bytes) | offset (4, BE), max_len (2, BE), bootloader version (1), mode (1, `OTA_MODE_*`, 1..6; 6 = link probe), the installed header's bytes 8..15 as in flash (CRC LE, version byte, flags, build LE; meaningless in mode 2). A phase-1 ZTU reads bytes 0-5 only (`tuya_mcu_ota.c:273`); a 6-byte request means a v1 bootloader. The frozen requests thus tell a future ZTU which bootloader asks, why, and what is installed: it can report a crash loop, or serve a last-good image to a rescue, without any bootloader change. | +24 bytes |
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

"Over the air" means under an own ZTU. Under a stock Tuya ZTU the app installed over SWD is
final until the ZTU is converted (section 15), so everything a unit may need there must
already be in release R's app.

*Measured* for items 1-7 of the first version: +256 bytes on 2.1.12's 11 472. Items marked
"new" came with the review; estimated ~60 bytes together, plus ~20 for the stock-ZTU
confirmation and ~80 for the link watchdog (item 11; estimates).

*Implemented* (items 1-11): 12 120 bytes at first, +648 on 2.1.12 and ~200 above the
estimates (168 bytes free of 12 KB; DIAG / MEASURE / SIM 11 732 / 11 744 / 11 772). Before the
shared flash driver's range guard (item 7) it was 12 088. The largest parts: the confirmation
and link watchdog pass 176, the boot_id / sector CRC / word read 108, the input condition 76,
the shared planned restart 48, the dark check 44. R is the last app a unit under a stock ZTU
gets and the base of every later fix, so its headroom was recovered before the tag, with the
same behaviour: the clock and UART set up from constants (no `SystemCoreClockUpdate()` and its
tables, no run-time baud division: -300) and the app's data in common sections like the
bootloader's (-188). Release R: 11 632 bytes, 656 free; DIAG / MEASURE / SIM 11 264 / 11 268 /
11 320.

1. **Header.** `{ APP_MAGIC, 0, 0, FW_VERSION, APP_FLAG_CONFIRMS | (RING_HIGH ? APP_FLAG_RING_HIGH
   : 0) | bench flags, FW_BUILD }` (`src/main.c:27-30`). `FW_BUILD` sits in `src/tuya.h`; `make
   FW_TEST=n` adds 1..99 for spare test builds.
2. **Watchdog first.** `watchdog_start()` directly after the RCC flags are read and cleared
   (`main.c:188-192`), before `clock_init()`, with the bootloader's period. The HardFault/NMI
   reset handler is already there (`main.c:180-185`).
3. **Confirmation** (section 7). The 0xA3 handler sets `g_ident_answered`. A new `case` for
   cmd 0x01 in `handle_frame()` (`src/tuya.c:162-237` has none today) counts well-formed
   product queries and still sends no answer. The main loop calls `boot_confirm()` at 30 s of
   uptime once `g_ident_answered` is set, or once the count is ≥ 2 with no 0xA3 in this run
   (every 0xA3 is answered, so that is `g_ident_answered` still 0). It clears only the tries
   (`BOOT_TRIES_MASK`).
4. **Identity v2** (10 bytes, section 2).
5. **Offer rule** (section 2), with `app_limit()` from `_settings_start` and `boot_id`, and the
   lower bound aligned with the bootloader (`size >= APP_HEADER_OFFSET + sizeof(app_header_t)`,
   item 42). New: the 11-byte rule also needs `256 <= build <= 0xFFFD`, and SIM builds decline
   every offer as DIAG and MEASURE builds do.
6. **Bootloader identity.**
   - DP 136: CRC-32 of the whole 4 KB boot sector, computed once at start (~15 ms). The released
     v1 bootloader (2136 bytes, 0xFF-padded) gives **0xEAC45031**; each v2 build's value goes
     into STATUS.md with the release.
   - DP 137: the boot word at start, low 24 bits (probe time, rescue info, LINK, tries).
   - Both are sent with the health report (`src/health.c:34-41`, first at 60 s). Under an own
     ZTU they are, like DPs 130..135, only logged by the ZTU's MCU link diagnostics: no
     converter change. Under a stock Tuya ZTU all of 130..137 probably reach Zigbee2MQTT as
     unknown Tuya DPs. Its 0x05 handler (stock ZTU 0xD60C) does not check its product-info
     flag. This is log noise, not harm (section 15). The bootloader version also goes out in
     identity byte 9.
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
11. **New: link watchdog** (user decision U4; defaults `LINK_WDG_*` in `src/ota_proto.h`). In
    release R's app, the first v2 app, because a unit under a stock Tuya ZTU never gets another
    one.
    - **Armed** only with a valid `boot_id` whose features have `BOOT_FEAT_LINK`, a valid boot
      word at the moment it fires, and in normal builds: never in SIM, MEASURE or DIAG builds
      (the bootloader skips the probe for them, so the restart would buy nothing). Off under
      the v1 bootloader.
    - **Silence** = time since the last well-formed frame from the ZTU (55 AA 02, length and
      checksum good, any command), or since the start of the run. Bytes that form no frame
      (noise, a stock ZTU's 9600-baud phases) do not count. An own ZTU answers every 0x20, which
      the app sends every 30 s (contract item 8); a stock Tuya ZTU sends its 0x01 every 1 s
      while unanswered. So a working ZTU of either kind never lets it grow past ~30 s.
    - **Fires** when all of these hold at one main-loop pass:
      - silence ≥ `LINK_WDG_SILENCE_MS` = 30 min;
      - uptime of this run ≥ `LINK_WDG_UPTIME_MS` = 4 h (kept as a sticky flag, so the 32-bit
        ms counter's wrap after 49.7 days does not matter). This is the back-off: every restart
        begins a new run, so link restarts are at least 4 h apart. In a long run the first one
        comes 30 min after the last frame; after a power-up, whose own probe just ran, not
        before 4 h;
      - every channel is off and dark (no fade or hold running), and no wall-switch or
        module-button input is pressed: no input event waits, no input change is being
        debounced, and the button, momentary and state switches are open. A toggle switch (the
        default type) resting closed is not pressed: the restart takes it as where it rests,
        and its next change toggles the light as before (a state switch resting closed would
        light its channel at the restart);
      - and that has held for `LINK_WDG_QUIET_MS` = 1 min: every pass with a light, a fade or
        an input starts the minute again. Without it the restart could come ~0.8 s after a
        wall-switch off (once the fade ends), and a flip back during the ~3 s restart would be
        lost (conformance review, 2026-11-19).
    - **Action**, the planned-restart path of an accepted offer: `phase_shutdown()` (gates low
      first, although they are low already), `dimmer_prepare_restart()` (a due settings save,
      the light state into the mailbox, `restored` = 0), then
      `w = (boot & ~BOOT_TRIES_MASK) | BOOT_PROBE_REQ; boot = w; boot_check = ~w;` and
      `NVIC_SystemReset()`. Nothing else is written; the mailbox magic stays clear.
    - **Effect.** The bootloader runs the link probe (section 5). Nothing newer carried: the app
      is back after ~0.05 s when the ZTU answers 0xA1, after ~3.1 s when it does not. A newer
      build carried: it is installed (~2.5 s with 64-byte blocks) and starts instead. Either
      way the restore rule brings back the state (all off, levels kept), never the power-on
      behaviour. The wall switch and the button are not read meanwhile: ~3.1 s at most without
      an update (user decision U4), plus the install time when a newer build comes, the same as
      after a power cut.
    - **Seen** in DP 137 (LINK, probe time) from the next health report that gets through, and
      in the ZTU log (mode 6). The app adds no DP.
    - **Tests** (host, with the decision as a pure function of silence, uptime, light and input
      state, bench flags and the `boot_id` features): fires at exactly 30 min of silence with
      4 h of uptime and everything off; any well-formed frame restarts the silence, garbage
      bytes do not; never with a channel on, a fade or a hold running, an input pressed; never
      before 4 h, also across the counter wrap; never without `BOOT_FEAT_LINK` (v1), with an
      invalid word, or in SIM / MEASURE / DIAG; the word written is exactly `(w & ~0x0F) |
      0x80` with its complement, after `phase_shutdown()` and before the reset; after the
      restart `dimmer_init()` restores "off" with the power-on behaviour "on".

All of this works under the v1 bootloader (compatibility matrix). The changes ship in the same
release as v2, because the full image that `install.sh` writes must contain a counted app. The
release's app goes to the spare over the air first, under v1 (section 14).

---

## 10. ZTU-side changes (fork, phase-3 ZTU release; over Zigbee)

The phase-3 ZTU release must stay correct with the v1 bootloader on the spare and with stock
MCUs. Towards a stock MCU it must send exactly what phase 1 sends, apart from the dump retry of
change 6 (section 15, rule 3).

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
   including the first boot after a ZTU OTA. An earlier MCU UART start may serve this only
   under the stock rules of change 6 (a) and (b): the first-contact timer starts at the end of
   the full init and is ≥ 2000 ms, and before that the link only answers 0xA1.
4. **Identity, by class of MCU.** The major always comes from a 0x0B answer in this ZTU run,
   never from a guess.
   - (a) An MCU identified as Open Dimmer in this ZTU run (a "ZDMS" / kind 1 answer): the
     identity (0xA3) is asked again after every seq-0 network query, every block request for
     offset 0 and every OK result.
   - (b) Major ≥ 2, not identified: 4 questions after each version answer and 2 more at later
     network queries (`IDENT_RETRIES`, `IDENT_LATE_MAX`). At most `UNIDENTIFIED_RESTARTS_MAX`
     (3) seq-0 queries per ZTU run are treated as a restart (first contact again), because a
     stock MCU may number every query 0. Nothing beyond that.
   - (c) Major < 2, or no 0x0B answer yet: never asked, and no restart handling beyond phase
     1's (one first contact, repeated once at its first network query).

   `UNIDENTIFIED_RESTARTS_MAX` moves from `tuya_mcu_app.c:203` to `tuya_mcu_ota.h` next to the
   contract, so a contract test cannot drop it.
5. **Results.** 0xA2 status 0 = written and valid; anything else a failure that counts toward
   the pause; the pause doubles up to a day and ends with a ZTU restart (item 41, phase 1).

Items 6 to 8 protect factory firmware, the app's stock-ZTU rule and its link watchdog. The
frozen bootloader does not need them, but they are written down, pinned and checked the same
way:

6. **Factory MCUs.** To an MCU not identified as Open Dimmer the ZTU sends only the phase-1
   command set (section 15, rule 3; 0xA3 only within item 4 (b)), with one queued frame in
   flight. Each frame is ≤ 29 bytes. Frames less than 2 ms apart (a burst: a reply and the
   queued frame next to it, or two replies) add up to ≤ 23 bytes; if the gate ever finds a
   longer burst, the later frame waits 2 ms, which stays within rule 3's ±50 ms. Frames over
   29 bytes (the 38- and 86-byte 0xA1 answers) go only as the direct reply to an 0xA1 request
   and are never pushed.
   That also holds for later ideas such as serving a second image to a rescue, an image-info
   push or a longer offer. Reason: the stock parser limits in section 15 (a frame of 32..38
   bytes wedges a WiFi 1.0.3 MCU's RX until a power cut).
7. **No 0x01.** An own ZTU never sends 0x01 (product query) to an MCU: the v2 app takes 0x01
   queries without an 0xA3 as the sign of a stock Tuya ZTU (section 7). This binds only the app
   and the ZTU, which both update over the air under an own ZTU. Nothing frozen depends on it.
8. **Heard.** An MCU identified as Open Dimmer gets a well-formed frame at least every 30 min.
   In practice: every 0x20 network query is answered, as phase 1 does (fork
   `tuya_mcu_app.c:316-327`), and the app sends one every 30 s. The v2 app's link watchdog
   takes 30 min without any frame as a dead link (section 9, item 11); a ZTU release that
   stopped answering would cost every unit a 3 s dark-switch restart every 4 h while off. Like
   item 7, it binds only parts that update over the air.

### Changes

1. **`on_ident`** (`tuya_mcu_ota.c:235-268`):
   - Identity `len >= 9`: the app's build = bytes 7-8 BE; offer when the carried header's build
     (image bytes 0xCE-0xCF LE) is greater. A carried build 0 is never offered to it.
   - Identity of 7 bytes: today's byte rule.
   - Store byte 9 (bootloader version; 0 when the identity is shorter) in `tuya_mcu_diag` and
     log it with the version.
   - `IDENT_LEN 7` stays the minimum; protocol 1 is unchanged.
2. **Identity scope: unchanged** (withdrawn after review FA-4). The first version dropped
   `secondary_mcu_image_size != 0` from `tuya_mcu_ota_may_be_own()` (`tuya_mcu_ota.c:197`) so
   that a ZTU without an image would ask too. That changes nothing in any build that exists:
   - under `SECONDARY_MCU_IMAGE` the image is always non-empty (`tools/embed_ztu.py` has no
     empty mode);
   - every other board compiles the stubs, where `may_be_own()` returns 0 (fork
     `tuya_mcu_ota.c:344-369`).

   Moving the identity code out of the `#ifdef` to make the claim true would send 0xA3 to the
   factory MCU of every Tuya-MCU board in the fork that reports major ≥ 2 (touch switches and
   relays that do send seq-0 queries). At those queries it would also rerun
   `tuya_dp_apply_inits()`, the IT DP writes. So the question stays limited to boards with Open
   Dimmer support: today `#ifdef SECONDARY_MCU_IMAGE`, or a new `device_db` flag (`own_mcu:
   true` → `OWN_MCU`) if an image-less ZDMS16 build is ever wanted. A ZDMS16 build without the
   image is forbidden, like a ZTU without own-MCU support. New in phase 3: contract item 4's
   three classes, with `UNIDENTIFIED_RESTARTS_MAX` in `tuya_mcu_ota.h`.
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
   ("MCU in rescue"), so a crash loop shows in the ZTU log, and mode 6 as a link probe (the MCU
   heard nothing from this ZTU for 30 min). Reporting it to Z2M, or serving a
   second image to a rescue, is a later ZTU change that needs nothing from the bootloader.
6. **Boot timing diagnostic, dump retry, and stock rules for an earlier UART.**
   `hal_millis()` at the end of `tuya_mcu_app_init()` (UART up) and at the first answered 0xA1
   go into the link diagnostics. B2 and every ZTU release check them against the contract.
   - **Dump retry (phase 3, in every build).** If no DP report arrives within ~1 s of the
     first-contact 0x28, the ZTU sends 0x28 once more. It uses only commands the stock MCU
     already gets. Why: the ZDMS16-2's stock MCU never sends 0x20 (`tuya_mcu_app.c:147-148`),
     so the dump goes out once. Only 0x0B is retried (`tuya_mcu_app.c:110-134`). The stock MCU
     listens only after `HAL_Delay(1500)` (WiFi 1.0.3 main 0x08002A68), so the dump at app init
     + 2050..2150 ms has only ~550 ms plus the ZTU's own init time of margin. *Measured* in the
     scratch stub: an MCU that starts listening later than ~2150 ms after app init loses the
     dump. Ballast min then reads 1 (unknown) instead of 51, and the min / max / switch-type /
     power-on entities stay empty until the next ZTU restart. Level scaling falls back to
     1..254 → 10..1000 (`dimmer_cluster.c:42-64`), and the stock MCU does not clamp DP2 (the
     stock dimming RE notes, kept outside the repo with the dumps: "Remote DP write"), so low
     levels drive the lamp below its configured minimum.
   - **Earlier UART (only if the margin to the 1500 ms contract gets thin).** The ZTU may start
     its MCU UART before `hal_zigbee_init()`, over the air, under these rules:
     - (a) The first-contact timer still starts at the end of the full init (after
       `hal_zigbee_init()` and `parse_config()`, as today) and stays ≥ 2000 ms. The earlier
       UART start never moves it. Moving `tuya_mcu_app_init()` as a whole would move it, and
       so would any later cut to `FIRST_CONTACT_DELAY_MS`.
     - (b) Before that point the link is answer-only. An 0xA1 is answered; its `off == 0` side
       effect changes nothing then, because the contact state is still CONTACT_NONE. Every
       other frame stays buffered and goes to the normal dispatcher after init; when the
       buffer is full, whole frames are dropped, as in a lost burst. The ZTU starts no frame of
       its own. Stock-MCU frames do arrive in that time whenever the ZTU restarts alone (OTA,
       config write) while the MCU runs: a wall-switch DP report, the DP2 echo 1 s later, or a
       0x03 from the button. They must not reach the dimmer listeners or `hal_factory_reset()`
       / `hal_zigbee_start_network_steering()` (`tuya_mcu_app.c:288-314`) on an uninitialised
       stack.

     A plain "poll once between the init steps", as the first version proposed, breaks (b).
     (a) and (b) are contract item 3's condition.
7. **Stock MCUs: UART-identical to phase 1.** Towards an MCU that reports major < 2, or has not
   answered 0x0B yet, the phase-3 ZTU sends exactly what home-dimmer 51f5c95a sends (the
   modules' ZTU 1.1.3-194952ee is built from aa5f02e4, which differs from 51f5c95a only in the
   carried image). The only exception is the dump retry of change 6. Such an MCU is never
   asked for its identity (major 1, `tuya_mcu_ota.c:18, 197`) and never sends 0xA1. The
   stock-compatibility gate in item 9 pins this (section 15).
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
     result, for an identified MCU (class (a));
   - class (b): a 0x80 MCU that never answers 0xA3 and sends a seq-0 0x20 every 30 s for 10
     simulated minutes: ≤ 4 first contacts (0x0B and 0x28 each), ≤ 18 0xA3 (4 per version
     answer + 2 late; *measured* 17), no 0xA0, and after the 3rd treated restart only the 0x20
     replies. *Measured* in the scratch stub with the limit raised to 255, as a literal
     contract would allow: 21 version queries, 21 dumps and 83 0xA3 in 10 minutes, without end.
     Class (c): the same run with a 0x47 MCU sends 0 frames with cmd 0xA0..0xA3.
     `test_own_mcu_restarting_alone_is_noticed` stays;
   - the stock-compatibility gate, the frame-limit checks, the `ready_ms` sweep and the stub
     variant without `SECONDARY_MCU_IMAGE` of section 15 ("Tests that pin the rules");
   - stub timing: the first 0xA1 answer within the contract from the stub's boot;
   - contract item 8: every 0x20 from an identified MCU gets its reply, also during a burst of
     other traffic and during the failure pause; 16-byte requests with mode 6 are served like
     any other.
10. **py32-bump** (fork workflow): compare builds instead of version strings. Refuse a carried
    build ≤ the one `home-dimmer` carries unless `force` (force is the rollback path of section
    6). Print the OTA file size against the 212 992-byte limit (section 12).

---

## 11. Tools and CI

| Tool | Change |
|---|---|
| `tools/mkimage.py` | Read and print the build. Refuse: build 0 or 1..255, 0xFFFF for an image it creates; 0xFFFE unless SIM; SIM without 0xFFFE; an image CRC of 0; size > 12 KB without RING_HIGH; size > 14 KB; flags 0x20..0x80; a build ≥ 256 without CONFIRMS. Keep today's version-byte checks. `--full`: refuse a boot.bin without `boot_id` at 0x10 unless `--old-boot`; accept an existing 2.1.x app image (build 0, no CONFIRMS) only with `--legacy-app` (bench test B6). (That RING_HIGH is set exactly when `_settings_start` is 0x08004800 is a CI check on the ELF with `arm-none-eabi-nm`.) |
| `tools/embed_ztu.py` | All `assert`s become `sys.exit` checks (item 42), so `python3 -O` cannot skip them. Mirror the phase-3 `image_info` (section 10): flags & 0x07; SP == 0x20000BE0; reset vector odd and inside the image; size ≤ 14 KB and > 12 KB only with RING_HIGH. Build 256..0xFFFD with CONFIRMS; build 0 only with `--legacy` (version byte ≤ 0x9C, no CONFIRMS, no RING_HIGH: a 2.1.x rollback). |
| `tools/release/release.sh` | `FW_BUILD` == the previous tag's + 100 (`git show <tag>:src/tuya.h`), so every release is above every test build before it. Version byte > the previous tag's (tags are unique per byte). The first release over 12 KB is refused unless an earlier tag has RING_HIGH (the ring mover). The bootloader size and the boot sector CRC (DP 136 value) go in the release notes. (Implementation review) The app headroom and the ZTU OTA margin are printed together before the tag: below 512 free bytes (CI's warning line) only with `ACCEPT_HEADROOM=<those bytes>`, the user's decision for that release; the margin from `ZTU_OTA` (the ZDMS16-2 OTA file that will carry the release, by default the newest on `home-dimmer_build`), refused over 0x34000, a warning when a 14 KB app would not fit, `ZTU_OTA=skip` said so. |
| `tools/swd/install.sh` | Show the image's bootloader version (boot_id) and boot sector CRC. Refuse a full image without boot_id unless `OLD_BOOT=1`; one over 12 KB keeps the settings (sector 4 read, merged, written back) unless `ERASE_SETTINGS=1`. `IMAGE=release` checks `SHA256SUMS` (item 53). Optional `WRP=1` (section 1). Write 0 to the boot word pair (0x20000BF8, 0x20000BFC) before every reset it issues. Tools that halt the core must set DBGMCU `DBG_IWDG_STOP` first: v2 starts the watchdog at every boot (install.sh:124 and `make flash` already do). |
| `tools/ota_swd_emu.py` | Answer the header request (0xC0, 16) before the blocks, accept 16-byte requests, send the 11-byte 0xA0 (build) so that build-only test images are accepted, and clear the boot word pair before its resets. |
| `Makefile` | `FW_TEST=` (a new BUILD dir like `VERSION_BYTE`). `make flash` clears the boot word pair before its reset. `make test-boot`, part of `make test`. Report the bootloader size from the ELF; warn above 3584 bytes. |
| `tests/test_tuya.c` | The confirmation rule of section 7: (1) two 0x01 frames, 30 s, no 0xA3 → confirmed (boot word bits 3..0 = 0, every other bit kept, `boot_check` = ~boot); (2) no 0x01 and no 0xA3 → not confirmed; (3) 0xA3 answered → confirmed as before; (4) one 0x01 only → not confirmed. 0x01 is never answered. The link watchdog cases of section 9, item 11 (here or in a test of their own). |
| `tests/test_ota_proto.c` | Done with the header: the layouts and constants of `src/ota_proto.h` against this document (offsets the v1 bootloader reads, the boot word fields, the frame examples of section 5 byte for byte). |
| `tests/test_stock_ztu_dump.py` | New, skipped when the dump is absent (path in `STOCK_ZTU_DUMP`; the file is the one `docs/stock-firmware-analysis.md` names). Pins the stock ZTU facts of section 15 against the file: the 10 entries at 0x487E0; the 58-entry table at 0x48808 with handlers only for 0x01, 0x03, 0x05, 0x06, 0x08, 0x0B, 0x0D, 0x0E, 0x20, 0x24-0x27, 0x29, 0x2C, 0x36-0x38, 0x3A and 0xC8E4 for the rest; the `cmp r3,#57` encoding `ab39` at 0xCA0A; no branch to 0xDCD8 in 0xC8E4..0xC8F0. |
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
and counts header and offset-0 requests. A **stock mode** models the stock Tuya ZTU: it never
answers 0xA1 / 0xA2 and sends the 9-byte 0x01 query `55 AA 02 00 nn 01 00 00 cs` every
1000 ms from its ready time. An option alternates 1 s phases of 9600-baud-like garbage (random
bytes with framing errors, ~1 byte per 1.04 ms). That is the stock ZTU's UART-setting cycling
without an NV lock record (9600 / 115200 × 2 configurations, a step every 1 s, stock ZTU
0xBB10-0xBCC2).

**Reset model.** `RCC_CSR` per boot: cold PWR|PIN with random RAM; warm SFT/IWDG; stale PWR
after the bootloader's own reset. App model: confirms or not, crashes or not, clears the flags.
The module button: held from power-up, released at a given time or never. The IWDG counts at
the LSI's pace and resets the chip when it runs out; 0xCCCC starts it from 0xFFF and does not
reload a running one, only 0xAAAA reloads, a PR / RLR write takes effect after the update time.
A warm reset keeps it running with its counter, PR and RLR (the pessimistic reading of B8), and
after it ran out it counts again from RLR; a power-up stops it. The app model runs it at PR 3 /
RLR 1000 unless a scenario gives the app another period.

**Register trace.** The first GPIOA write is BRR of PA0/PA1; HSI_FS 4; USART BRR 208; IWDG
started, unlocked, PR 3 and RLR 1000, then reloaded once the update is in effect, within 2 ms
of the reset, on a warm boot without a mode too.

**Images.** Synthetic, from `tests/boot/mkimages.py`: a valid vector table, header and CRC,
chosen builds and flags, and controlled identical pages (for example page 0). No SDK needed.

**Scenarios** (each with a pass condition; the prototype passed all of them in the byte-timed
model):

| Group | Scenarios |
|---|---|
| Update | Asked normal (done; next boot tries 1, no probe); asked + ZTU silent (back to the app after 10 s); power cut after erase and after program at every page (all recover); image change mid-transfer (new image within ~15 s); ZTU answers only an identical page 0 (old app after 10 s); noisy link, 50 seeds (all succeed); 128-byte blocks; unsolicited ZTU frames every 1000/300/100/50 ms; FLASH result path (a page that never verifies: the ZTU sees 3 FLASH results and pauses; 362 requests in 60 s, no tight loop); carried image with a bad SP: refused at page 0, nothing written |
| Probe | Newer build taken, no second probe after its reset; same image, older build, old image without build, both without build: not taken; installed 2.1.x without build: taken; ZTU silent: app at the window; ZTU ready at 1.4 s and 2.9 s: taken; after the window: not; 13 KB over a 12 KB-ring app refused; over a RING_HIGH app taken; DIAG / MEASURE / SIM installed: no request at all; carried SIM and DIAG refused; wrong magic refused; answer latency sweep 20-201 ms: always found; the B3 case (the probe does not depend on the app's offer path): newer build taken after a power cut |
| Rescue | 3 unconfirmed boots then windows; same CRC skipped, 0 offset-0 requests; older image taken; window length 30 s; `restored` marked; 13 KB on a 12 KB-ring app refused; bench images refused (counter, button, no app); power cut during a rescue transfer; duty cycle of an app that hangs in init; `wdg-carry`: the crash loop under an IWDG kept over every reset at the crashing app's ~4.9 ms (its watchdog runs out, or it faults 3.9 ms after a reload): window at boot 4, the bootloader's period reloaded < 2 ms after each reset |
| Button | Held and released at a cold boot: rescue, older taken; same image: app after ~30 s; released after 4.5 s: rescue; never released: app after 5.1 s, STUCK, nothing written; held at a warm boot: ignored; not held: no delay |
| Counter | 2.1.x app (no CONFIRMS) with 8+ watchdog resets: never a window; bench build with CONFIRMS: never counted; confirming app with resets between confirmations: never a window; never confirmed: every warm reset after the third opens a window; asked update resets tries; written image resets tries; stale PWRRSTF + IWDG counts as warm |
| Link | (section 5, "Probe asked by the app") Request on a warm boot: the carried cases of the power-on probe, each with mode 6 only, bit 7 cleared in the stored word, LINK set, the probe time replaced, restore words untouched; newer build taken, no second probe after its reset, tries 1; ZTU not answering 0xA1: app at ~3.1 s, nothing written; DIAG / MEASURE / SIM installed: no request, bit 7 cleared; request + asked update: asked; request + tries 3: counter rescue; cold boot with a stale request: mode 3, LINK clear; request in an invalid word: no probe; LINK set + crash loop: first window at the 4th unconfirmed boot; 10 link restarts (the app's tries = 0 policy) under a ZTU that never answers 0xA1, with foreign frames: 0 windows, each ≤ 3.2 s |
| Mailbox | Random RAM at power-up, 200 boots: never asked, word rebuilt, restore words untouched (byte compare); `restored` written only by a counter rescue; mode byte of every request (1..6); tries always read through `BOOT_TRIES_MASK` |
| Stock ZTU | `stockztu-cold`: ZTU ready at 300 ms, 20 power-ups: app at 3.05-3.15 s, 0 pages written, 28-31 header requests with mode byte 3, probe time 0 in the boot word, no request started while `g_rx_len != 0`, largest watchdog gap < 250 ms. `stockztu-button` (released at 0.5 s): window 30 s, app at ~30.6 s, nothing written. `stockztu-counter`: the app model confirms by the stock-ZTU rule, each run ≥ 30 s, 1 cold + 10 warm resets: 0 windows, `restored` untouched; the same run with the old rule gives 8 windows (documents the cost). `stockztu-noapp` (1 h model time): a request every 500 ms, no RX buffer growth, watchdog gaps < 250 ms, no request sent mid-frame. `stockztu-link`: a link request: app at ~3.1 s, mode 6 requests only, nothing written, LINK set with probe time 0. The five again with the 9600-baud garbage phases on |
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
| **Prototype total before the user decisions** | **3216 of 4096 (880 free)** |
| Link-watchdog hook (user decision U4): bit 7 read and cleared, the probe on a warm boot, probe time reset, LINK bit, mode 6 | +24 |
| Tries read and written through `BOOT_TRIES_MASK` (needed since bits 7 and 6 carry other things) | +12 |
| **Prototype total** | **3252 of 4096 (844 free)** |

The three user-requested paths (counter with watchdog, button, probe) took 464 bytes in the
first prototype, above the ~300-byte estimate. The review's fixes add 436 bytes more, mostly
for the shared update path (header first, page-0 check, half-duplex, watchdog period) rather
than for the three paths, and the link hook 36. The slot is the bootloader's alone, so the
extra costs nothing else. The implementation re-measures; the half-duplex guard's 52 bytes for
one condition look like a code-layout effect. If it must shrink, the order is: info bits (-12),
LED blink pattern (-20), boot info (-44). This design keeps all of them, and the link hook is
not on the list (it is a user decision).

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

The implemented release-R app (section 9) is 11 632 bytes (656 free of 12 KB; 12 120 before the
headroom was recovered). *Measured* with it carried, the phase-3 ZTU's ZDMS16-2 OTA file is
211 426 bytes: 1 566 bytes below 0x34000 (211 938 and 1 054 with the 12 120-byte build). So
release M itself (at most 12 KB) fits, but a 14 KB app after it needs 2 704 bytes more, 1 138
more than the ZTU build has: freeing room in the ZTU build stays the precondition of the ring
move. `tools/release/release.sh` prints the app headroom and this margin together before every
tag (`release_check.py`, section 11). The implemented
bootloader is 3 364 bytes (732 free), 112 more than the prototype (the watchdog reload after
the update wait, from the implementation review, is 8 of them).

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
| 18 | ZTU that never asks 0xA3 above an Open Dimmer MCU | Never updated | Own ZTU without Open Dimmer support: every warm reset after the third gives a 30 s window, then the power-on behaviour. Stock Tuya ZTU: confirmed by the 0x01 rule (section 7), no windows | An own ZTU without support is forbidden (STATUS). Every ZDMS16 ZTU build carries an image, so no image-less case exists (section 10, change 2). Stock Tuya ZTU: not supported, no updates until the ZTU is converted (section 15). Without the 0x01 rule there: windows from the 3rd warm reset, each installing nothing |
| 19 | App whose UART is dead and which never resets | SWD | Link watchdog (user decision U4): after 30 min without a ZTU frame, with every channel off and 4 h of uptime, a restart with a probe that takes a newer build; also a power cut or the button | Only newer builds without the button; it waits while a channel stays on; under the v1 bootloader none (spare only) |
| 20 | Defect in v2 itself | - | Harness gate (section 11), spare trial (section 14) | Frozen: SWD, or a boot-updater app (~150 ms brick window) while WRP is off |
| 21 | Brown-outs in a row | Each boots the app | Each is a cold boot with one probe (an own ZTU usually answers at once if it did not reset; a stock Tuya ZTU never answers: 3.1 s per dip) | ≤ window delay per dip |
| 22 | Flash page that never verifies, or a controller that stays busy | FLASH results, ZTU pause (busy: watchdog loop) | FLASH results, ZTU pause; the busy wait is bounded | Hardware: SWD cannot fix it either |
| 23 | Wrong assumption about the reset flags (B8) | - | Worst case: a cold boot read as warm skips one probe and keeps counting tries | Verified on the spare before the modules |
| 24 | Open Dimmer under a stock Tuya ZTU (installed before the ZTU was converted, or the ZTU put back to stock) | 0x01 unanswered; no updates | Same; 3.1 s dark per power-up; no window and nothing written | Not supported but safe: convert the ZTU over the air without re-pairing or resetting first (section 15) |
| 25 | A ZTU change sends a stock MCU something new (an extra 0xA3 or dump, a long frame) | - | Rules and gate of section 15; contract items 4 and 6; rollout step 3 diag check | A frame of 32..38 bytes would wedge a stock MCU's RX until a power cut (WiFi 1.0.3 parser; Zigbee 1.0.7 inferred) |
| 26 | ZTU link dead while the app is fine (the ZTU is the broken side) | Nothing happens | A link restart at most every 4 h, only while every channel is off; the state comes back; LINK in DP 137 (probe time 0 if the ZTU does not answer 0xA1) | ~3.1 s without wall-switch input per restart while dark. A ZTU release that stops answering 0x20 would cause it on every unit: contract item 8 |
| 27 | Bug in the app's link watchdog (fires too often or too early) | - | Every restart runs a probe (a newer, fixed build is taken at once); the restore keeps the light state | The app's tries = 0 with each request also defeats the counter for such a bug: an older image then needs the button. Mitigation: the decision is a pure function with host tests (section 9, item 11), and the spare runs every release first |

---

## 14. Rollout

1. **Implementation** on a branch from this design:
   - `boot/boot.c`, `boot/boot.ld`, `src/ota_proto.h` (with the ZTU contract), `src/flash_drv.c`;
   - the app changes of section 9;
   - `tests/boot/` and `make test-boot`;
   - the tools of section 11; STATUS.md; an INSTALL.md section on the button rescue, with the
     wording of user decision U2 (the button is pressed through the insulated case, which is
     never opened while the mains is connected);
   - in the fork, the phase-3 ZTU with the tests of section 15;
   - docs that this design step does not edit:
     - `docs/stock-firmware-analysis.md`: "Stock ZTU and unknown commands" and "Stock MCU
       parser limits" (section 15, with addresses; the Zigbee 1.0.7 limits marked inferred);
     - `docs/INSTALL.md`, next to its lines 19-21: Open Dimmer under a stock ZTU is not
       supported but safe; if it happens, do not re-pair or reset the module, convert the ZTU
       over the air first; ZTU first stays a hard rule;
     - `docs/STATUS.md` migration step 2: the stock Tuya ZTU is named next to the old fork
       builds, and the per-module diag check of step 3 below is added.

   CI green, then a review of the bootloader diff against this document.
2. **Release R** vX.Y.Z (build 300) with v2 and the counted app. Record the boot sector CRC.
3. **ZTU phase-3 release** (section 10), carrying R's app. OTA to the spare's ZTU first. The
   spare still has v1 + 2.1.12: the ZTU offers R's app by the byte rule (7-byte identity), and
   the spare takes it under v1. That runs the matrix rows "2.1.x | v1 | phase 3" and "v2 app |
   v1 | phase 3". Then the other modules' ZTUs one at a time, with their stock MCUs (section
   15, matrix row 3). Per module:
   - Before its OTA: `mcu_version` reads 1.0.7 (the version byte is the only stock
     discriminator), and the diag attributes are decoded and saved (counters 0xff40, log
     0xff41..0xff45, fork `helper_scripts/decode_mcu_diag.py`).
   - After its OTA: the phase-1 user-level check (STATUS migration step 2: `mcu_firmware`
     "Tuya (stock)", `mcu_version`, exposes, lights, wall switch, OnLevel). Then a UART-level
     check from the same diag decode. TX_CMD ids are only 0x0B and 0x28 plus the expected DP
     writes, with no 0xA0..0xA3. RX_DP for DPs 3, 4, 5 and 14 appears after first contact. The
     min / max brightness entities match the values from before the OTA. `rx_bad` and
     `tx_timeout` do not grow over a day.
   - Then one ZTU restart with the light on (a config re-write): light state and level do not
     change.

   Checklist row per module, saved with the rollout notes. Pass: no 0xA0..0xA3 entries, the
   dump DPs present, entities unchanged. A lost dump does not show in the lights while they stay
   on, but it does show in the min / max entities and at low levels, so the user-level check
   alone is not enough.
4. **Spare** (mains off, probe on CN2): `IMAGE=<R's zdms16_py32_full.bin> tools/swd/install.sh`.
   This also exercises install.sh's re-flash path (item 8 / STATUS hardware gate). The settings
   in sector 4 stay untouched.
5. **Bench tests on the spare** (mains rules apply: with the mains connected the case stays
   closed and the button is pressed through it; SWD steps with the mains off):

| # | Test | Pass |
|---|---|---|
| B1 | After the install: read-back equal; DP 136 = the release's CRC; ZTU log shows bootloader version 2 and requests of 16 bytes | as stated |
| B2 | 20+ mains power cycles, nothing newer carried: light delay (timed), DP 137 probe time each run, the ZTU's boot timing diagnostic; then 3 more right after a ZTU OTA (its first boot) | Every probe time ≤ 1500 ms (the contract), delay = probe time + ~50 ms, no update. A probe time of 0 while the ZTU serves means > 3 s: rebuild once with a 4000 ms measurement window to see the real time. If the contract is missed, the ZTU is fixed (section 10, item 6) before any further module; the frozen window stays. |
| B3 | Broken update path: a test build in the range below R (e.g. 299), normal flags with CONFIRMS, its offer handler patched to decline (SWD; `make OFFER_DECLINE_TEST=1 FW_PREV=1 FW_TEST=n`); the ZTU carries R | The offer is declined (ZTU log); after a power cut the probe installs R (DIAG and MEASURE builds cannot be used here: the probe skips bench builds) |
| B4 | Crash loop: a test build R+1 that faults 1 s after start (SWD; `make CRASH_TEST=1 FW_TEST=n`), ZTU carries R | 3 crashes, then a window installs R; DP 137 shows RESCUE and WRITTEN; the light uses the power-on behaviour |
| B5 | Button held through power-up and released when the LED lights; carried image different / same; then held for > 5 s | Different: installed, DP 137 BUTTON. Same: app after ~30 s. Held: app after ~5 s, STUCK, nothing written. Never a rejoin or factory reset. |
| B6 | 2.1.12 app under v2 (full image from the v2.1.12 app with `--legacy-app` and v2 boot), watchdog resets via a halt without `DBG_IWDG_STOP` | No window; a power cut lets the probe take the carried build. (2026-11-19: not run: with a carried image over 12 KB the last step cannot pass by design — no image over 12 KB goes over a 12 KB-layout app; 2.1.x reaches 2.2.x through 2.1.14) |
| B7 | Mains cut at ~50 % of a probe-started update (fast LED blink), 3 times | Each recovers on the next power-up |
| B8 | Reset flags: DP 131 after a power cycle that follows a software reset; flags unchanged by the bootloader's LSION write; IWDG running or not after `NVIC_SystemReset` (SWD) | PWR only (cold); results recorded in STATUS |
| B9 | Normal offered OTA with both channels lit | Light restored; dark time measured |
| B10 | 1-2 weeks trial: health DPs 131-137 | No unexpected resets; DP 137 shows tries 1 at every start (each run before it confirmed) and LINK clear (no link restart under a working ZTU) |
| B11 | Link watchdog: a spare test build (below R) with the thresholds cut to 2 min silence / 5 min uptime and its frame counter fed nothing (an SWD build switch: frames are still handled, only the watchdog does not see them); the ZTU carries R, then the same build. With a channel on, then with both off | Never fires while a channel is on. With both off: a restart, the light state unchanged, mode-6 requests in the ZTU log; R installed by the probe; with nothing newer, the switch answers again within ~0.2 s; DP 137 shows LINK and the probe time; never a rescue window over 10 cycles |

6. **Further modules**, each at its migration slot (STATUS migration order): install.sh stock
   path with the same release full image, then record DP 136 and 137 per module. From then on
   their bootloader never changes. App and ZTU updates go over Zigbee, the spare first, as the
   process rule says. After every ZTU rollout, a cold boot with probe time 0 (and LINK clear) in
   DP 137 on any module under an own ZTU of phase 3 or later is a contract miss. A stock Tuya
   ZTU never answers, so the rule does not apply there.
7. **Later, only when the app approaches 12 KB:** the ring-move release M (section 1), its
   preconditions, spread and verified everywhere, before any larger app.

**Open measurement before freezing v2:** the ZTU's answer time after a power-up (B2), checked
against the 1500 ms contract. It no longer sets the window. Within the contract the light delay
after a power cut (answer time + ~50 ms) stays inside the 1-2 s the user accepted. That holds
for own ZTUs. Under a stock Tuya ZTU, which is not supported, it is 3.1 s (section 15).

---

## 15. Factory firmware compatibility

Requirement (user, 2026-11-18): compatibility with the factory firmware must be guaranteed. The
other modules run the own ZTU (phase 1, 1.1.3-194952ee) above the stock PY32 firmware 1.0.7, and
they must keep working through every ZTU change of this phase. Two reviews checked both
directions against the code and the stock material (section 16, lines FA and FB):
- **Direction A: stock PY32 behind an own ZTU.** Evidence: the fork's code paths, and the stub
  build of 51f5c95a run against a stock-like MCU model. The model answers 0x0B with 0x47 and
  0x28 with a DP dump, echoes writes and ignores everything else. 1.0.7 is read-protected
  (stock-firmware-analysis.md, "SWD read of the Zigbee PY32"), so its reaction to unknown
  commands and its parser limits come from a disassembly of the WiFi sibling 1.0.3.
- **Direction B: Open Dimmer behind the stock Tuya ZTU.** Evidence: static analysis of the
  public stock ZTU dump. Nothing ran on hardware, and it is one dump of unknown version.

### Who is identified, and how

| Party | Identified by |
|---|---|
| Open Dimmer MCU | Its answer to the ZTU's own 0xA3 in this ZTU run: "ZDMS", kind 1 (`on_ident`, fork `tuya_mcu_ota.c:236-241`). Never from a version byte, a DP or a guess |
| Stock MCU | Major < 2 in a 0x0B answer in this ZTU run (1.0.7 = 0x47; the version byte is the only stock discriminator). An MCU that has not answered 0x0B yet is treated the same way |
| Possibly own MCU | Major ≥ 2, no identity answer yet: asked within contract item 4 (b), never offered anything |
| Own ZTU (for the app) | Any 0xA3 question in this run |
| Stock Tuya ZTU (for the app) | No 0xA3, and at least 2 well-formed 0x01 queries in this run (section 7). Used only for the confirmation |

The bootloader identifies nobody. It asks for the carried header and takes only an image that
suits its mode (section 8), and it needs nothing from a stock ZTU.

### Rules

1. **Own commands only to an identified Open Dimmer MCU.** 0xA0 (offer) goes out only from
   `on_ident`, after an identity answer in this run (fork `tuya_mcu_ota.c:236-241, 267`). 0xA3
   goes only to an MCU of major ≥ 2, within contract item 4. The 0xA1 answers (38 / 86 bytes) go
   only in reply to an 0xA1 request (`:270-300`). `image_info`, the build rules and the v2
   request parsing are reached only through `on_ident` or `answer_block`.
2. **Never an offer to a stock MCU.** A stock MCU cannot produce the identity answer, so no
   ZTU release reaches an offer for it (rule 1).
3. **Towards a stock MCU the phase-3 ZTU is UART-identical to phase 1** (section 10, change 7).
   That means the same commands, data, order and timing (±50 ms):
   - 0x0B at app init + 2000 ms (2 retries) and 0x28 at first contact (`tuya_mcu_app.c:156,
     162-183`);
   - 0x04 DP writes;
   - replies to 0x20 / 0x24 / 0x25;
   - 0x02 only on boards whose MCU keys are the UI (never on the ZDMS16-2);
   - no 0x00 (heartbeat), 0x01 (product info), working mode, 0xA0 or 0xA3.

   The one deliberate difference is the dump retry (section 10, change 6). It is sent only when
   the first dump got no DP report.
4. **Frame limits towards an MCU not identified as Open Dimmer** (contract item 6): ≤ 29
   bytes per frame (the largest frame the phase-1 queue can build: 8 + 4 + `value[16]` + 1,
   fork `tuya_secondary_mcu.c:173`), one queued frame in flight, and ≤ 23 bytes for frames
   less than 2 ms apart (the stock ISR ring, below). Frames over 29 bytes go only as the direct
   reply to an 0xA1 request. Phase 3's own frames are 9 bytes (0xA3) and 20 bytes (0xA0
   v2). `send_reply` bypasses the one-in-flight queue
   (`tuya_secondary_mcu.c:108-114`), so the burst rule counts replies too.
5. **First contact only after the full init.** The first-contact timer starts at the end of
   the full ZTU init and stays ≥ 2000 ms. Before that point only 0xA1 is answered; other frames
   are dispatched after init. A dump without any DP report is retried once (section 10, change
   6).
6. **The identity question exists only in builds with Open Dimmer support**
   (`#ifdef SECONDARY_MCU_IMAGE`, and every such build carries an image). Every other board in
   the fork compiles the stubs, where `tuya_mcu_ota_may_be_own()` returns 0, so touch switches
   and relays with factory MCUs never see 0xA3 or a treated restart (section 10, change 2).
7. **Converter unchanged for stock modules.** `mcu_version` keeps "x.y.z" / "x.y.z own"
   (section 10, change 8). There are no new attributes, and DPs 136 / 137 stay out of
   `device_db`, so `mcu_firmware` stays "Tuya (stock)" and the exposes stay as they are.
8. **Open Dimmer under a stock ZTU: bounded costs, nothing written.** The v2 app confirms by
   the stock-ZTU rule (section 7). The bootloader's probe and windows cost time and install
   nothing. The link watchdog stays quiet while the stock ZTU sends anything (its 0x01 every
   1 s) and costs at most one 3.1 s probe per 4 h, while dark, if it goes silent. The
   combination is not supported (below).
9. **The link watchdog adds nothing towards a stock MCU.** It lives in the Open Dimmer app.
   The ZTU serves its mode-6 requests by contract item 1 like every 0xA1, and only to the MCU
   that sent them. Its contract item 8 (answer 0x20) is phase 1's behaviour.

### What each side sees, and why it is harmless

**A stock PY32 1.0.7 under the phase-3 ZTU** sees what it sees under phase 1 (rule 3): at most
one 0x28 more, after a lost dump. It answers 0x0B in 3 ms with 0x47 and ignores 0x0C
(stock-firmware-analysis.md, 0x0B and 0x0C probes). The WiFi 1.0.3 dispatcher ignores unknown
commands (0x080026CA). *Measured* in the scratch stub over 5 simulated minutes, with seven seq-0
0x20 queries, an OK 0xA2 and two 0xA1 requests injected: the ZTU sent only 0x0B, 0x28 and the
two 0xA1 replies, never 0xA0 or 0xA3. Every frame was 9-10 bytes except the 0xA1 replies, which
came only on request. The ZDMS16-2's stock MCU sends no 0x20 at all (`tuya_mcu_app.c:147-148`),
so on the real modules even less happens.

**The stock Tuya ZTU under Open Dimmer** sees:
- from the v2 bootloader: 16-byte 0xA1 header requests, 10 per second for 3 s after every
  power-up (the probe), 2 per second in a button or counter window (30 s), and 2 per second
  for good with no valid app. The same 10 per second for 3 s after a link restart (mode 6),
  which needs 30 min without any valid frame from it, so normally never. Never an 0xA2:
  nothing is written without an answer;
- from the app: 0x20 network queries, DP reports (0x05 / 0x06) with the health DPs 130..137
  every 2 min, and 0x03 after a 3 s module-button hold (`controls.c:167`). It gets no answer to
  its 0x01.

What it does with them (stock ZTU dump):
- Its byte parser 0xC858 (state table 0x487E0) needs 55 AA 02, len ≤ 246 (0xC9A8) and a good
  sum (0xC8B4-0xC8E2). It keeps a 100 ms inter-byte timer 0x2E (0xC942-0xC948).
- Then it checks len ≤ 255 (0xC9EE) and (cmd − 1) ≤ 0x39 (0xCA02-0xCA0E); a frame that fails
  goes to 0xC8E4. The command table 0x48808 sends 39 of its 58 entries there too. 0xA1 and
  0xA2 fail the range check.
- 0xC8E4 stops timer 0x2E and returns to the parser. There is no call to the TX queue 0xDCD8,
  no pending-ack entry (0xB898) and no NV write. So probe, rescue and no-app requests are
  dropped silently, at any rate and for days, and cannot stall, reset, leave or factory-reset
  it.
- Its bootloader region 0x0-0x6A0C contains no Telink UART register literal (the app region has
  them from 0x22878 on), so no request reaches a serial loader.
- The health DPs probably reach Zigbee2MQTT as unknown raw Tuya DPs: its 0x05 handler (0xD60C)
  is not gated by its product-info flag (0x842428 is read only at 0xC45A, 0xC750, 0xCE24,
  0xD956 and 0xD9FE, none of them reached from 0xD60C, and written at 0xDBB0 in the 0x01
  handler). That is log noise, not harm.

The assumption for other stock releases: a Tuya MCU-SDK module ignores commands it does not
know, because the Zigbee serial protocol has no NAK. The design relies on nothing more. The
bootloader needs nothing from a stock ZTU, and its traffic is bounded at 10 frames per second
for 3 s, then at most 2 per second.

### Stock MCU parser limits

From the WiFi 1.0.3 disassembly. The Zigbee 1.0.7 build is read-protected and probably shares
the vendor template, so these limits are inferred for it:
- The USART ISR ring holds 23 bytes (0x200001D3..0x200001EA) and drops bytes when it is full
  (0x080038F8-0x0800393E).
- The linear parse buffer holds 31 bytes (0x0800406E-0x08004072), but frame lengths up to 38
  bytes are accepted (0x080040C8). A frame of 32..38 bytes therefore never completes: no byte
  is read while the count is 31 (0x080040D8 → 0x08004112), and RX stays wedged until a power
  cut. The unit is then controllable only from the wall switch.
- Back-to-back bursts over 23 bytes lose bytes while the MCU blocks its main loop (the 600 ms
  LED blink, a flash save).
- `main` runs `HAL_Delay(1500)` before its init (0x08002A68). The Zigbee unit's SWD window of
  ~1.5 s after power-up fits the same start.

Phase 3 stays inside these limits (rule 4). Contract item 6 keeps later ZTU changes inside them.

### Combination matrix

| # | MCU | Bootloader | ZTU | Status | What happens |
|---|---|---|---|---|---|
| 1 | stock 1.0.7 | (stock, none) | stock Tuya | factory | Nothing of this design runs |
| 2 | stock 1.0.7 | (stock) | own, phase 1 | supported: the modules today | Baseline: 0x0B, 0x28, DP writes. Never asked 0xA3 |
| 3 | stock 1.0.7 | (stock) | own, phase 3 | supported | UART-identical to row 2 (rule 3), plus at most one dump retry. Gate test, rollout step 3 check |
| 4 | stock, reporting major ≥ 2 (another batch, or the `_TZE204_jtbgusdc` variant whose stock version nobody has read) | (stock) | own, phase 1 / 3 | supported, bounded | 4 + 2 identity questions per version answer, ≤ 3 treated restarts per ZTU run, never an offer (contract item 4 (b)) |
| 5 | factory MCU of another fork board (touch switch, relay) | (stock) | own, any | unchanged | No identity code in the build (rule 6) |
| 6 | Open Dimmer 2.1.x / v2 app | v1 / v2 | own, phase 1 / 3 | supported: the design target | Section 2 matrix |
| 7 | Open Dimmer, any | v1 / v2 | stock Tuya | NOT SUPPORTED, but safe | Next subsection |
| 8 | Open Dimmer, any | v1 / v2 | own, from before Open Dimmer support | forbidden (STATUS migration order) | Never asked 0xA3, no updates. Counter windows from the 3rd warm reset (section 7) |
| 9 | Open Dimmer, any | any | own ZDMS16 build without an image | does not exist | `embed_ztu.py` has no empty mode. Forbidden like row 8 |

### Open Dimmer under a stock Tuya ZTU (row 7)

How it can happen: a module installed over SWD before its ZTU was converted (INSTALL section 0
forbids it, but nothing enforces it), or a ZTU put back to stock.

**Not supported:**
- Zigbee control is not guaranteed. The stock ZTU needs an answer to 0x01 to set its name
  "_TZE284_" + PID (handler 0xDA5A, writes at 0xDB32-0xDB6A), to lock its MCU UART setting in
  NV (0xC7F4, called at 0xDBAA) and to stop its 1 s query. Open Dimmer does not answer 0x01
  (`src/tuya.c:162-237` has no case for it; INSTALL.md lines 19-21).
- There is no MCU update path. The stock ZTU never asks 0xA3, never offers and drops every
  0xA1. The app installed over SWD is final until the ZTU is converted, and no v2 escape
  (probe, counter, button) can install anything.
- Without a UART lock record in its NV, the stock ZTU cycles its MCU UART setting (9600 /
  115200 × 2 configurations, a step every 1 s, 0xBB10-0xBCC2), so DP traffic works at best a
  quarter of the time. The dump holds the record (0xEA20A: `aa 00 01 c2 00 01 aa`, 115200,
  configuration 1) and the full name (0xEC641: "_TZE284_jtbgusdc"). Whether a reset unit keeps
  them is unverified: no stock-ZTU unit is available.

**Guaranteed:**
- The stock ZTU is not disturbed (the dump evidence above).
- The light works locally (wall switch, module button) once the app runs.
- The bootloader writes nothing, because no image ever arrives.
- The costs are bounded and listed. Each one is pinned by a `make test-boot` scenario of the
  "Stock ZTU" group (section 11):
  - 3.1 s dark after every power-up or brown-out (the probe's full window; v1 jumps at once,
    `boot/boot.c:343`). Wall-switch presses in that time are lost;
  - a button rescue always ends after 30 s (app at ~30.6 s);
  - no counter windows, thanks to the stock-ZTU confirmation (section 7). Without it, every
    warm reset from the third after a power-up until the next power cut would give 30 s dark
    and then the power-on behaviour: with "off" a lamp that was on stays off, with "on" it
    switches on by itself, at night too;
  - a link restart only if the stock ZTU sends no valid frame for 30 min (it sends its 0x01
    every 1 s while unanswered, and a quarter of the time even while it cycles its UART
    setting): at most one per 4 h, only with every channel off, 3.1 s without wall-switch
    input, the state restored, nothing written (scenario `stockztu-link`);
  - the health DPs show in Zigbee2MQTT as unknown DPs.

**Recovery:** convert the ZTU over the air first (INSTALL section 0, `tuya_with_ota.js`),
without re-pairing or resetting the module. Then everything in this design applies. The
conversion needs the stock ZTU joined and identified as `_TZE284_jtbgusdc`. The stock ZTU sets
its default name "TS0601" / "_TZE284_" at 0xB6BC and appends the PID only in its 0x01 handler.
A reset or re-pair, including the app's 3 s module-button hold, may therefore leave it as
"_TZE284_" only, if it loses its NV name record. Zigbee2MQTT then cannot match it, no
conversion OTA is offered, and only a wired ZTU flash remains. The stock MCU firmware is gone
for good (RDP).

**Considered and not adopted:**
- An early probe exit on the stock ZTU's 0x01 frame in the frozen bootloader (~10-16 bytes,
  light after ~1 s instead of 3.1 s). It would freeze the assumption "no own ZTU sends 0x01
  within the probe window" into every future own ZTU: the same kind of unwritten contract that
  R1-2 removed. Row 7 is not supported, so the 3.1 s is accepted.
- Answering 0x01 in R's app with stock-format product info (the keys "p", "v", "g" that its
  parser looks for, literals at 0x488F4-0x488FC, frame len ≤ 246). The stock ZTU would then
  lock 115200, keep its name and stop its query, which makes row 7 partly usable. It is not
  needed for safety, so it is left as a product decision. If adopted, it must be in R's app (a
  unit under a stock ZTU never gets another one), and the stock-ZTU confirmation must count the
  queries before the first answer. A `test_tuya.c` case then checks the JSON and len ≤ 246.

### Tests that pin the rules (built in the implementation)

Fork tests use a new `StockMcu` model in `tests/tuya_mcu.py`. It answers 0x0B with 0x47 and
0x28 with a dump, echoes writes, ignores everything else, drops frames before `ready_ms`, and
checks every frame it receives from a ZTU that has not identified it.

| Rule | Test | Pass |
|---|---|---|
| 1, 2; contract item 4 | `tests/test_mcu_ota.py`: the class (b) and (c) cases of section 10, change 9; `test_own_mcu_restarting_alone_is_noticed` kept | (b) bounded as stated there, no 0xA0; (c) 0 frames with cmd 0xA0..0xA3; an identified MCU is re-asked at every restart |
| 3 | `tests/test_stock_mcu_compat.py` (new): a 0x47 `StockMcu` through cold boot, a ZTU-only restart with the MCU running, a late-ready MCU, Zigbee on / off / level / scene commands, the module-button 0x03, spurious seq-0 0x20 / OK 0xA2 / 0xA1 offset 0 / 0xC0 frames, and 10 simulated minutes idle | The TX trace (cmd, data, order, time ±50 ms) equals a golden file recorded from 51f5c95a. Cmds ⊆ {0x0B, 0x28, 0x04, the 0x20 / 0x24 / 0x25 replies}, 0xA1 replies only right after an 0xA1 request, zero 0xA0 / 0xA3. The only allowed difference is the dump retry in the late-ready scenario. The scratch run of 51f5c95a gives the baseline: 0x0B / 0x28 at 2000 / 2050 ms, a 0x20 reply + 0x0B + 0x28 again after the first seq-0 query, 38 / 86-byte 0xA1 replies only on request |
| 4; contract item 6 | `StockMcu` frame checks under the gate, plus a fuzz of every Zigbee-side input (ZCL commands, diag control 2, config and dp_config writes, OTA notify) | Every frame ≤ 29 bytes; frames < 2 ms apart add up to ≤ 23 bytes; 0xA1 replies only after a request |
| 5; contract item 3 | `ready_ms` sweep 0..3000 against every build, an early-UART build included | The first frame the ZTU starts itself (not an 0xA1 answer) goes out ≥ 2000 ms after the end of app init. DPs 3 / 5 known and ballast min = `tuya_to_zcl(201)` for every `ready_ms` ≤ 3000 (≤ 2000 without the retry). A 0x06 report and a 0x03 injected before init are dispatched only after `hal_zigbee_init` (stub hook), and nothing reaches `hal_factory_reset` or the steering call before it |
| 6 | Stub variant without `SECONDARY_MCU_IMAGE` (the config of a TPZ2 touch switch, as in `test_mcu_link_robustness.py:122`) with an MCU that answers 0x0B with 0x80 and sends a seq-0 0x20 every 30 s; a CI `nm` check | Zero 0xA3; first contacts only as in phase 1 (the timer, and once more at the MCU's first query), none at later seq-0 queries; `tuya_mcu_ota_may_be_own` is the stub in every board build whose `device_db` entry has no `secondary_mcu_image` |
| 7 | The converter tests (`test_converter_own_mcu.py`) on the stock fingerprint; optionally unknown DPs 130..137 on it | Exposes and `mcu_firmware` "Tuya (stock)" unchanged; unknown DPs ignored without an exception |
| 8, app | `tests/test_tuya.c` (section 11) | Confirmed with two 0x01 and no 0xA3; not with one 0x01 or none |
| 8, bootloader | `make test-boot`, group "Stock ZTU" (section 11), with and without the 9600-baud garbage phases | As stated there |
| 8, link watchdog | The link watchdog's host tests (section 9, item 11) fed a stock-ZTU frame stream (0x01 every 1 s, 1 s phases of 9600-baud garbage) for 24 model hours | Never fires; with the stream stopped it fires once per 4 h and only while dark |
| 9 | `tests/test_stock_mcu_compat.py` (rule 3) with 16-byte mode-6 requests injected from the MCU side | The stock-MCU trace unchanged; mode-6 requests answered like mode 3 |
| Stock ZTU facts | `tests/test_stock_ztu_dump.py` (section 11) | The parser and table facts above match the file |
| All, on the modules | Section 14, step 3: diag decode per stock module before and after its ZTU OTA | No 0xA0..0xA3 entries; the dump DPs present; entities unchanged |

---

## 16. Review resolution

Three reviewers attacked the first version of this design (commit f1a5ddd): recovery and
bricking (R1), protocol and compatibility (R2), size, timing and implementability (R3). No
blocker was found. Every major and minor finding is listed with its resolution. Findings that
several reviewers made are resolved once and cross-referenced.

| ID | Sev. | Finding | Resolution |
|---|---|---|---|
| R1-1 | major | A counter rescue after a crash before `dimmer.c:215` leaves `restored` at 0, so the image installed hours later restores a stale light state | Adopted in frozen code: the bootloader sets `restored` = RST1 when it opens a counter rescue window (+16 B). Sections 3, 4; harness scenario `restored`. |
| R1-2 | major | The frozen probe window is an unwritten timing contract with a ZTU that changes over the air | Window frozen at 3000 ms instead of 1.5 × the spare; ZTU contract (0xA1 semantics, robustness, 1500 ms, identity after every start, results) in `ota_proto.h` and `tuya_mcu_ota.h`; fuzz and timing tests; B2 on every ZTU release incl. its first boot after an OTA; DP 137 watch after each rollout. Sections 5, 10, 14. |
| R1-3 | minor | Rescue and no-app mode install SIM/DIAG images carried by mistake | Adopted: every mode asks for the header first and refuses bench flags, a wrong magic and a size beyond the layout, before anything is written (no-app mode too). Sections 6, 8; scenario `benchrescue`. |
| R1-4 | minor | The bootloader leaves the IWDG period to reset values or the last app | Adopted: PR 3 / RLR 1000 with bounded waits (+44 B), and (implementation review) the reload after the wait that puts the new RLR in the counter (+8 B). Section 4; scenario `wdg-carry`. |
| R1-5 | minor | `tries` saturates: every warm reset after the third opens a window, not every third; a lost first query means no confirmation | Text corrected (sections 6, 7, 13 #18); *measured* 7 windows in 10 boots. App over the air: seq 0 until the ZTU answers (section 9, item 9). |
| R1-6 | minor | Section 10 serves build 0 in one item and forbids it in another | One rule: the ZTU refuses to offer, never to serve, what the bootloader may install; build 0 is served. Same as R2-1. |
| R1-7 | minor | Ring move: a post-M app without the lower-half scan loses settings after a downgrade; step 2 checks the version, not the copy; 16 pages halve endurance | Adopted: a generic scan rule for every app from M on; M reports "ring moved" and step 2 checks it; endurance and save rate are a precondition of M. Section 1. |
| R1-8 | minor | Counter rescue ignores the ring layout and can overwrite live settings | Adopted for every mode with an installed app, the button included (user decision 2: an image never overwrites settings); only "no valid app" stays ungated. Sections 1, 6; scenario `bigrescue`. |
| R1-9 | minor | A stuck button gives a 30 s window and an unintended install at every power cut | Adopted: held 100 ms, then released within 5 s; a stuck button costs 5 s and installs nothing (+52 B with R2-6). Section 4; scenario `stuck`. |
| R1-10 | minor | The harness cannot see IWDG gaps, parser faults or a wrong guard range | Adopted: IWDG key model with a 250 ms bound, CRC time, frame fuzz, the real flash driver against a fake FLASH peripheral, `nm` checks of the guard range. Section 11; the prototype ran the fuzz (0 of 300 failed). |
| R1-11 | minor | No tool requires CONFIRMS; no build wrap rule; test builds can outnumber the next release | Adopted: `mkimage`/`embed_ztu` require CONFIRMS for builds ≥ 256; releases exactly +100; test builds +1..99 (`FW_TEST`). Sections 2, 11. |
| R1-12 | minor | An app with a dead UART that never resets is never rescued automatically | First not adopted (an app-side link watchdog would disturb a working light whenever the ZTU is the broken side). Superseded by user decision U4: a frozen hook (+36 B) and a watchdog in R's app that fires only while dark, at most once per 4 h, and never feeds the counter. Sections 5, 7, 9 (item 11), 13 #19, #26, #27. |
| R2-1 | major | Build-0 rules contradict each other; rollout step 2 left the phase-3 ZTU nothing to carry | One rule (R1-6); rollout reordered: release R first, the phase-3 ZTU carries R's app, the spare takes it under v1; matrix row "2.1.x / v1 / phase 3" added. Sections 2, 10, 14. |
| R2-2 | major | Frozen requests say nothing about the bootloader or its mode, so no future ZTU can tell a crash loop from an update | Adopted: 16-byte request (bootloader version, mode, installed header bytes 8..15) and a 4-byte result with the build (+24 B); a 6-byte request means v1. Header-first also cut a rescue window's offset-0 requests from 61 to 0. Section 8. |
| R2-3 | major | ZTU timing and 0xA3 behaviour are not written down; "the UART cannot start earlier" is wrong | Contract and tests as R1-2; identity asked without a carried image (phase 3; withdrawn by FA-4: every ZDMS16 build carries one); boot timing diagnostic; the claim corrected (the pins are parsed at `config_parser.c:359`, but an earlier start only buffers without a poll during init; kept as a ZTU-side option). Sections 5, 7, 10. |
| R2-4 | minor | `image_info` checks less than `app_valid()`; a bad-SP image replaced a working app | Adopted twice: the phase-3 `image_info` mirrors `app_valid()` plus layout and build rules, and the bootloader checks page 0 before the first write (+60 B). Sections 8, 10; scenario `badsp`. |
| R2-5 | minor | Saturating `tries` (as R1-5); a ZTU without a carried image never asks 0xA3; SWD benches fall into windows | As R1-5; every ZDMS16 ZTU carries an image, so it always asks (R3-9 as corrected by FA-4); bench builds are never counted; tools clear the boot word before their resets. Sections 3, 7, 11. |
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
| R3-9 | minor | The ZTU asks 0xA3 only when it carries an image | First adopted, then withdrawn on re-check (FA-4): the condition always holds in ZDMS16 builds (`embed_ztu.py` has no empty mode), and dropping the `#ifdef` would ask factory MCUs of other boards. The scope is written down instead. Section 10, change 2. |
| R3-10 | minor | The flash busy wait (~0.67 s) outlasts the cold-boot watchdog | Adopted: bounded at ~50 ms in the shared driver; a BSY-never-clears test. Section 8. |
| R3-11 | minor | `-Werror` fails on the jump cast; the harness links zlib; wrong `flash_drv.c` line references | Fixed: cast through `uintptr_t`; the harness has its own CRC; references corrected (`flash_drv.c:46, 54-58, 83-86, 90-94`). Sections 4, 8, 11. |

Measured corrections that came with the review: `app_valid()` takes ~43-54 ms, not ~25 ms; the
DP 136 CRC takes ~15 ms, not ~7 ms; an asked update takes ~2.5 s in the byte-timed model, not
0.9 s.

### Factory firmware compatibility reviews (design at 316a157)

Two reviewers checked the revised design against the factory firmware, after the user's
requirement of 2026-11-18 (section 15). Direction A (FA) put the stock PY32 1.0.7 behind the
phase-3 ZTU. Verdict: compatible as designed for the modules of today, but not yet guaranteed by
the design. Direction B (FB) put Open Dimmer behind a stock Tuya ZTU. Verdict: no harm to the
stock ZTU, but no support statement, and a real counter cost. No blocker was found. Every major
and minor finding is listed with its resolution.

| ID | Sev. | Finding | Resolution |
|---|---|---|---|
| FA-1 | major | Contract item 4 had no cap for MCUs not yet identified, and a literal contract test would push the `UNIDENTIFIED_RESTARTS_MAX` limit out. *Measured* (0x80 MCU, a seq-0 0x20 every 30 s, 10 min): with the limit 4 dumps and 17 0xA3, then nothing; with 255, 21 dumps and 83 0xA3, without end | Adopted: item 4 in three classes (identified; major ≥ 2 unidentified, ≤ 3 treated restarts per ZTU run plus 4 + 2 questions; major < 2 or no 0x0B answer: never asked). The major comes only from a 0x0B answer in this run. The constant moves to `tuya_mcu_ota.h`. Two tests. Sections 7, 10, 15. |
| FA-2 | major | The earlier-UART option had no stock rules. Moving `tuya_mcu_app_init()` moves the one-shot dump, which is lost when the stock MCU listens later than ~2150 ms after app init (ballast min 1 instead of 51, empty entities, low levels below the minimum). A poll during init would dispatch stock frames, the 0x03 reset included, on an uninitialised stack | Adopted: change 6 (a) the timer starts at the end of the full init, ≥ 2000 ms; (b) answer-only (0xA1) before it, other frames dispatched after init; plus a dump retry in phase 3 whether or not the UART moves. (a) and (b) are contract item 3's condition. `ready_ms` sweep and pre-init dispatch tests. Sections 10, 15. |
| FA-3 | major | "Nothing changes for stock MCUs" was not pinned by any phase-3 test; the new item 4 triggers were tested only with own MCUs; rollout step 3 did not read the diag log | Adopted: change 7 states UART-identity to phase 1, with the dump retry as the one listed difference. Stock-compatibility gate `tests/test_stock_mcu_compat.py` against a golden trace of 51f5c95a. Rollout step 3 reads the diag log (FA-6). Sections 10, 14, 15. |
| FA-4 | minor | Dropping `secondary_mcu_image_size != 0` changes nothing in any existing build. Making "identity without an image" true would ask factory MCUs of other boards and rerun their IT inits | Adopted: change 2 withdrawn. The question stays limited to boards with Open Dimmer support (`SECONDARY_MCU_IMAGE`, or a future `own_mcu` flag), and a ZDMS16 build without an image is forbidden. Stub-variant test and CI `nm` check. Sections 7, 10, 13 #18, 15; R2-3, R2-5 and R3-9 corrected. |
| FA-5 | minor | The stock parser's frame-size limits (23-byte ring, 31-byte buffer, a 32..38-byte frame wedges RX) were not in the contract | Adopted: contract item 6 (≤ 29 bytes per frame, ≤ 23 per burst, one in flight; over 29 only as a direct 0xA1 reply). Limits recorded in section 15, and in stock-firmware-analysis.md with the implementation. Model checks and a fuzz. |
| FA-6 | minor | Rollout step 3's user-level check cannot see UART-level changes or a lost dump | Adopted: per-module diag decode before and after the ZTU OTA, a ZTU restart with the light on, `mcu_version` 1.0.7 checked first, a checklist row per module. Section 14. |
| FB-1 | major | Under a stock ZTU nothing confirms: from the 3rd warm reset after a power-up every warm reset gives 30 s dark and then the power-on behaviour, and no window can install anything. The SWD-installed app is final there, so "everything but the bootloader ships over the air" did not hold | Adopted in R's app, not in frozen code: confirm after 30 s also when no 0xA3 came and ≥ 2 0x01 queries did. Contract item 7 (an own ZTU never sends 0x01). Decisions row 9-11 and section 9 corrected; the stock ZTU named in sections 3, 6, 7 and 13 #18. Tests in `test_tuya.c` and `stockztu-counter` (0 windows; the old rule 8). |
| FB-2 | major | The design had no support statement for Open Dimmer under a stock ZTU | Adopted: matrix row in section 2, and section 15's subsection with not supported / guaranteed / recovery. INSTALL and STATUS wording comes with the implementation (section 14, step 1). |
| FB-3 | minor | What a stock ZTU does with unknown commands was not answered | Adopted: the dump facts with addresses and the safe assumption for other stock releases are in section 15, and go into stock-firmware-analysis.md with the implementation. `tests/test_stock_ztu_dump.py` pins them. |
| FB-4 | minor | A stock ZTU always costs the full 3.1 s, which four places contradicted, and "probe time 0 = contract miss" is wrong there | Adopted: sections 4, 5, 13 #21 and 14 name the stock ZTU; the miss rule is limited to own ZTUs of phase 3 or later. An early probe exit on 0x01 in frozen code was considered and not adopted: it would freeze a timing assumption on every own ZTU. Scenario `stockztu-cold`. |
| FB-5 | minor | The harness had no ZTU that never answers 0xA1 while sending its own frames (and 9600-baud garbage when it cycles its UART) | Adopted: stock mode in the ZTU model and the "Stock ZTU" group in `make test-boot`. Section 11. |
| FB-6 | minor | The only way out is the conversion OTA, which needs the stock ZTU's full name; a reset or re-pair may lose it | Adopted as doc rules: do not re-pair or reset, convert the ZTU first; ZTU first stays a hard rule in INSTALL. Answering 0x01 in R's app is listed as a product decision, not adopted (not needed for safety). Section 15. |
| FB-7 | minor | DPs 130..137 reach Zigbee2MQTT as unknown DPs under a stock ZTU | Adopted: noted in section 9, item 6, and section 15. |

### User decisions (2026-11-18)

The user decided the open points of the revised design. They are binding, and this document
follows them everywhere.

| ID | Topic | Decision | Where |
|---|---|---|---|
| U1 | Probe window | 3 s (`PROBE_WINDOW_MS` 3000, frozen), as designed. | Sections 4, 5 |
| U2 | Button rescue | As specified (held at power-on, released within 5 s, 30 s window). Wording: the module sits in an insulated case, the button is pressed through the case, and the case is never opened while the mains is connected. | Sections 4, 14 (INSTALL.md) |
| U3 | Rescue windows and the light after them | 30 s windows (`RESCUE_WINDOW_MS`), and after a counter rescue the light takes the power-on behaviour (`restored` = RST1), as designed. | Sections 3, 4, 6 |
| U4 | Link watchdog | The frozen bootloader gets a hook: through a mailbox bit the app can ask for the power-on probe at its next start (newer build only, 3 s window). The first v2 app ships the watchdog with defaults: it fires only after 30 min without a frame or answer from the ZTU, only while every channel is off, at most once per ~4 h (back-off); it then sets the bit and resets. The warm reset restores the state by the existing restore logic; the wall switch is unresponsive for at most ~3 s. Supersedes the resolution of R1-12. | Decisions row 14; sections 3, 4, 5, 7, 9 (item 11), 10 (item 8), 11, 12, 13, 14 (B11), 15 |
| U5 | Factory firmware | Compatibility with the factory firmware (section 15) is a hard requirement. | Section 15 (rule 9 added for U4) |

Readings this document chose where U4 left the details open, each on the safe side:

| Point | Chosen | Why |
|---|---|---|
| The mailbox bit | Boot word bit 7 (`BOOT_PROBE_REQ`); bit 6 (`BOOT_INFO_LINK`) records that a link probe ran; the tries move to bits 3..0 and are always read through `BOOT_TRIES_MASK`. | The info nibble was full; the tries byte held 0..3 only. The mask is frozen code: an unmasked compare would read bit 6 as 64 tries and open windows (*measured* with the mask: `..WWWW` as before). |
| "Same rules as the power-on probe" | The request only adds "or a requested warm boot" to the probe's condition; everything else is the probe's code (header first, newer build, layout, no bench, page-0 check, 3 s, first answer ends it). | Nothing new in `run()`, nothing new to get wrong. |
| "Counts as a probe, not a rescue" | No window, no `restored` mark, no skip, no older image; for the counter an ordinary warm boot (+1), with the app writing tries 0 together with the request, so the app runs with tries 1 as after a power-on probe. The order update > rescue > probe stays: a request never displaces an asked update or a due counter rescue. | Without tries 0 a link restart every 4 h under a dead ZTU would open a counter window from the 4th one, then use the power-on behaviour (a lamp set to "on" would switch on at night), against U4's "restores the state". Keeping the tries rule in the app rather than the frozen code leaves it changeable over the air. Cost: section 7, "Link watchdog", and section 13 #27. |
| "Cleared by the bootloader" | At every boot, honoured or not, before anything else uses the word; a cold boot rebuilds the word anyway. | A stale request can never make every later warm reset a probe. |
| The back-off | A minimum run length of 4 h before a link restart; no state across resets. | Every restart begins a new run, so "at most once per ~4 h" holds without any extra mailbox state; the first restart in a long run still comes 30 min after the last frame. |
| Where the watchdog is armed | Only with a v2 bootloader that announces `BOOT_FEAT_LINK`, a valid boot word, in normal builds. Off under v1 and in SIM / MEASURE / DIAG builds. | Without the probe the restart buys nothing; bench builds skip the probe. |
| "Every channel off" | Both channels off and dark (no fade or hold), and no wall-switch or button input pressed. | A press at the moment of the restart would be lost. |
| "Frame or answer from the ZTU" | Any well-formed frame (header, length, checksum), any command; noise bytes do not count. | Covers own ZTUs (0x20 answers every 30 s; contract item 8 added) and the stock ZTU (0x01 every 1 s), so the watchdog never fires under a working ZTU of either kind. |
| "Unresponsive for at most ~3 s" | Holds when nothing newer is carried (*measured* 3.12 s when the ZTU does not answer 0xA1, 0.05 s when it does). When a newer build is carried, its install adds ~2.5 s (64-byte blocks), as after a power cut. | Installing a fix is the point of the hook; it happens only while the light is off. |
| Request byte 7 | `OTA_MODE_LINK` (6) instead of 3, also for the update it starts. | The ZTU log can tell a link probe from a power-on probe (R2-2's purpose); every ZTU serves it by contract item 1. |

---

## 17. Implementation readings (2026-11-19)

Where this text left a choice, the implementation took the safe reading below. Each one is
pinned by a host test (`make test`, `make test-boot`, or the fork's stub tests).

**Bootloader (`boot/boot.c`).**
- The LED blinks fast once a block is taken, also in a probe- or link-started update (the
  section 4 table and B7); "the LED stays off" in section 5 is the listening phase. The LED is
  switched off before the jump.
- A header answer is taken only if the carried header's size and CRC equal the block header's
  (contract item 1). CRC 0 is refused in every mode (in a rescue window a pinned 0 would mean
  "not pinned" and the window would never end).
- The probe refuses builds above `OTA_BUILD_MAX`, and compares with the build installed at this
  boot as the request carries it, not the live header (which may hold a half-written image).
- Asked mode takes only the mailbox CRC; asked with an invalid app is NO_APP (any image).
- The boot word, with bit 7 cleared and the run's info bits, is stored right after the mode
  decision, so a reset during the run never replays a link request. Bits 5..4 are passed
  through.
- After a give-up the run switches to NO_APP and keeps its request pace (501 ms over 1 h). A
  taken header counts as progress (section 8). A block that reaches past the image end is a
  stray, so a ZTU that ignores `max_len` never completes an update (contract item 1).
- The HSIRDY wait is bounded too. A byte that cannot start a frame is dropped at once; a too
  long length rescans from the next byte.
- `watchdog_start()` ends with a reload (KR 0xAAAA) after the bounded update wait: the section 4
  sequence without it left a warm boot that only jumps on the previous app's countdown (an RLR
  write reaches the counter only at a reload). The harness models an IWDG kept over warm resets.
- `g_boot` (SWD state) keeps v1's first 28 bytes for `tools/ota_swd_emu.py` and adds the build
  (0x1C) and the mode (0x1E).

**App.**
- DP 137 is 0 when the boot word is not valid (v1, random RAM, a torn pair).
- Link watchdog "no input pressed": the button, momentary and state switches open, no input
  event waiting, no change being debounced; a toggle switch resting closed does not block it.
  "Dark": both channels off and the fader output 0. "A frame": what the app's parser takes
  (55 AA 02, at most 96 data bytes, checksum). The silence is `now -` the last frame's time;
  only the 4 h flag is sticky. Added after the conformance review: the idle condition must have
  held for `LINK_WDG_QUIET_MS` (1 min), so a switch flipped back right after an off is never
  lost to the restart (+20 bytes).
- The app's watchdog start has bounded waits like the bootloader's.
- The confirmation is idempotent (it writes only while tries != 0); a well-formed 0x01 is cmd
  0x01 with length 0.
- The late-press rule's cold start is the app's power-on rule (`DIMMER_COLD_START`), measured
  from the app's start.
- `make FW_PREV=1 FW_TEST=n` builds below the release (B3, B11), and `LINK_WDG_TEST=1` builds
  only there. The Makefile and `src/tuya.h` refuse every other combination, with the same words.

**Docs.** `docs/stock-firmware-analysis.md` holds the two sections of section 14, step 1
("Stock ZTU and unknown commands", "Stock MCU parser limits", the 1.0.7 limits marked inferred),
and contract item 6 in `src/ota_proto.h` and the fork's `tuya_mcu_ota.h` points at the second.
A re-check of the dump found a fifth read of the product-info flag (0xD9FE, next to 0xD956);
it is not reached from the 0x05 handler either, so the statement above holds.
`tests/test_stock_ztu_dump.py` also pins the bootloader region without UART register literals
and the flag's readers.

**Tools.** `mkimage.py` and `embed_ztu.py` refuse more than section 11 lists, all on the safe
side (version byte < 0x80, vectors the bootloader would not start, a build that is not a
release build or release + n with `--test n`, reserved flags, mismatched flags).
`install.sh` clears the boot word before every reset its tools issue (pyOCD `load` resets after
each write), writes `DBG_IWDG_STOP` right after the halt, reads WRPR at every attach and clears
a set WRP before writing; `make flash` also refuses an image over 12 KB. The ring-mover gate of
`release.sh` reads the earlier releases' RING_HIGH from their released images.

**ZTU (fork, phase 3).** The 2 ms burst gate is code for every board, towards MCUs not
identified as Open Dimmer only, and a burst also ends at an MCU answer. The dump retry is timed
from the first contact. Block requests are logged once per run as BOOT_REQUEST, not per frame.
A request shorter than 16 bytes is recorded as a v1 bootloader. `send_frame` refuses frames
over 29 bytes.

**Release R.** Version byte 0x9D (2.1.13): the smallest step above 2.1.12, which keeps the
version space (item 31); build 300. Its headroom was recovered before the tag (section 9:
11 632 bytes, 656 free), with the register values unchanged: `clock_init()` writes what
`LL_RCC_HSI_SetCalibFreq()` and `LL_Init1msTick()` wrote, and the UART's BRR is 208 from
constants (a compile-time check). The release gate then needs no acceptance.
