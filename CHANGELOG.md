# Changelog

Short, user-facing summary per release. The release notes on GitHub are taken from here
(`tools/changelog.py`); a release without a section for its version is refused.

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
