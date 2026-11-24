# Features and settings

Everything the AVATTO ZDMS16-2 does with Open Dimmer and the fork's Zigbee firmware, and every
setting Zigbee2MQTT shows for it, with its values and default. This page describes Open Dimmer
**2.2.8** and the Zigbee firmware that carries it (build `1.1.3-d903f5ea` and later). How to
install: [INSTALL.md](INSTALL.md).

The names are Zigbee2MQTT's. A setting **per channel** exists once for each channel: Zigbee2MQTT
shows it on the endpoints `l1` and `l2`, and MQTT and Home Assistant name it `<name>_l1` and
`<name>_l2` (for example `fade_on_time_l1`). The others are **per device**.

The **Needs** column says what a feature needs:

| Needs | Meaning |
|---|---|
| Zigbee firmware | the fork's Zigbee firmware; works with the factory dimmer chip too |
| Open Dimmer | Open Dimmer on the dimmer chip, 2.2.6 or later; what came with 2.2.7 is marked in the text |
| 2.2.8 | Open Dimmer 2.2.8 or later. A module whose dimmer chip still has bootloader v2 keeps the Open Dimmer version it has until it gets v3 ([INSTALL.md, Updates](INSTALL.md#updates)) |

Zigbee2MQTT shows the Open Dimmer settings once it has identified the dimmer chip as Open Dimmer
(`mcu_firmware`); the device option `open_dimmer_settings` changes that ([Device
options](#zigbee2mqtt-device-options)).

## Contents

- [At a glance](#at-a-glance)
- [Light and brightness](#light-and-brightness)
- [Transitions and fades](#transitions-and-fades)
- [Dimming curves and lamp fitting](#dimming-curves-and-lamp-fitting)
- [Wall switch](#wall-switch)
- [Gestures (actions)](#gestures-actions)
- [Direct control (bindings)](#direct-control-bindings)
- [Scenes and groups](#scenes-and-groups)
- [Power cuts and restarts](#power-cuts-and-restarts)
- [Auto-off and identify](#auto-off-and-identify)
- [Network](#network)
- [Updates and install](#updates-and-install)
- [Diagnostics](#diagnostics)
- [Zigbee2MQTT device options](#zigbee2mqtt-device-options)
- [Device config](#device-config)
- [With the factory dimmer chip](#with-the-factory-dimmer-chip)
- [Recipes](#recipes)

## At a glance

| Area | Feature | Setting or command | Needs |
|---|---|---|---|
| Light | Two dimmable lights | `state`, `brightness` | Zigbee firmware |
| Light | The lamp's range, the slider spread over it | `min_level`, `max_level` | Zigbee firmware |
| Light | The level the light comes on at | `on_level`, `on_level_local` | Zigbee firmware; `on_level_local`: Open Dimmer |
| Light | A brightness set while off, kept for the next on | `execute_if_off` | Zigbee firmware |
| Light | Remotes that dim by move, step and stop | Level Control commands | Zigbee firmware |
| Fades | Exact transition times | `transition`, `brightness_fade_time` | Open Dimmer |
| Fades | Fade on and off, a slow end of the fade off | `fade_on_time`, `fade_off_time`, `fade_off_slow_zone` | Open Dimmer |
| Curves | Dimming curves for LED and incandescent lamps | `dimming_curve` | Open Dimmer |
| Curves | A curve measured for the lamp | `custom_curve` | 2.2.8 |
| Curves | Lamp compensation | `lamp_compensation` | 2.2.8 |
| Curves | Start boost for LED drivers | `start_boost` | Open Dimmer |
| Wall switch | Toggle, state or momentary switch | `switch_type` | Zigbee firmware |
| Wall switch | Hold dimming speed and its slow zone | `hold_time`, `hold_slow_zone` | Open Dimmer |
| Wall switch | The hold learns which way you want | – | Open Dimmer |
| Wall switch | A long press from off rises from the minimum | `long_press_from_off` | 2.2.8 |
| Wall switch | Double tap to full brightness | `double_tap_full` | 2.2.8 |
| Wall switch | Normal, detached or off | `switch_mode` | Open Dimmer |
| Gestures | Single, double, triple, hold, release as actions | `action` | Open Dimmer |
| Gestures | A press reported at once | `fast_press` | Open Dimmer |
| Direct control | Bound lights and groups follow the wall switch | Bind, source endpoint 3 / 4 | Zigbee firmware; detached mode: Open Dimmer |
| Direct control | What goes to the bound lights | `bind_mode`, `double_press_action`, `triple_press_action` | Open Dimmer |
| Scenes | Zigbee Scenes, 16 per module | `scene_store`, `scene_recall`, ... | Zigbee firmware |
| Groups | Zigbee groups | Zigbee2MQTT groups | Zigbee firmware |
| Power | Power-on behaviour | `power_on_behavior` | Zigbee firmware (both channels together); per channel: Open Dimmer |
| Power | Power-on level | `current_level_startup` | Open Dimmer |
| Power | Back as it was after a restart | – | Open Dimmer |
| Timers | Auto-off | `countdown` | Open Dimmer |
| Timers | Identify | `identify` | Open Dimmer |
| Network | Network reset by wall switch presses | `multi_press_reset_count` | 2.2.8 (factory chip: a fixed 8) |
| Network | Module button | – | Open Dimmer |
| Network | Conversion from the stock firmware keeps the network | `factory_identity` | Zigbee firmware |
| Updates | Zigbee firmware over the air | Zigbee2MQTT OTA | Zigbee firmware |
| Updates | Open Dimmer installed and updated by the Zigbee firmware | `mcu_flasher`, ... | Zigbee firmware and the two wires |
| Updates | A failed update repairs itself; button rescue | – | Open Dimmer |
| Diagnostics | Versions, health of the dimmer chip | `mcu_*` | Zigbee firmware; health: Open Dimmer |

## Light and brightness

Each channel is a dimmable light (`state`, `brightness`). Brightness 1..254 is spread over the
channel's `min_level`..`max_level`, so the whole slider does something.

| Setting | Per | Values | Default | What it does |
|---|---|---|---|---|
| `min_level` | channel | 1..254 | 51 | The lowest output, where the lamp still gives steady light. Brightness 1 is this. Kept in the dimmer chip. With Open Dimmer it is a physical limit, the same for every curve. A change keeps the current level inside the range. |
| `max_level` | channel | 1..254 | 254 | The highest output. Brightness 254 is this. |
| `on_level` | channel | 1..255 | 255 | The brightness an On command and the wall switch bring the light on at. 255: the level it had. |
| `on_level_local` | channel | 1..255 | 255 | The brightness the wall switch brings the light on at, kept apart from `on_level`. 255: as `on_level`. |
| `execute_if_off` | channel | true, false | false | true: a brightness change without on/off while the light is off sets the brightness it comes on at, and the light stays off. false: such a change is ignored while off. |

- Both on levels are kept by the module, so the wall switch needs no round trip through the
  network to use them.
- The wall switch turned on within 5 s of the light's own off (tapped off and on again) brings
  back the level the light had, whatever `on_level` says. After an off from the network the on
  level applies as usual.
- Remotes that dim by holding a button (Move, Step, Stop) are followed at the remote's rate; Open
  Dimmer moves smoothly between the remote's steps. A Stop stops the light where it is, also in
  the middle of a transition.

## Transitions and fades

| Setting | Per | Values | Default | What it does |
|---|---|---|---|---|
| `fade_on_time` | channel | 0..10 s | 0.95 s | Fade from dark to full when switched on; a lower level takes its share of it. Also a plain On from the network (no brightness). |
| `fade_off_time` | channel | 0..10 s | 0.95 s | Fade from full to dark when switched off. |
| `fade_off_slow_zone` | channel | 0..100 % | 15 % | Switching off, the fade slows down to an even end in this lowest part of the channel's `min_level`..`max_level`. The light then stays at the minimum for 80 ms and goes dark. Before 2.2.8: a share of the whole output scale, default 12 %. |
| `brightness_fade_time` | channel | 0..60 s, preset `default` | 2 s | Fade of brightness changes from the network while the light is on, as the time over the whole range: a smaller change is quicker. 2 s is the module's own default. |

A **transition time** (Home Assistant's `transition`, or the device option `transition`) wins:
the change takes exactly that long at one even rate, on, off and level alike. 0 is at once. The
device option does not apply to a plain on (no brightness), which takes `fade_on_time`.
Without one the fades above apply: `fade_on_time` from dark, `fade_off_time` to off,
`brightness_fade_time` while lit.

The wall switch, the module button and the auto-off always take the fades above, never a
transition time from the network (2.2.7).

A change of brightness during a running fade (a slider dragged) is timed from where the light
is, so the speed stays even.

## Dimming curves and lamp fitting

| Setting | Per | Values | Default | What it does |
|---|---|---|---|---|
| `dimming_curve` | channel | see below | `led_soft` | How the level turns into output. |
| `custom_curve` | channel | 17 numbers | empty | The table of the `custom` curve. |
| `lamp_compensation` | channel | 0..63 | 0 (off) | For LED lamps that are brighter dimmed down than switched on at the same level. |
| `start_boost` | channel | 0..0.75 s | 0 (none) | Coming on from dark, full output this long, then the brightness. |

The curves:

| `dimming_curve` | For |
|---|---|
| `led_soft` | gentle; suits most LED lamps (the default) |
| `led` | even perceived steps (CIE lightness) |
| `led_deep`, `led_deeper` | more room at the dim end, for LED lamps that get bright soon above their minimum |
| `incandescent` | filament and halogen lamps |
| `linear` | as the stock firmware: the conduction time follows the level |
| `custom` | a table measured for the lamp (`custom_curve`); acts as `led_soft` until the table is set (2.2.8) |

**Custom curve** (2.2.8): 17 outputs 0..10000 at 0, 1/16 .. 1 of the `min_level`..`max_level`
range, comma-separated; 0 first, 10000 last, never falling. `tools/custom_curve.py` measures a
lamp with an illuminance sensor or a light meter and writes it: [CUSTOM-CURVE.md](CUSTOM-CURVE.md).
The dimmer chip saves the table with its settings. Set `min_level` and `max_level` before you
measure. With the custom curve a hold of the lit light keeps its rate down to the minimum (the
curve is already even); `hold_slow_zone` then only slows the start of a hold from dark.

**Lamp compensation** (2.2.8): some LED lamps give more light at the same output after they have
been brighter, so the bottom of a dim-down never gets as dim as a start from dark. With the
compensation set, the output is lowered after the light came down: most at the minimum, less
higher up, none once the conduction is a tenth of the half cycle above the minimum's, and never
while it rises or right after it came on. The level shown does not change. The value is in steps
of the output scale (63 is about a quarter of it). To set it: dim down from bright to the
minimum and compare with the minimum switched on from dark; change the compensation step by step
(a higher value: darker after a dim-down) until the two look the same.

**Start boost**: for LED drivers that need a kick to start. Also before a hold from dark. A
transition time stays as it is. A hold from dark let go during the boost leaves the light at its
minimum (2.2.7). No boost when a restart of the dimmer chip brings the light back.

## Wall switch

Each channel has a wall switch input (S1, S2).

| Setting | Per | Values | Default | What it does |
|---|---|---|---|---|
| `switch_type` | channel | `toggle`, `state`, `momentary` | `toggle` | toggle: a rocker switch, every flip toggles. state: a rocker switch, its position is on or off. momentary: a push button, a press toggles and a hold dims. |
| `switch_mode` | channel | `normal`, `detached`, `off` | `normal` | normal: the switch switches and dims the light. detached: the light only follows the network, the switch only reports its gestures (and drives bound lights). off: the switch is ignored. |
| `hold_time` | channel | 0.1..10 s | 3.3 s | A hold of a momentary switch over the whole range. |
| `hold_slow_zone` | channel | 0..100 % | 25 % | A hold slows down in this lowest part of `min_level`..`max_level`, to pick a very dim level: going down, and rising from dark. Before 2.2.8: a share of the whole output scale, default 20 %. |
| `long_press_from_off` | channel | true, false | true | A long press while off: true, the light comes on at its minimum, waits there 0.7 s and rises while held, so it can be let go at any level from the lowest. false: it comes on at its level as for a tap, and the hold dims from there (2.2.8). |
| `double_tap_full` | channel | true, false | true | Two short taps of a momentary switch (normal mode) take the light to `max_level`, from off and from on. false: each tap toggles (2.2.8). |
| `fast_press` | channel | true, false | false | Detached mode, momentary switch: a press is reported at once as the action `press` ([Gestures](#gestures-actions)). |

**Momentary switch** (normal mode):

- A tap from off turns the light on at the press, to its on level. A tap while lit turns it off
  at the release.
- A hold (a press of 0.4 s or more) of the lit light dims it at `hold_time` over the range, slower
  in the slow zone. The release stops it at once and saves the level. During a hold the level is
  reported every 0.5 s, so Home Assistant and bound lights follow it.
- Which way a hold goes: the first hold goes towards the far end. A hold that starts within 2 s
  of the last one's end goes the other way (a correction). Any other hold takes the way learnt
  for the quarter of the range it starts in: a hold let go within 1.2 s and turned round at once
  went the wrong way, and that quarter learns the other one (a quarter that was just confirmed
  needs a second miss). A hold taken to the end counts as right. The learnt ways survive power
  cuts.
- A hold from off: see `long_press_from_off`.
- A double tap: the first tap acts as usual, the second (within 0.4 s of the first's release)
  takes the light to `max_level`. A tap followed by a hold still dims; a third tap is a plain tap
  again.
- A hold ends by itself after 2 × `hold_time` + 2 s, so a stuck switch cannot dim for ever.

**Toggle switch**: every change toggles. **State switch**: on while closed; set at once when
the type is changed to it, and when the mode goes back to normal. At every start of the dimmer
chip (normal mode) the light follows the state switch's position, whatever `power_on_behavior`
says.

- A momentary or toggle switch already closed at power-up is no press.
- The S1 input stays low about 0.1 s after a release, so a very fast double tap on it can merge
  into one press in hardware.

## Gestures (actions)

The wall switch's gestures come to Zigbee2MQTT as the device's `action`:

| `action` | When |
|---|---|
| `single_l1`, `double_l1`, `triple_l1` | one, two or three short presses, each started within 0.4 s of the last release; sent 0.4 s after the last release (more than three: nothing) |
| `hold_l1` | a press became a hold (0.4 s); taps just before it are dropped |
| `release_l1` | the end of a hold |
| `press_l1` | a press, at once (`fast_press` in detached mode) |

`_l2` for channel 2.

- A momentary switch reports its gestures in the normal and the detached mode. A toggle or state
  switch reports only in the detached mode, every change as `single`.
- In the normal mode the light acts on every press too. If an automation uses `double_l1`, set
  `double_tap_full` off, or the light also goes to full.
- With `fast_press` there is no single, double or triple on that channel; hold and release as
  before.
- The factory dimmer chip reports no gestures.

## Direct control (bindings)

Each channel's wall switch has an endpoint of its own: **3** for channel 1 and **4** for
channel 2 (Dimmer Switch, with On/Off, Level Control and Scenes clients). Lights and groups bound
to it follow the wall switch without the coordinator.

To bind in Zigbee2MQTT: the module's **Bind** tab, source endpoint 3 (or 4), the target device's
endpoint or a group, clusters `genOnOff` and `genLevelCtrl` (and `genScenes` for a scene
recall).

What the bound lights get:

| Wall switch | Normal mode | Detached mode (Open Dimmer) |
|---|---|---|
| on, off | On, Off | – |
| tap (single), `press` | – | Toggle |
| hold | one move in the hold's direction at its speed, then a stop and a move to the level the hold ended at | moves up and down by turns (the first up; a single starts again at up), 50 levels a second (or the channel's Level Control `defaultMoveRate`, if one was written) |
| release | – | Stop |
| double press | follows the light (to full with `double_tap_full`) | `double_press_action` (default: full) |
| triple press | – | `triple_press_action` (default: none) |

| Setting | Per | Values | Default | What it does |
|---|---|---|---|---|
| `bind_mode` | channel | `auto`, `off`, `mirror`, `remote` | `auto` | auto: by the wall switch mode, as in the table above. off: nothing. mirror: only following the light (the normal column), in any mode; in the detached mode the wall switch does not change the light, so only the module button is passed on. remote: only the gestures (the detached column), also in the normal mode, where the channel acts too; in the normal mode that needs a momentary switch (a toggle or state switch reports no gestures there). |
| `double_press_action` | channel | `none`, `full`, `off`, `scene_1`, `scene_2` | `full` | What a double press sends to the bound lights when the gestures drive them. scene_1 / scene_2: Recall Scene 1 / 2 of the group the Scenes binding goes to ([Scenes](#scenes-and-groups)). |
| `triple_press_action` | channel | as `double_press_action` | `none` | The same for a triple press. |

- On, Off, Toggle and the move to a level are sent with an acknowledgement and sent again if a
  bound device misses one; groups get them without. A move and a stop are sent once (a late
  repeat would move again).
- Only what the dimmer does by itself is passed on (the wall switch, the module button, a double
  tap to full; not the end of an auto-off), never a command from the network, so two modules
  bound to each other cannot loop.
- Nothing goes out without a binding. The light entities stay as they are.
- With the factory dimmer chip the normal mode works; `bind_mode` and the actions are not
  shown.

## Scenes and groups

Each channel (endpoints 1 and 2) can be in Zigbee groups and has the Zigbee Scenes cluster.
Zigbee2MQTT's scene commands work on a channel and on a group: `scene_store`, `scene_add`,
`scene_recall`, `scene_remove`, `scene_remove_all` (and the frontend's scene controls).

- A scene keeps each channel's on/off state and brightness.
- A recall fades in the scene's transition time. A scene without one (a new one stored with
  `scene_store`, or one added without `transition`) fades with the channel's own fade settings.
  A `scene_store` over an existing scene keeps that scene's time.
- 16 scenes per module, for both channels and all groups together, kept over power cuts.
- A group's scenes go when the channel leaves the group; all scenes go when the module leaves
  its network.
- If Zigbee2MQTT offers no scenes for the module, re-interview it once (a module interviewed
  before scenes existed).

Examples (MQTT; the same in the frontend):

```text
zigbee2mqtt/<group>/set        {"scene_store": {"ID": 1, "name": "Evening"}}
zigbee2mqtt/<group>/set        {"scene_recall": 1}
zigbee2mqtt/<device>/l1/set    {"scene_add": {"ID": 2, "name": "Night", "state": "ON", "brightness": 30, "transition": 3}}
```

**A scene from the wall switch**: bind the wall switch's endpoint (3 or 4) with `genScenes` to
a group, store scene 1 or 2 in that group, and set `double_press_action` (or
`triple_press_action`) to `scene_1` or `scene_2`. The gestures drive bound lights in the detached
mode (`bind_mode` auto) or with `bind_mode` remote ([Recipes](#recipes)). A Scenes binding to a
device instead of a group recalls that device's own scene (group 0).

## Power cuts and restarts

| Setting | Per | Values | Default | What it does |
|---|---|---|---|---|
| `power_on_behavior` | channel (Open Dimmer); device (factory chip) | `off`, `on`, `previous` | `off` | Whether the channel comes on when the power comes back. previous: as it was. |
| `current_level_startup` | channel | 0..255, presets `minimum` (0), `previous` (255) | `previous` | The brightness after a power cut: 1..254 that brightness, 0 the minimum, 255 the one it had. Whether it comes on is `power_on_behavior`. |

- After a restart of the dimmer chip alone (a watchdog reset, a fault, an update) the light comes
  back as it was, on/off and level. A second such restart within a minute, a power cut or a
  brown-out follow the power-on settings.
- After a restart of the module, it reports the channels only once they know their state, so a
  hub never sees a false off.
- The mains may be 40..70 Hz; the times follow the measured half period.

## Auto-off and identify

| Setting | Per | Values | Default | What it does |
|---|---|---|---|---|
| `countdown` | channel | 0..86400 s | 0 (none) | Written while on, the channel turns off after this long. Any off ends it (reported as 0). |
| `identify` | device | `identify` | – | Blinks the lamps of the module for 3 seconds, then they are as they were. |

- The Zigbee command On with timed off sets the countdown too; the longer time wins.
- A countdown survives a restart of the dimmer chip (the module writes the rest of the time
  again, or turns the light off if the time has passed); a power cut ends it.

## Network

| Setting | Per | Values | Default | What it does |
|---|---|---|---|---|
| `multi_press_reset_count` | device | 0..255 | 10 | That many closures of one wall switch, each within 10 s of the first, make the module leave its network and pair again; the switch's light blinks for 3 s. 0: never. 1..7 act as 8. |

- **Network reset from the wall** (2.2.8): any switch type, in the normal or the detached mode;
  never a switch whose `switch_mode` is off. A toggle switch counts its closures: 10 closures
  are about 20 flips. The factory dimmer chip counts its own fixed 8 and ignores the setting.
  Open Dimmer before 2.2.8 has no network reset from the wall switch (the module button only).
- **Module button**: a press toggles both channels (any on: all off). Held for 3 s or more, the
  module leaves its network and pairs again, except a hold that began in the first 10 s after a
  power-up (that one is taken for a late rescue press). Held while the power comes: the button
  rescue ([Updates and install](#updates-and-install)).
- **Conversion from the stock firmware** keeps the network: the module comes back in it after the
  conversion, no re-pairing. It also keeps what it was from the factory: `factory_identity`
  ([Diagnostics](#diagnostics)).

## Updates and install

- **Zigbee firmware**: over the air from Zigbee2MQTT, through the fork's OTA index and converter
  ([INSTALL.md, step 1](INSTALL.md#1-zigbee-firmware-over-the-air)).
- **Open Dimmer install**: two wires soldered once from the Zigbee module to the dimmer chip;
  the Zigbee firmware then installs Open Dimmer by itself at the next power-up
  ([INSTALL.md](INSTALL.md)). Without the wires: an SWD programmer
  ([INSTALL-PROGRAMMER.md](INSTALL-PROGRAMMER.md)).
- **Open Dimmer updates** come with the Zigbee firmware, which carries the newest release. The
  dimmer chip is updated while its light is off: 70 s after it was switched off (or after the
  Zigbee firmware started, if it was off then). A light that stays on is updated after a day
  anyway. The update takes a few seconds and keeps the light dark, then the light is as it was.
- **A failed update repairs itself**: an update cut short (a power cut) is fetched again. A
  version that fails to start three times in a row opens rescue windows in which another image
  the Zigbee firmware carries is installed, an older one too; the same image is not written
  again.
- **Bootloader v3**: 2.2.8 and later need it; a module that has v2 gets it over the two wires
  from Zigbee2MQTT ([INSTALL.md, Updates](INSTALL.md#updates)).
- **Button rescue**: hold the module button while the power comes, and the version the Zigbee
  firmware carries is installed if it differs from the one on the chip; with nothing to install
  the light starts after ~30 s ([INSTALL.md, Updates](INSTALL.md#updates)).
- **Link watchdog**: if the Zigbee firmware sends nothing for 30 minutes, the dimmer chip
  restarts and checks for a newer build: at most every ~4 hours, only after every channel has
  been off and no switch used for a minute. The light stays as it is.

The two-wire link has controls of its own in Zigbee2MQTT, shown with the device option
`mcu_flasher: show`:

| Setting | What it does |
|---|---|
| `mcu_flasher` | Starts a job: `survey_dp` (reads the debug port's id), `survey_full` (the chip's identity, option bytes and checksums; only reads), `update_bootloader` (writes the bootloader if it differs; `update_bootloader_force`: always), `install` (takes a factory chip to Open Dimmer: erases its firmware and settings), `abort`. |
| `mcu_flasher_confirm` | A writing job runs only if its name was written here within the last 60 s. |
| `mcu_flasher_status` | The state and result of the last job. |

## Diagnostics

| Name | What it shows | Needs |
|---|---|---|
| `mcu_firmware` | Open Dimmer or the factory (Tuya) firmware of the dimmer chip; while not Open Dimmer, also an install that waits for a power cycle, runs or failed | Zigbee firmware |
| `mcu_version` | The dimmer chip's firmware version | Zigbee firmware |
| `factory_identity` | What the module was from the factory: the manufacturer name of its factory Zigbee firmware and the factory dimmer chip's version (for example `_TZE284_jtbgusdc, MCU 1.0.7`) | Zigbee firmware |
| `mcu_uptime` | Time since the dimmer chip's last start | Open Dimmer |
| `mcu_reset_cause` | Why it last started: power (power-up or brown-out), software (an update or a fault), watchdog, pin | Open Dimmer |
| `mcu_zero_cross_losses` | Times it lost its lock on the mains zero cross since its start | Open Dimmer |
| `mcu_uart_rx_losses` | Bytes from the Zigbee module it lost since its start | Open Dimmer |
| `mcu_temperature` | The dimmer chip's die temperature, about ±10 °C absolute (nothing acts on it) | Open Dimmer |
| `mcu_settings_ring_moved` | Its settings are in the flash area of 2.1.14 and later, so larger versions can be installed | Open Dimmer |

The health values come a few seconds after a restart of either chip (2.2.7), then every 10
minutes.

## Zigbee2MQTT device options

| Option | Values | Default | What it does |
|---|---|---|---|
| `open_dimmer_settings` | `auto`, `show`, `hide` | `auto` | The Open Dimmer settings: auto, shown once the module has identified the dimmer chip as Open Dimmer; show, always; hide, never. Zigbee2MQTT rebuilds the entities when it restarts or this option changes. |
| `transition` | seconds | empty | A transition time for every brightness change and off from Zigbee2MQTT and Home Assistant; one in the message wins. A plain on still takes `fade_on_time`. Empty: the module's fades. 0: at once. |
| `mcu_flasher` | `hide`, `show` | `hide` | Shows the controls of the two-wire link ([Updates and install](#updates-and-install)). |

## Device config

`device_config` (with `device_config_ext` for its overflow) is the module's configuration
string, `jtbgusdc;TS0601-ZDMS16-2;DM2;M;SWD3D7;` on this module. Leave it as it is. `SWD3D7;`
is the two-wire link on the pads D3 and D7: the install and the `mcu_flasher` jobs need it. A
module whose config had no such token got it once with the update that made the link the
default; a token removed by hand later stays removed.

## With the factory dimmer chip

The Zigbee firmware runs with the factory dimmer chip too, before Open Dimmer is installed or
without it. Then it gives:

- the two lights with `min_level`, `max_level`, `on_level` and `switch_type`;
- one `power_on_behavior` for both channels;
- Zigbee Scenes and groups;
- direct control in the normal mode (bound lights follow on, off and hold);
- updates over the air and the conversion that keeps the network, `factory_identity`;
- `mcu_firmware` and `mcu_version`.

The factory chip fades by itself: transition times are not used. It reports no gestures and
resets the network at its own fixed 8 presses. `execute_if_off` works (the module keeps it),
but it is shown only with `open_dimmer_settings: show`.

## Recipes

**Night level from the wall switch.** Set `on_level_local` to a low level: the wall switch
brings the light on dim, while `on_level` (and so a hub's On) stays as it is. A double tap still
takes it to full.

**The wall switch drives other lights only.** Set `switch_mode` to detached: the channel's own
light follows only the network. Bind the wall switch's endpoint (3 or 4) to the lights or a group
(`genOnOff`, `genLevelCtrl`). A tap toggles them, a hold dims them, a double press takes them to
full. The gestures still come to Zigbee2MQTT as actions.

**The wall switch drives its own light and others.** Leave `switch_mode` normal and `bind_mode`
auto, and bind the wall switch's endpoint to the other lights: they follow its on, off and hold.

**A scene on a double press.** Make a group with the lights, set them as you want them and store
scene 1 in the group (`scene_store`, ID 1). Bind the wall switch's endpoint to the group with
`genOnOff`, `genLevelCtrl` and `genScenes`. With a momentary switch, set `bind_mode` remote
(the channel still acts on the presses; set `double_tap_full` off if its own light should not go
to full) and `double_press_action` to `scene_1`. A toggle or state switch reports no double
press, so this needs a momentary switch.

**An LED lamp that flickers or does not start low.** Raise `min_level` until the lowest level is
steady. A lamp that starts only from a brighter level: a small `start_boost` (0.05 s steps),
raised until it starts from dark every time. A lamp that gets
bright soon above its minimum: `led_deep` or `led_deeper`, or measure a `custom` curve. A lamp
whose dim-down ends brighter than its start from dark: `lamp_compensation`.
