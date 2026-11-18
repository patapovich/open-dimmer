# Bootloader v2: design (fix plan phase 3)

Status: design, 2026-11-18. Nothing here is implemented yet. Code references are to this repo
at 421685a and to the fork `patapovich/tuya-zigbee-switch`, branch `home-dimmer` at 51f5c95a
(paths under `src/zigbee/` there are marked "fork"). Item numbers refer to the review of
2026-11-18, which is kept privately (see STATUS.md).

The bootloader can only be replaced over SWD (mains off, probe on CN2). Every module installed
from now on keeps the v2 bootloader for good, so v2 is designed to never need replacing: it
gets every escape path that does not depend on the app, accepts a larger app than today, and
leaves every policy that may still change (version rules, offers, settings layout) to the app
and the ZTU, which update over the air.

Numbers marked *measured* come from two throwaway experiments that are not in the repo:
- the review's host harness (the unmodified `boot/boot.c` against flash and RAM mapped at
  their chip addresses, forked boots, and a C model of the ZTU's block server);
- a prototype of this design (`boot.c` rewritten as specified here, built with
  arm-none-eabi-gcc 13.2.1 `-Os` like `make`), run in an extended copy of that harness with
  the scenarios of section 11 (all pass, 1.1 s of CPU time).

## Decisions in short

