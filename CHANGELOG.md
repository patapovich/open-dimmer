# Changelog

Short, user-facing summary per release. The release notes on GitHub are taken from here
(`tools/changelog.py`); a release without a section for its version is refused.

## 2.2.0 (2026-11-19)

- Up to 14 KB (after 2.1.14's ring move; needs the v2 bootloader on the module).
- Auto-off: a countdown (DP 6 / 12, s) written while on turns the channel off after that long;
  an off cancels it.
- Wall switch mode per channel (DP 162 / 163): normal, detached (the light is the network's, the
  switch only reports its gestures; a toggle or state switch's change as a single) or off.
- Level after a power cut per channel (DP 164 / 165, 0 = the last level).
- Identify (DP 166 / 167, s): the channel blinks, then is as it was.
- Start boost per channel (DP 168 / 169, ms up to 750): coming on from dark at full that long,
  then the level (for LED drivers that need a kick).
- Fast press per channel (DP 170 / 171): in the detached mode a momentary switch's press is
  reported at once (for direct control of other devices), hold and release as before, no
  double / triple on that channel.
- Health: the MCU die temperature (DP 138, degC) and the chip's factory calibration of its
  sensor (DP 139), so the spread between chips can be seen. No protection is built on it yet.

## 2.1.14 (2026-11-19)

- Transition time: a Zigbee transition (Home Assistant's `transition`) makes the change take
  exactly that long at one even rate, on, off and level alike (DPs 160 / 161, with a Zigbee
  firmware that sends them); without one the fade settings apply as before.
- The settings ring moves to the sector's upper half (2 KB), so later versions can be up to
  14 KB: the settings are found where they were and copied up once; health DP 137 bit 25 says
  when that is done (bit 24: the new layout).
- Built with link-time optimization: 384 bytes smaller, same behaviour.

## 2.1.13 (2026-11-19)

- Bootloader v2 (build 300 of the app, the first with a build number). It is installed over SWD
  only (`tools/swd/install.sh`); on the old bootloader this app runs as before, without the
  points marked v2.
- v2: at power-on the module takes a newer build the Zigbee firmware carries, even if the
  running app cannot take updates; the light comes as soon as the Zigbee firmware answers, after
  ~3 s if it does not.
- v2: an app that keeps crashing (3 starts in a row that never confirm themselves) gets 30 s
  windows in which any other carried image is installed, older ones included. Holding the
  module button while the power comes (pressed through the insulated case, never opened on
  mains) forces such a window.
- v2: link watchdog. If the Zigbee firmware sends nothing for 30 min, the app restarts (at
  most every ~4 h, only after every channel has been off and no switch used for a minute) and
  checks for a newer build; the light state stays, the wall switch is not read for up to ~3 s
  (longer while a newer build is being installed).
- Updates go by build number with a Zigbee firmware that knows it, by version byte otherwise.
- Health: DP 136 (bootloader sector CRC) and DP 137 (boot word: probe time, rescue, button,
  written, stuck and link flags, tries).

## 2.1.12 (2026-11-18)

- After a watchdog, fault or update restart the light comes back as it was (on/off and level);
  only a second such restart within a minute, or a real power cut, follows the power-on setting.
- Wall switch hold: a network "on" no longer ends a hold, a hold ends by itself if the input
  sticks, the level a hold picks is saved, the first hold after a start goes down from a bright
  level.
- Min and max writes keep the level inside the range.
- Zero-cross tracker: the acceptance window applies while locking too and is narrower once
  locked, so a noise pulse cannot start conduction at the wrong moment.
- Settings: one flash page per save attempt; a worn page is skipped instead of blocking saves.
- Health: UART receive losses as DP 135, wider counters, uptime in minutes (no wrap).
- Diagnostics and measurement builds no longer get replaced by an update offer.
- 928 bytes less flash (own memset/memcpy, unsigned maths), same behaviour.

## 2.1.11 (2026-11-18)

- Channels without saved settings start with the `led_soft` dimming curve.

## 2.1.10 (2026-11-17)

- Fade and hold times in milliseconds stay right on 60 Hz mains (rates follow the measured
  half period).

## 2.1.9 (2026-11-17)

- Hold slows down near the minimum only when going down or when it started from dark.
- Channels without saved settings start with the `led` dimming curve; health report every
  2 minutes.

## 2.1.8 and earlier (2026-11-17)

- First releases: phase control with curves (linear, led, led_soft, incandescent, led_deep,
  led_deeper), min/max as physical limits, per-channel power-on, fade and hold settings, wall
  switch gestures, module button for both channels, health report, update over Zigbee through
  the Zigbee firmware.