| # | Topic | Decision |
|---|---|---|
| 1 | Layout | Bootloader alone in sector 0 (4 KB). It takes apps up to 14 KB (`BOOT_APP_MAX` 0x3800). Settings ring stays at 0x08004000 (4 KB) until an app needs more than 12 KB; a "ring-move" release first moves it to 0x08004800 (2 KB). Sector 0 is not write-protected in phase 3 (the app gets an address guard instead). |
| 2 | Versions | 16-bit build number in the app header's reserved bytes (0xCE..0xCF). The version byte stays for display, for the converter and for old peers. Identity (0xA3) and offer (0xA0) get the build appended; old peers ignore the extra bytes. |
| 3 | Mailbox | The two reserved words become the boot word (tries, rescue info, probe answer time) and its complement. The v1 bootloader never reads them. |
| 4 | Boot flow | Gates low first, then the watchdog, then the reset cause from the RCC flags (never cleared by the bootloader). Mode order: update (asked, or no valid app) > rescue (button, or 3 unconfirmed boots) > power-on probe > jump. |
| 5 | Probe | After a cold reset only: header-only 0xA1 for offset 0xC0 (16 bytes) every 100 ms for up to 1.5 s (provisional, set from a measurement on the spare). Takes only a valid, larger build number with another CRC that fits the installed app's settings layout. No ZTU change needed. |
| 6 | Rescue | 30 s listening window. Takes any image the ZTU serves whose CRC differs from the installed one, older ones included. With nothing new it starts the app again, and the next unconfirmed reset listens again. |
| 7 | Confirm | The app confirms after 30 s of uptime once it has answered a 0xA3 in this run. Only apps with the header flag CONFIRMS are counted, so 2.1.x apps never fall into rescue loops. |
| 8 | Update path | Give-up is based on progress (item 28). Stray blocks are asked for again after 50 ms; other images are paced at 500 ms. Pages are retried once. The SR-flag shim is dropped. Blocks of up to 128 bytes are taken; the ZTU stays at 64 bytes for now. |
| 9-11 | App, ZTU, tools | Listed in sections 9-11. Every change except the bootloader ships over the air. |
| 12 | Size | *Measured* prototype: 2780 of 4096 bytes (+644 over v1's 2136). The three rescue paths take 464 bytes, above the ~300-byte estimate; ~1.3 KB stays free. |

---

## 1. Flash layout and constants

PY32F002A: 20 KB flash, 128-byte pages (the erase unit used here), 4 KB sectors (the
write-protection unit and pyOCD's erase unit), 3 KB RAM.

| Region | Address | Size | Content | Written by |
|---|---|---|---|---|
| Sector 0 | 0x08000000-0x08000FFF | 4096 | Bootloader v2 (~2.8 KB code, the rest 0xFF). `boot_id_t` at 0x08000010. | SWD only |
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
```

`boot_id_t` sits in its own section, placed in `boot.ld` right after `.isr_vector`, so
`boot.bin` stays contiguous and `mkimage.py --full` keeps padding it to 4 KB with 0xFF. On the
v1 bootloader, 0x08000010 holds code, so the magic does not match. The app takes that as "v1:
12 KB, no counter, no probe".

**App limit (app side).** `app_limit() = min(_settings_start - APP_BASE, boot_id ok ? app_max :
APP_LIMIT_LEGACY)`. Today that is 12 KB with either bootloader. After the ring move it is 14 KB
on v2 and stays 12 KB on v1.

### How a >12 KB image never overwrites a live ring at 0x08004000

| Path into a write | Gate |
|---|---|
| Offer (app consent) | 2.1.x apps check `size <= APP_MAX_SIZE` (12 KB, `src/tuya.c:193`). v2 apps check `size <= app_limit()`. |
| Power-on probe (no consent) | The installed header's flag RING_HIGH sets the limit: 14 KB with it, 12 KB without (section 5). |
| v1 bootloader | `app_valid()` and `on_block()` cap at 12 KB (`boot/boot.c:184-186, 242`), so a unit with v1 never runs or writes past 12 KB. |
| Tools | `mkimage.py` refuses >12 KB without RING_HIGH. `release.sh` refuses the first >12 KB release unless a RING_HIGH release of at most 12 KB was released before it (section 11). |
| Rescue (button, counter) and "no valid app" | No gate on purpose: up to `BOOT_APP_MAX`. Recovery comes before settings. The worst case is that records in 0x08004000-0x080047FF are overwritten and the new app loads an older record from the upper half, or its defaults. Code is never corrupted. |

### Ring-move sequence (only when the app nears 12 KB; CI already warns below 512 bytes free)

1. **Release M (the ring mover).** Same features plus:
   - Linker: `SETTINGS ORIGIN = 0x08004800, LENGTH = 2K`; app `FLASH LENGTH = 14K`.
   - Header: RING_HIGH set (one Makefile switch sets both the linker and the flag; CI checks
     that the flag matches `_settings_start`).
   - Image size: still at most 12 KB, so every installed app accepts it and a v1 bootloader can
     run it.
   - `store_load()` scans all 32 pages (0x08004000-0x08004FFF) with today's newest-sequence
     rule. If the newest valid record is in the lower half, it is saved into the new ring at
     once with seq + 1, before anything else is saved. The sequence counter runs across both
     halves, so the copy is newer than every old record.
   - M never erases or writes the lower half.
   - A power cut during the copy leaves the old record in place, and the next start copies it
     again.
2. **Spread M.** Deploy M to every module. Check that each one shows M's version or a later one
   in `mcu_version` (a release always bumps the version byte, section 2).
3. **Larger apps.** Releases over 12 KB are allowed from then on. They drop the lower-half scan,
   because that area is their own code. A module still on a pre-M app is safe: it declines the
   big offer and its probe refuses it (both 12 KB gates). It stays on its app until the ZTU
   carries M again.
4. **Downgrade after step 3.** If a rescue installs a pre-M app, that app finds code (no valid
   record) in the lower half and records of its own format in the upper half. It works, possibly
   with older settings.

SWD note: pyOCD erases whole 4 KB sectors (Makefile:98-102). An SWD install of an image over
12 KB therefore erases sector 4, the ring. `install.sh` must refuse such a full image unless
`ERASE_SETTINGS=1` is given, and docs/INSTALL.md must say so.

### Write protection (WRP) of sector 0

How it works: the option bytes at 0x1FFF0E80 have the layout of `tools/swd/ob_bor25.bin` (`aa f9
55 06 | ff 00 00 ff | ff ff ff ff | ff ff 00 00`: OPTR 0xF9AA, SDKR 0x00FF, a reserved word, WRPR
0xFFFF; OPTR, SDKR and WRPR each followed by its complement). A 0 bit in WRPR protects one 4 KB sector. Sector 0 alone is WRPR
0xFFFE, complement 0x0001: `... fe ff 01 00` in the last four bytes.

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
| 0xCD | 1 | flags | SIM 0x01, MEASURE 0x02, DIAG 0x04 | + CONFIRMS 0x08, RING_HIGH 0x10; 0x20..0x80 reserved, 0 |
| 0xCE | 2 | build (u16) | reserved {0, 0} | `FW_BUILD` |

`sizeof(app_header_t)` stays 16. The v1 bootloader reads only magic, size and CRC. Old ZTUs read
magic, size, CRC, version and flags (fork `tuya_mcu_ota.c:78-102`), so neither notices the new
fields.

### Reserved codes

| Field | Value | Meaning |
|---|---|---|
| version byte | 0x00-0x7F | Not ours: the ZTU asks for the identity only from major 2 (fork `tuya_mcu_ota.c:18, 198`), and the converter takes major < 2 for a stock MCU. Major stays at least 2 forever. |
| | 0x8E / 0x8F | DIAG / MEASURE default bytes (Makefile:38) |
| | 0xBF | SIM builds only (`src/tuya.h:31`) |
| | 0xFF | Never (an app with it could not take a byte-rule offer) |
| build | 0 | No build number (2.1.x and older) |
| | 1-255 | Never (refused by `mkimage.py`) |
| | 256-0xFFFD | Normal builds, strictly increasing per image ever flashed or carried |
| | 0xFFFE | SIM builds: no image is newer, so nothing is ever offered to them |
| | 0xFFFF | Never |
| flags | 0x07 | Bench builds (SIM / MEASURE / DIAG): never carried (`embed_ztu.py`, ZTU), never replaced by the probe; leave over SWD or a forced rescue |

**Rules for humans** (enforced in section 11):
- `FW_BUILD` lives in `src/tuya.h` next to `FW_VERSION`. It is bumped for every image that goes
  onto a module, including spare test builds (`make FW_BUILD=n`).
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
| v2 app (0xA0) | `len >= 11` (build-aware ZTU): `build > FW_BUILD`. Shorter (phase-1 ZTU): `byte > FW_VERSION && byte != 0xBF && byte != 0xFF`, as today. Both also need `0xD0 <= size <= app_limit()`. |
| 2.1.x app | `byte > FW_VERSION`, size within 0xC0..12 KB (unchanged, `src/tuya.c:189-193`) |
| phase-3 ZTU (build-aware, section 10) | Identity of 9 or more bytes: offer if `carried build > app build`. 7 bytes (2.1.x app): offer if `carried byte > app byte` (today's `tuya_mcu_ota.c:254`). |
| phase-1 ZTU | `carried byte > app byte` (unchanged) |
| v2 bootloader, probe | `carried build > installed build` (a header without one counts as build 0). Neither side looks at the byte. |
| v2 bootloader, rescue | Order does not matter: any CRC other than the installed one (section 6) |

### Compatibility matrix

| MCU app | Bootloader | ZTU | Offers | Probe | Counter rescue | Notes |
|---|---|---|---|---|---|---|
| 2.1.x | v1 | phase 1 | byte | - | - | The spare today |
| v2 app | v1 | phase 1 / phase 3 | byte / build | - | - | The spare between the app OTA and its SWD re-flash. `app_limit()` is 12 KB (no boot_id). The confirmation writes are ignored. |
| 2.1.x | v2 | any | byte | yes (any build counts as newer than build 0) | no (no CONFIRMS) | Only after a rescue downgrade. Button rescue still works. |
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
| 0x14 | `restored` | - | never touched | read/write (`dimmer.c:205-216, 240, 245`) | same |
| 0x18 | `boot` (was `reserved[0]`) | - | read and written at every boot | - | reads at start; clears bits 7..0 to confirm |
| 0x1C | `boot_check` (was `reserved[1]`) | - | written = ~boot | - | rewritten with a confirmation |

Boot word:

| Bits | Field |
|---|---|
| 31..24 | Tag 0xB2. The word is valid only when the tag matches and `boot_check == ~boot`. Random RAM matches with p ≈ 2^-40; that matters for the first boot after an SWD install, a software reset with random RAM. |
| 23..12 | Probe answer time in ms since the first probe request (1..4095), 0 = no answer or no probe. `PROBE_WINDOW_MS` < 4096 is a static assert. |
| 11..8 | Info: bit 8 RESCUE (the counter started a rescue window), bit 9 BUTTON (the button forced one), bits 10-11 reserved 0. |
| 7..0 | `tries`: unconfirmed boots of a CONFIRMS app, 0..3 |

Life cycle:
- **Cold boot** (section 4): the word is rebuilt as `0xB2000000`. Probe time, info and tries
  all count since the last power-up.
- **Warm boot**: the word is kept if it is valid, otherwise rebuilt.
- **Asked update, or no valid app**: tries = 0, because the app just proved its update path or
  there is no app.
- **Image written** (result OK, just before the reset): tries = 0. A new image gets 3 fresh
  runs.
- **Before every jump**: if the app's header has CONFIRMS and tries < 3, tries + 1. The word is
  stored together with its complement.
- **App confirmation** (section 7): tries = 0, the other bits are kept.

Survival: the system reset keeps RAM, and both linker scripts end RAM at 0x20000BE0, so neither
startup touches the mailbox. After a power-up the RAM is random. The bootloader rebuilds the
boot word and clears the magic pair. The app ignores the restore words after a power-up
(`dimmer.c:206`).

Restore words: v2 never touches words 0x08, 0x0C and 0x14. The app's "restore once in a row"
policy keeps working unchanged:
- After a rescue reached through a crash loop, `restored` is already RST1, so the new app starts
  with the power-on behaviour.
- After a probe or button update the reset flags include PWRRSTF, so the new app also uses the
  power-on behaviour.
- After an offered update it restores the light as today.

---

## 4. Boot flow

### Sequence from reset

```c
Reset_Handler:
    pins_safe();          /* PA0/PA1 gates low (BRR) + outputs, PA5 LED off, PA6 input with pull-up:
                             about 10 instructions after reset, as today (boot.c:63-69, 358-371) */
    copy .data (the RAM flash routine), zero .bss; main();

main:
    clock_and_uart();     /* HSI 24 MHz with the factory trim, SysTick 1 ms polled, USART1 115200 8N1 */
    watchdog_start();     /* LSION, wait for LSIRDY at most ~10 ms, IWDG->KR = 0xCCCC; prescaler and
                             reload left as they are: reset values after a power-up (LSI/4, 4096:
                             ~0.5 s); the app sets ~1 s first thing in main (section 9) */
    cold  = !(RCC->CSR & (SFTRSTF | IWDGRSTF));  /* the bootloader never writes RMVF: the app reads the
                                                    flags for its restore policy and health DP 131 */
    asked = mb->magic == UPDT && mb->magic_inv == ~UPDT;
    g_expect_crc = asked ? mb->expect_crc : 0;   mb->magic = mb->magic_inv = 0;
    word  = (!cold && valid(mb->boot)) ? mb->boot : 0xB2000000;
    h     = installed header;  ok = app_valid();
    if (!ok || asked)                               { tries = 0;            run(UPDATE, GIVE_UP_MS); }
    else if (cold && button_held())                 { info |= BUTTON;  skip = h->crc; run(RESCUE, RESCUE_WINDOW_MS); }
    else if ((h->flags & CONFIRMS) && tries >= 3)   { info |= RESCUE;  skip = h->crc; run(RESCUE, RESCUE_WINDOW_MS); }
    else if (cold && !(h->flags & BENCH))           { limit = RING_HIGH ? 14K : 12K; run(PROBE, PROBE_WINDOW_MS); }
    if ((h->flags & CONFIRMS) && tries < 3) tries++;
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
| PWR + SFT | The bootloader's own reset after a probe or button update, before any app ran | no, so no second probe (*measured*: 0 header requests on the boot after a probe update) |
| PWR + IWDG | Power-up, then the app died before it cleared the flags | no, the counter goes on (*measured*, crash-loop scenario) |
| SFT and/or IWDG | Update restart, fault, watchdog | no |

A brown-out that keeps RAM shows PWR only and counts as cold (tries 0, one probe), the same
view the app takes (`dimmer.c:206`: power-on behaviour). Two hardware facts are assumed: PWRRSTF is set at power-up, and
a power reset clears SFTRSTF and IWDGRSTF. The app's restore policy already relies on the first
one. Both are checked on the spare (bench test B8).

### Watchdog

The bootloader starts the IWDG at every boot, so it no longer matters whether a software reset
stops a running IWDG. The old comments say it keeps running (`boot.c:293`, `main.c:158-159`);
this was never verified, and B8 records it.

`tick()` refreshes the watchdog on every pass of every loop (probe, update, rescue, button). The
longest blocking steps:
- `app_valid()` over 14 KB: ~25 ms (the review measured ~20 ms for 11.6 KB);
- a page write: ~3.5 ms erase + ~1 ms program (`src/flash_drv.h:1-4`), twice when retried.

Both are under 10 % of the 0.5 s minimum.

A 2.1.x app starts its own watchdog only at the end of init (`main.c:203`). With v2 the IWDG is
already running, as after every OTA today. Its init before that point takes ~40 ms (the longest
step is the 30 ms `LL_mDelay` in `controls.c:47`). Option byte IWDG_SW stays 1 (software
watchdog, OPTR 0xF9AA): no option-byte change.

### Button (PA6, active low, pull-up; `docs/stock-firmware-analysis.md` pin table)

The pull-up is set in `pins_safe()`. The button is checked only on a cold boot, after the
watchdog has started. It counts as held when PA6 reads low on every pass for `BUTTON_MS` = 100
ms. The loop ends at the first high read, so a released button costs nothing.

If the button is still held when the app starts, the app ignores it: an input closed at
power-up "rests there" (STATUS.md, Behaviour), so a long hold never turns into the ≥3 s
rejoin / factory reset.

### Timeouts

| Constant | Value | Why |
|---|---|---|
| `PROBE_EVERY_MS` | 100 | The light comes at most ~100 ms after the ZTU can answer |
| `PROBE_WINDOW_MS` | 1500, provisional | Upper bound of the light delay after a power cut (user: 1-2 s). Final value from B2: the largest measured answer time × 1.5, rounded up to 100 ms, between 1000 and 3000. It must be < 4096 (boot word field). |
| `BUTTON_MS` | 100 | Rejects bounce and the pull-up settling; the user only has to keep the button pressed while the power comes back |
| `REQUEST_EVERY_MS` | 500 | Unchanged (boot.c:34): the request pace when nothing comes |
| `REJECT_PACE_MS` | 50 | A stray block (wrong offset, duplicate, short) is asked for again 50 ms later instead of at once (review: ~210 requests/s in the deadlock) |
| `GIVE_UP_MS` | 10 000 | Unchanged value, now measured from the last accepted block (item 28) |
| `RESCUE_WINDOW_MS` | 30 000 | Section 6 |
| frame silence | 50 | Unchanged (boot.c:136-138) |
| result flush | 20 | Unchanged (boot.c:321) |

### What the light and LED do

The gates are driven low in `pins_safe()` and never touched again by the bootloader, so the
light is dark in every bootloader state. The app decides the light at its start: the power-on
behaviour after a cold boot, or the restore-once rule after a warm one.

| State | LED (PA5) |
|---|---|
| Probe | Off |
| Rescue or update, nothing taken yet | Slow blink, 512 ms on / 512 ms off |
| Writing (a block was taken) | Fast blink, 128 ms on / 128 ms off |

### Jump checks

`app_valid()` checks:
- the magic;
- `0xD0 <= size <= BOOT_APP_MAX`;
- the initial SP in 0x200xxxxx and the reset vector inside the image (as boot.c:184-186, with
  the new ceiling);
- the CRC.

Then `jump_app()` runs as today (boot.c:198-209): interrupts off, SysTick off, USART1 reset,
VTOR = APP_BASE, MSP = SP, branch. No stricter SP or Thumb checks go into the frozen code;
those belong in `mkimage.py` / `embed_ztu.py` (item 42).

### State machine

```
            +--------------------- reset ----------------------+
            | gates low, clock, IWDG, flags, mailbox, app_valid |
            +---------------------------------------------------+
   no valid app | asked          cold & button | tries>=3 & CONFIRMS     cold & not bench      else
            v                         v                                     v                  |
     UPDATE (pinned if asked)    RESCUE (skip installed CRC)        PROBE (header @0xC0)       |
     10 s without progress:      30 s without a block:              no answer in window,       |
       app valid -> JUMP           app valid -> JUMP                not newer, too big -> JUMP |
       else unpin, go on           else unpin, go on                newer -> UPDATE pinned     |
            |  image done                 |  image done                                        |
            +-----> result OK, tries = 0, NVIC_SystemReset <---------+                         v
                                                                      JUMP (tries+1 if CONFIRMS)
```

---

## 5. Power-on probe protocol

**Request.** A normal block request (`OTA_CMD_BLOCK`) for the header, seq 0. Bytes:
`55 AA 02 00 00 A1 00 06 | 00 00 00 C0 00 10 | 78` (offset 0x000000C0, max_len 16, checksum
0x78). It goes out every 100 ms from the start of the window.

**Answer** (fork `answer_block`, `tuya_mcu_ota.c:270-300`, unchanged): 38 bytes.
`55 AA 02 00 00 A1 00 1D | 00 00 00 C0 | size | crc | ver | 16 header bytes | cs`. Example, a
carried 11628-byte image with build 301: size `00 00 2D 6C`, then `41 50 44 5A 6C 2D 00 00
<crc LE> 9B 08 2D 01`.

The ZTU serves any offset without keeping state. The `off == 0` side effect
(`tuya_mcu_app_mcu_restarted()`, `tuya_mcu_ota.c:279-283`) does not fire for 0xC0, so the probe
leaves the ZTU's first-contact logic alone. A real update afterwards starts at offset 0 and
resets it as today.

Why offset 0xC0 and not offset 0 with max_len 0 (the review's first sketch)? That answer holds
only the block header (size, CRC, version byte), without the build number, and it fires the
`off == 0` restart on the ZTU.

**Decision.** All of these must hold, else jump:
- `cmd == 0xA1`, `n >= 13 + 16` and `offset == 0xC0` (other frames are ignored and the probe
  keeps listening);
- carried header magic == "APDZ";
- carried size (block header) ≤ limit, where limit = 14 KB if the installed header has
  RING_HIGH, else 12 KB;
- `(carried flags & 0x07) == 0`;
- carried CRC ≠ installed CRC;
- carried build > installed build.

Then `g_expect_crc` is pinned to the carried CRC and the update runs in the same loop (pace 500
ms, give-up 10 s). The first request is offset 0, so the ZTU resets its contact state and
answers whole blocks.

**When is the probe skipped?**
- On warm boots. A crash loop is the counter's job, and an asked update brings its own image.
- When the installed app is a bench build (flags & 0x07). DIAG and MEASURE builds decline every
  offer so that they stay (item 17, `src/tuya.c:179-185`). The probe must not undo that after
  each power cut, and SIM benches keep their build too.
- When the app is invalid or asked: UPDATE mode takes any image anyway.

**When can the ZTU answer?** The ZTU's MCU UART is up as soon as `tuya_mcu_app_init()` has run
(`app.c:52`, after `os_init` and `parse_config`: `telink/main.c:46-57`), and frames are handled
on every main-loop pass (`app.c:65`, `tuya_secondary_mcu_poll` at `tuya_secondary_mcu.c:663`).

The ~2 s before the ZTU's first own frame is its first-contact timer
(`FIRST_CONTACT_DELAY_MS 2000`, `tuya_mcu_app.c:156, 371`), not its UART start. The first 0xA1
also makes the ZTU check its carried image once (`image_info`, one CRC over ~11.5 KB).

The answer time is therefore expected to be a few hundred ms, but it has not been measured. v2
records it in the boot word (bits 23..12), and the app reports it (DP 137, section 9). B2 reads
it over 20+ power cycles before the window is frozen. No answer comes while the ZTU is pausing
after failed updates (`image_info` returns 0, item 41): the probe then costs the full window.

**ZTU change: none needed, none advisable.** Every own ZTU build since phase 1 answers the
request above. A new "image info" command would only work with ZTUs not yet built, and v2 would
still need this request as a fallback: more frozen code for nothing. The ZTU's UART cannot start
earlier, because its pins come from the parsed config. For the probe, the phase-3 ZTU only adds
tests (section 10).

**Light delay after a power cut** (*measured* in the prototype, nothing newer carried):

| ZTU ready at (ms) | 0 | 100 | 300 | 600 | 1000 | 1400 | ≥1600 or silent |
|---|---|---|---|---|---|---|---|
| App starts at (s) | 0.00 | 0.10 | 0.30 | 0.60 | 1.00 | 1.40 | 1.50 |

With a newer build carried, the app started after the probe and the update (1.21 s with the ZTU
ready at 300 ms, 2.31 s at 1400 ms; the harness time scale is optimistic for the update,
section 8). The other probe cases all behaved as intended: same image, older build, a carried
2.1.12 without a build, both without a build, a 13 KB image over a 12 KB-ring app, a DIAG
installed (no request at all). A 13 KB image over a RING_HIGH app was taken, and so was build
301 over an installed 2.1.10.

---

## 6. Rescue mode (3 unconfirmed boots, or the button)

**Requests.** As an update: `ask(expect, min(left, room))` from offset 0, every 500 ms while
nothing comes, at once after each accepted block.

**What is taken.** Nothing is pinned; the installed app's CRC is skipped (`g_skip_crc`). That
means any image the ZTU serves with another CRC and a size ≤ `BOOT_APP_MAX`, **older builds
included**. Reasons:
- The counter fires only when the installed app demonstrably cannot run 30 s and answer the
  ZTU. Any other image the ZTU validated (magic, size, CRC, no bench flag) is at least as good a
  bet.
- The usual repair is to carry the last good image again (py32-bump with force). A newer-only
  rule would turn that into a re-release under a new build first.
- The button is the user's explicit request: it is the only way to go back to an older build
  without a new release, because the offer path and the probe take newer builds only.

The same CRC is skipped because re-installing it would rewrite nothing (identical pages are not
erased, `flash_drv.c:173-177`) and would end in the same crash.

**Window.** 30 s from the start, or from the last accepted block (then 10 s). With nothing new
the bootloader starts the app again. `tries` stays at 3, so the app's next unconfirmed reset
opens the next window.

**Why not stay listening for ever (the literal "stays listening")?**
- For an app that crashes or hangs in init, the effect is the same: *measured* 96 % of the time
  listening (12 cycles of a 1 s watchdog hang + 30 s window).
- A false trigger would otherwise darken a working light until the next power cut.
- A window of 30 s also covers a ZTU that reboots into a new image (~2-3 s) during a window.

**Power cuts.**
- Before any page is written: the next power-up is cold, tries is 0 and the app gets 3 fresh
  runs (plus the probe).
- After a page is written: no valid app, so UPDATE takes any image. *Measured*: 12 of 12 cut
  points recovered, with the ZTU then carrying a third image.

*Measured* crash loop (the installed build 302 confirms nothing; the ZTU carries 302, then from
boot 7 an older 299):
1. Boots 1-3 jump (tries 1, 2, 3).
2. Boots 4-6 open a 30.0 s window each (same CRC skipped) and then jump.
3. Boot 7 installs 299 in 1.2 s; the new app runs with tries 1 and confirms.

Button runs:
- ZTU carries an older image: installed in 1.3 s; on the next boot, with the button still held
  but warm, the app starts at once.
- Same image carried: the app started after 30.1 s.
- Not held: no delay.

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
- The answered 0xA3 shows that UART RX/TX, framing and the identity handler work. Every own
  ZTU asks for it after each MCU start: the first query with seq 0 counts as a restart
  (`tuya_mcu_app.c:208-221, 316-327`), then come the version question and 4 identity questions
  in 7.5 s, and 2 more at later network queries if all were lost (`tuya_mcu_ota.h:4-14`,
  `tuya_mcu_ota.c:162-196`).
- The ZTU makes its offer right after that answer, so a confirmed app has already had its chance
  to take a newer image.
- The 30 s covers the start-up work: the first settings save (2 s after a change), the DP dump,
  the zero-cross lock and the first fades.
- A crash after 30 s needs no bootloader rescue: the app lives long enough to accept an offer.

No fallback "confirm after N minutes without 0xA3". A stable app is never pushed into rescue
anyway (the counter moves only at resets). Without the fallback, an app with a broken UART that
also resets now and then is still rescued.

The only cost lands on a module under a ZTU that never asks 0xA3 (a ZTU without own-MCU
support, which STATUS already forbids): every third warm reset gives a 30 s dark window.

**CONFIRMS flag.**
- v2 apps set it in every build.
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
| Progress-based give-up (item 28) | `progress = g_ms` at the start and at every accepted block. When `g_ms - progress > window`: if `app_valid()` start it, else drop the pin and skip (`g_expect_crc = g_skip_crc = 0`) and continue with 10 s windows. The `erased` flag stays only as an SWD debug field. | v1 harness: image change mid-transfer stuck after 300 s (63 214 requests); silence after an identical page 0 stuck after 120 s. *Measured* in v2: 11.7 s to the new image; 10.0 s back to the intact app. |
| Paced rejects | `on_block()` returns TOOK / STRAY / OTHER / DONE. TOOK: next request at once. STRAY (offset, length or duplicate): after 50 ms. OTHER (another CRC than the pinned one, the skipped CRC, size out of range): stays at the 500 ms pace. | Noisy link (5 % drop, 5 % late duplicate, 2 % bit flip): v1 0/50 failures, mean 6.0 s, max 9.9 s; v2 0/50, mean 7.2 s, max 10.6 s |
| Image change mid-transfer | Unpinned (rescue, no app): another size or CRC restarts from offset 0, as today (boot.c:248-250). Pinned: rejected until the give-up unpins, which happens only once the old app is gone. Otherwise the intact old app starts. | as above |
| CRC pinning | Asked: mailbox `expect_crc`. Probe: the probed CRC. Rescue: no pin, skip the installed CRC. No app: the mailbox CRC if asked, unpinned after 10 s without progress. | |
| Block size | Requests stay `min(left, room in page)` ≤ 128. Any 1..128-byte block that fits the page is taken. The ZTU stays at `BLOCK_MAX 64` (fork `tuya_mcu_ota.c:21`, `tuya_secondary_mcu.c:117`) until its main-loop blocking with 150-byte frames (~13 ms busy-wait per frame at 115200) has been measured; it can change later without SWD. | *Measured* with 128: 91 instead of 182 round trips, noisy mean 4.0 s |
| Flash driver | The `BOOTLOADER` SR-flag shim goes (`flash_drv.c:125-149, 180-184`): flags are cleared before each operation, as in the app since phase 2. Each page is still tried twice before `OTA_RESULT_FLASH`. A write-range guard comes from linker symbols (section 9). | `test_flash_drv`, built today with and without `BOOTLOADER`, then needs one variant plus the guard |
| Receive | After a bad checksum the buffered bytes are rescanned before more are read (item 39(4)) | +16 bytes |
| NOTIFY in the bootloader | Dropped (boot.c:314-319). The ZTU offers only after an identity answer (`tuya_mcu_ota.c:235-268`), and the bootloader never gives one. | |
| Watchdog | `tick()` refreshes in every loop. Longest step ~25 ms (section 4). | |

Power cut at every step:

| Cut during | After power returns | Outcome |
|---|---|---|
| Probe, or a window with nothing written | App intact; cold; tries 0 | Normal boot (and a probe) |
| Any page erase or program (asked, probe, rescue) | App CRC invalid; mailbox random, so nothing is pinned | UPDATE takes any image the ZTU serves. *Measured*: 162 of 162 cut points (81 differing pages × after erase / after program) recovered; 12 of 12 during a rescue. |
| After the last page, before the result | New app valid | Cold boot, probe finds the same build, jump |
| Between the app's UPDT write and the bootloader | Mailbox lost | Cold: the probe takes the image if it is newer; otherwise the old app runs and the ZTU offers again at first contact |
| Ring move (M's copy) | Old record intact | M copies again |

Real update time: the harness answers in 2-5 ms. A real round trip is about 1.3 ms request +
ZTU latency + 7.5 ms answer at 64 bytes, so a full update stays dark ~2-3 s on hardware (to
measure in B9), plus `app_valid()`.

---

## 9. App-side changes (ship over the air; *measured* +256 bytes on 2.1.12's 11 472)

1. **Header.** `{ APP_MAGIC, 0, 0, FW_VERSION, APP_FLAG_CONFIRMS | (RING_HIGH ? APP_FLAG_RING_HIGH
   : 0) | bench flags, FW_BUILD }` (`src/main.c:27-30`). `FW_BUILD` sits in `src/tuya.h` with a
   `make FW_BUILD=` override.
2. **Watchdog first.** `watchdog_start()` directly after the RCC flags are read and cleared
   (`main.c:188-192`), before `clock_init()`. The HardFault/NMI reset handler is already there
   (`main.c:180-185`).
3. **Confirmation** (section 7). The 0xA3 handler sets `g_ident_answered`, and the main loop
   calls `boot_confirm()` once both conditions hold.
4. **Identity v2** (10 bytes, section 2).
5. **Offer rule** (section 2), with `app_limit()` from `_settings_start` and `boot_id`, and the
   lower bound aligned with the bootloader (`size >= APP_HEADER_OFFSET + sizeof(app_header_t)`,
   item 42).
6. **Bootloader identity.**
   - DP 136: CRC-32 of the whole 4 KB boot sector, computed once at start (~7 ms). The released
     v1 bootloader (2136 bytes, 0xFF-padded) gives **0xEAC45031**; each v2 build's value goes
     into STATUS.md with the release.
   - DP 137: the boot word at start, low 24 bits (probe time, rescue info, tries).
   - Both are sent with the health report (`src/health.c:34-41`, first at 60 s). Like DPs
     130..135 they are only logged by the ZTU's MCU link diagnostics: no converter change. The
     bootloader version also goes out in identity byte 9.
7. **Flash write-range guard.** `flash_write_page()` refuses addresses outside
   `[_flash_write_start, _flash_write_end)`: the settings region in the app linker script, the
   app area in `boot.ld`. That replaces the `#ifdef BOOTLOADER` in `src/flash_drv.c`.
8. **Later, release M only:** the ring move of section 1.

All of this works under the v1 bootloader (compatibility matrix). These changes ship in the same
release as v2, because the full image that `install.sh` writes must contain a CONFIRMS app. They
may also go to the spare earlier, over the air.

---

## 10. ZTU-side changes (fork, phase-3 ZTU release; over Zigbee)

The phase-3 ZTU release must stay correct with the v1 bootloader on the spare and with stock
MCUs.

1. **`on_ident`** (`tuya_mcu_ota.c:235-268`):
   - Identity `len >= 9`: the app's build = bytes 7-8 BE; offer when the carried header's build
     (image bytes 0xCE-0xCF LE) is greater.
   - Identity of 7 bytes: today's byte rule.
   - Store byte 9 (bootloader version; 0 when the identity is shorter) in `tuya_mcu_diag` and
     log it with the version.
   - `IDENT_LEN 7` stays the minimum; protocol 1 is unchanged.
2. **0xA0** gains the build: 11 bytes (section 2).
3. **`image_info`** (`tuya_mcu_ota.c:78-102`):
   - refuse flags & 0x07 (adds DIAG 0x04, as `embed_ztu.py` already does);
   - refuse a build in 1..255 or 0xFFFF;
   - `IMAGE_MAX` becomes 14 KB. This is safe with every MCU: 2.1.x apps decline more than 12 KB,
     v2 apps decline more than `app_limit()`, and the v1 bootloader never gets such an offer
     accepted.
4. **Serving rules unchanged.** Stateless 0xA1 for any offset while the image is good and not
   paused (failure back-off from phase 1, item 41); `off == 0` still means an MCU restart;
   `BLOCK_MAX` 64.
5. **Stock MCUs.** They are never asked for their identity (major 1, `tuya_mcu_ota.c:198`) and
   never send 0xA1, so nothing changes for them.
6. **Display.** `mcu_version` keeps the exact "x.y.z own" format. The converter parses it, and a
   changed format risks the 2026-11-17 class of entity loss. The build and bootloader version
   go to the link diagnostics only; a Basic attribute for them is a later, separate converter
   change.
7. **Tests** (`tests/test_mcu_ota.py`):
   - build-aware offers to a 10-byte identity;
   - byte rule for a 7-byte identity;
   - 11-byte 0xA0;
   - DIAG, build 0 and build 0xFFFF images neither offered nor served;
   - a 0xA1 at offset 0xC0 with max_len 16 answered with exactly the 16 header bytes and no
     first-contact reset;
   - the same request during the failure pause gets no answer;
   - a 14 KB image served whole.
8. **py32-bump** (fork workflow): compare builds instead of version strings. Refuse a carried
   build ≤ the one `home-dimmer` carries unless `force` (force is the rollback path of section
   6).

---

## 11. Tools and CI

| Tool | Change |
|---|---|
| `tools/mkimage.py` | Read and print the build. Refuse: build 0 or 1..255, 0xFFFF; 0xFFFE unless SIM; SIM without 0xFFFE; size > 12 KB without RING_HIGH; size > 14 KB; flags 0x20..0x80. Keep today's version-byte checks. `--full`: refuse a boot.bin without `boot_id` at 0x10 unless `--old-boot`. (That RING_HIGH is set exactly when `_settings_start` is 0x08004800 is a CI check on the ELF with `arm-none-eabi-nm`.) |
| `tools/embed_ztu.py` | All `assert`s become `sys.exit` checks (item 42), so `python3 -O` cannot skip them. Add: flags & 0x07; build 256..0xFFFD; SP == 0x20000BE0 and reset vector odd and inside the image (the stricter checks that stay out of the bootloader); size ≤ 14 KB and > 12 KB only with RING_HIGH. |
| `tools/release/release.sh` | `FW_BUILD` > the previous tag's (`git show <tag>:src/tuya.h`). Version byte > the previous tag's (tags are unique per byte). The first release over 12 KB is refused unless an earlier tag has RING_HIGH (the ring mover). The bootloader size check and the boot sector CRC (DP 136 value) go in the release notes. |
| `tools/swd/install.sh` | Show the image's bootloader version (boot_id) and boot sector CRC. Refuse a full image without boot_id unless `OLD_BOOT=1`, and one over 12 KB unless `ERASE_SETTINGS=1`. `IMAGE=release` checks `SHA256SUMS` (item 53). Optional `WRP=1` (section 1). Tools that halt the core must set DBGMCU `DBG_IWDG_STOP` first: v2 starts the watchdog at every boot (install.sh:124 and `make flash` already do). |
| `tools/ota_swd_emu.py` | Send the 11-byte 0xA0 (build) so that build-only test images are accepted. |
| `Makefile` | `FW_BUILD=` override (a new BUILD dir like `VERSION_BYTE`). `make test-boot`, part of `make test`. Report the bootloader size from the ELF; warn above 3584 bytes. |
| `.github/workflows/ci.yml` | The host test job runs `make test`, which includes `test-boot`: plain `cc`, no ARM toolchain or zlib, Linux ≥ 4.17 for `MAP_FIXED_NOREPLACE` (ubuntu-24.04 has it). The build job's size step adds the bootloader's text bytes against 4096. |

### `make test-boot` (tests/boot/, grown from the review harness)

**Build.**
- `boot/boot.c` is compiled unmodified with `#define main boot_main`, `-Wall -Wextra -Werror`.
- A fake `py32f0xx.h` routes USART1, SysTick and GPIOA through hook functions. RCC, IWDG and SCB
  are plain structs. `__set_MSP` and `NVIC_SystemReset` end the forked child.
- Flash (0x08000000, 20 KB), RAM (0x20000000, 3 KB) and the factory trim page (0x1FFF0000) are
  mapped at their chip addresses. Every boot is a `fork()`, so `.bss` is fresh as after
  `Reset_Handler`.

**Flash model** (a fake `flash_write_page()`):
- identical page → no erase;
- 4.5 ms per write;
- power-cut injection after the erase or after the program;
- asserts page alignment and the write range 0x08001000..0x080047FF;
- records the highest address written (for the ring-limit cases).

Its contract (returns 1 iff the page reads back equal; an identical page is not erased) is the
same list `tests/test_flash_drv.cpp` checks on the real `src/flash_drv.c` against
`tests/host/flash_model.hpp`. That is how the two stay in step.

**ZTU model.** A C port of `answer_block`:
- stateless; `BLOCK_MAX` 64 or 128;
- a ready time after power-up;
- failure counting and pause;
- serve-below-offset;
- image switch at page k;
- drop, late duplicate and bit-flip noise.

**Time and reset models.**
- 20 µs per SysTick read, 87 µs per UART byte, answers 2-5 ms after the request.
- `RCC_CSR` per boot: cold PWR|PIN with random RAM; warm SFT/IWDG; stale PWR after the
  bootloader's own reset.
- App model: confirms or not, crashes or not, clears the flags.

**Images.** Synthetic, from `tests/boot/mkimages.py`: a valid vector table, header and CRC,
chosen builds and flags, and controlled identical pages (for example page 0). No SDK needed.

**Scenarios** (each with a pass condition; the prototype passed all of them):

| Group | Scenarios |
|---|---|
| Update | Asked normal (done; next boot tries 1, no probe); asked + ZTU silent (back to the app after 10 s); power cut after erase and after program at every page (all recover); image change mid-transfer (new image within ~12 s); ZTU answers only an identical page 0 (old app after 10 s); noisy link, 50 seeds (all succeed); 128-byte blocks; FLASH result path (a page that never verifies: ZTU sees 2, no tight loop) |
| Probe | Newer build taken, no second probe after its reset; same image, older build, old image without build, both without build: not taken; installed 2.1.x without build: taken; ZTU silent: app at the window; ZTU ready before / after the window; 13 KB over a 12 KB-ring app refused; over a RING_HIGH app taken; DIAG / MEASURE / SIM installed: no request at all; carried bench flag refused; wrong magic refused |
| Rescue | 3 unconfirmed boots then windows; same CRC skipped; older image taken; window length 30 s; power cut during a rescue transfer; duty cycle of an app that hangs in init |
| Button | Held at a cold boot: rescue, older taken; same image: app after 30 s; held at a warm boot: ignored; not held: no delay |
| Counter | 2.1.x app (no CONFIRMS) with 8+ watchdog resets: never a window; confirming app with resets between confirmations: never a window; asked update resets tries; written image resets tries; stale PWRRSTF + IWDG counts as warm |
| Mailbox | Random RAM at power-up: never asked, word rebuilt; restore words untouched by the bootloader (byte compare) |

CI runs it with every push. It is the gate before any SWD install on a module.

---

## 12. Size and RAM budget

*Measured* on the prototype: arm-none-eabi-gcc 13.2.1, `-Os`, the flags of the Makefile's
bootloader build. Each feature's cost is measured by switching it off alone and by adding it
alone to a build with all of them off.

| Part | Bytes |
|---|---|
| v1 as released | 2136 |
| Restructure (one loop for all modes, block results) | +36 |
| Watchdog start (LSI, start key) | +40 |
| Try counter, boot word, counter rescue | +88 (with the watchdog alone: +120) |
| Button | +72..76 |
| Power-on probe | +236 alone, +284 with the rest |
| Boot info + probe time in the boot word | +44 |
| Progress give-up | +16 |
| Paced rejects | +16 |
| Receive rescan | +16 |
| `boot_id_t` | +16 |
| LED slow / fast blink | +20 |
| SR-flag shim dropped, second page attempt kept | 0 |
| NOTIFY answer dropped | (in the restructure) |
| **Prototype total** | **2780** |
| Not in the prototype: bounded LSI wait, write-range guard | ~+30 |
| **Expected v2** | **~2810 of 4096 (1286 free)** |

The three user-requested paths (counter with watchdog, button, probe) take 464 bytes together,
against the ~300-byte estimate. The slot is the bootloader's alone, so the extra costs nothing
else. If the implementation must shrink, the order is: LED blink pattern (-20), boot info
(-44), button (-72). This design keeps all of them.

RAM: `.data` 48 bytes (the RAM flash routine), `.bss` ~496 bytes (RX buffer 150, injection
buffer 150, page buffer 128, state), stack 512. That is ~1.06 KB of the 3040 bytes below the
mailbox.

---

## 13. Failure modes: what could still need SWD

| # | Failure | v1 today | v2 | Residual |
|---|---|---|---|---|
| 1 | CRC-valid app crashes or hangs in init (item 0) | Endless loop, SWD | IWDG from the bootloader; 3 unconfirmed boots, then rescue windows (~96 % listening); the ZTU carries any other image | The ZTU must get another image (Zigbee OTA) |
| 2 | App runs, but its update path is broken (RX, parser, 0xA3/0xA0, version compare, mailbox) | SWD | Power cut, then the probe takes a newer build; or the button | Needs a power cut or the button, and a ZTU answer within the probe window |
| 3 | App runs but misbehaves; a rollback is wanted | Re-release as newer | Re-release under a new build (cheap now), or the button with the older image carried | - |
| 4 | Power cut during any update page | Recovers (review: 182 cases) | Recovers (162 + 12 cut points) | - |
| 5 | ZTU image changes mid-transfer (item 28) | Dark until a power cut | Unpinned after 10 s, new image | - |
| 6 | ZTU silent after an identical page 0 (item 28) | Dark until a power cut | Old app after 10 s | - |
| 7 | ZTU pauses after failed results (item 41) | Dark during the pause (if the app is gone) | Same | Up to 24 h dark in the worst back-off. A ZTU restart (mains cycle) clears the pause. |
| 8 | ZTU carries a broken but valid image | Installed, then case 1 | Installed, then case 1; the same image is skipped in rescue | Needs a ZTU OTA with a fixed image |
| 9 | ZTU's own firmware broken (link dead) | MCU fine, no updates | Same | Fixed through the ZTU's Zigbee OTA; if that is broken too, the ZTU needs its own serial flashing (out of scope) |
| 10 | Image too large for the installed ring layout | Refused (12 KB) | Refused by the offer path and the probe; taken by rescue | Rescue may overwrite low-half settings (no code damage) |
| 11 | Version space runs out (item 31) | Byte 0xFF ends OTA | Build numbers 256..0xFFFD; the byte bumps only for releases | - |
| 12 | Bench image carried by mistake | ZTU refuses SIM / MEASURE | ZTU refuses all three; the probe refuses the flags | - |
| 13 | App writes into sector 0 by a bug | Bootloader gone, SWD | Range guard in the app's flash driver; WRP available | Two bugs needed; WRP off (section 1) |
| 14 | LSI never ready | - (the app would hang in `watchdog_start`) | Bounded wait, then the start key anyway | Hardware fault |
| 15 | Button stuck pressed | - | Each power-up: one 30 s window, then the app (which ignores a closed input) | 30 s delay per power cut |
| 16 | ZTU without own-MCU support above an Open Dimmer MCU | Never updated | Same, plus a 30 s window every third warm reset | Forbidden by the migration order (STATUS) |
| 17 | Defect in v2 itself | - | Harness gate (section 11), spare trial (section 14) | Frozen: SWD, or a boot-updater app (~150 ms brick window) while WRP is off |
| 18 | Brown-outs in a row | Each boots the app | Each is a cold boot with one probe (the ZTU usually answers at once if it did not reset) | ≤ window delay per dip |
| 19 | Flash page that never verifies (worn or damaged cell) | FLASH results, ZTU pause | Same | Hardware: SWD cannot fix it either |
| 20 | Wrong assumption about the reset flags (B8) | - | Worst case: a cold boot read as warm skips one probe and keeps counting tries | Verified on the spare before the modules |

---

## 14. Rollout

1. **Implementation** on a branch from this design:
   - `boot/boot.c`, `boot/boot.ld`, `src/ota_proto.h`, `src/flash_drv.c`;
   - the app changes of section 9;
   - `tests/boot/` and `make test-boot`;
   - the tools of section 11; STATUS.md; an INSTALL.md section on the button rescue.

   CI green, then a review of the bootloader diff against this document.
2. **ZTU phase-3 release** (section 10). OTA to the spare's ZTU first: the spare still has v1,
   and that combination must work. Then the other modules' ZTUs one at a time: with their stock
   MCUs nothing changes, and the check is the same as phase 1.
3. **Release** vX.Y.Z with v2 and the CONFIRMS app. Record the boot sector CRC.
4. **Spare** (mains off, probe on CN2): `IMAGE=<that release's zdms16_py32_full.bin>
   tools/swd/install.sh`. This also exercises install.sh's re-flash path (item 8 / STATUS
   hardware gate). The settings in sector 4 stay untouched.
5. **Bench tests on the spare** (mains rules apply; the button and SWD steps with mains off):

| # | Test | Pass |
|---|---|---|
| B1 | After the install: read-back equal; DP 136 = the release's CRC; ZTU log shows bootloader version 2 | as stated |
| B2 | 20+ mains power cycles, nothing newer carried: light delay (timed), DP 137 probe time each run | Delay = probe time + app start; no update. **Then freeze `PROBE_WINDOW_MS`** (section 4); if it changes, rebuild, re-flash, repeat B1-B2. |
| B3 | Broken update path: a test build that declines every offer (SWD), ZTU carries a newer build | The offer is declined (ZTU log); after a power cut the probe installs the newer build |
| B4 | Crash loop: a test build that faults 1 s after start (SWD), ZTU carries the release | 3 crashes, then a window installs the release; DP 137 shows RESCUE |
| B5 | Button held through power-up; carried image different / same | Different: installed, DP 137 BUTTON. Same: app after ~30 s. Never a rejoin or factory reset. |
| B6 | 2.1.12 app under v2 (full image from the v2.1.12 app and v2 boot), watchdog resets via a halt without `DBG_IWDG_STOP` | No window; a power cut lets the probe take the carried build |
| B7 | Mains cut at ~50 % of a probe-started update (fast LED blink), 3 times | Each recovers on the next power-up |
| B8 | Reset flags: DP 131 after a power cycle that follows a software reset; IWDG running or not after `NVIC_SystemReset` (SWD) | PWR only (cold); IWDG result recorded in STATUS |
| B9 | Normal offered OTA with both channels lit | Light restored; dark time measured |
| B10 | 1-2 weeks trial: health DPs 131-137 | No unexpected resets; DP 137 shows tries 1 at every start (each run before it confirmed) |

6. **Further modules**, each at its migration slot (STATUS migration order): install.sh stock
   path with the same release full image, then record DP 136 and 137 per module. From then on
   their bootloader never changes. App and ZTU updates go over Zigbee, the spare first, as the
   process rule says.
7. **Later, only when the app approaches 12 KB:** the ring-move release M (section 1), spread
   and verified everywhere, before any larger app.

**Open measurement before freezing v2:** the ZTU's answer time after a power-up (B2) sets
`PROBE_WINDOW_MS`.
