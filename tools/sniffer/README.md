# Zigbee channel sniffer (ADALM-Pluto + GNU Radio)

`zbsniff.py` records one Zigbee channel around the clock into hourly pcap files. `zbpcap.py`
reads those files and says, for one node, what happened at the MAC level: its polls, the ACKs,
the frame pending bit and the retries. Written to watch a router and its sleepy end devices for
days without a laptop beside them.

The receiver is the O-QPSK chain of Bastian Bloessl's
[gr-ieee802-15-4](https://github.com/bastibl/gr-ieee802-15-4) (GPL-3.0) on
[GNU Radio](https://www.gnuradio.org/); `zbsniff.py` builds the flowgraph of its OQPSK PHY
example in Python. [Wireshark](https://www.wireshark.org/)'s tshark decodes and decrypts the
captures.

> [!CAUTION]
> **Captures hold key material.** A capture of a join (or a Trust Center rejoin) carries the
> network key, encrypted only with the well-known default Trust Center link key, so anyone with
> the file can read the key. Treat every capture like the key itself: keep it on the sniffer host
> (the service writes it 0640), do not attach it to issues or upload it. The key table file is a
> secret too. Sniff only your own network.

> [!WARNING]
> **The Pluto transmits after power-up.** It boots with its TX path live (TX LO at 2450 MHz,
> -10 dB). `zbsniff.py` turns TX off before it starts receiving. See [Gotchas](#gotchas).

## Hardware

- ADALM-Pluto (AD9363). Tested with Pluto firmware v0.30.
- A USB 2 port, and a USB extension cable: the Pluto should sit 1-2 m from the devices under
  test.
- A Linux host. Tested on Debian 13 (also inside an LXC container) with GNU Radio 3.10.12.
  At 4 MS/s it uses about 18 % of one CPU core. A channel with ~300 frames/min writes about
  30 MB/day; gzipped hours are 30 KB to 1 MB.

## Install

Packages (Debian 13):

```sh
sudo apt install gnuradio gnuradio-dev cmake g++ git pkg-config pybind11-dev libiio-utils tshark
```

tshark asks whether non-root users may capture packets: answer no (only files are read).

gr-ieee802-15-4, branch `maint-3.10`, tested at commit `a210b11` (gr-foo is not needed):

```sh
git clone -b maint-3.10 https://github.com/bastibl/gr-ieee802-15-4.git
cd gr-ieee802-15-4
git checkout a210b11
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
sudo cmake --install build
sudo ldconfig
python3 -c 'from gnuradio import ieee802_15_4'
```

If the import fails, add the directory where the install put `ieee802_15_4` to `PYTHONPATH`
(in the unit too: `Environment=PYTHONPATH=...`).

User, directories, scripts, udev rule and unit:

```sh
sudo useradd --system --home-dir /var/lib/zbsniff --shell /usr/sbin/nologin zbsniff
sudo install -d -o zbsniff -g zbsniff -m 750 /var/lib/zbsniff /var/lib/zbsniff/pcap
sudo install -d -m 755 /opt/zbsniff /etc/zbsniff
sudo install -m 755 zbsniff.py zbpcap.py /opt/zbsniff/
sudo install -m 644 53-adalm-pluto.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
sudo install -m 644 zbsniff.service /etc/systemd/system/
sudo systemctl daemon-reload
```

Plug the Pluto in (or replug it), then check it as the service user:

```sh
iio_info -s
sudo -u zbsniff iio_attr -u usb: -c ad9361-phy altvoltage0 frequency
```

The udev rule gives the group `zbsniff` access to the Pluto's USB device node (libiio's `usb:`
backend opens it). Use another group by changing the rule, the user and the unit together.

**LXC container.** Put the rule on the host with `MODE="0666"` (the container's group ids are not
the host's; this gives every local user on the host access to the Pluto). Then allow the USB
character devices and bind the USB bus into the container, e.g. in Proxmox's
`/etc/pve/lxc/<id>.conf`:

```
lxc.cgroup2.devices.allow: c 189:* rwm
lxc.mount.entry: /dev/bus/usb dev/bus/usb none bind,optional,create=dir
```

Restart the container after the change.

## Running it

Settings come from the environment or from `KEY=VALUE` lines (no quotes) in
`/etc/zbsniff/zbsniff.conf` (another file: `ZBSNIFF_CONF`). The unit reads the same file. All
are optional:

| Key | Meaning | Default |
| --- | --- | --- |
| `CHANNEL` | Zigbee channel, 11-26 | `11` |
| `GAIN` | RX gain in dB (manual), or `slow_attack`, `fast_attack`, `hybrid` (AGC) | `35` |
| `URI` | libiio context of the Pluto (`iio_info -s` lists them) | `usb:` |
| `DIR` | output directory | `/var/lib/zbsniff/pcap` |
| `KEEP_DAYS` | delete hour files older than this | `7` |
| `SAMP_RATE` | sample rate, a multiple of 4000000 (the receiver runs at 4 MS/s) | `4000000` |
| `LO_OFFSET` | LO distance from the channel centre in Hz; needs `SAMP_RATE=8000000` (then at most +-2850000) | `0` |
| `BUFFER` | samples per libiio buffer; the time stamps' granularity | `8192` |

Bad settings are refused at the start, before the radio is touched: `zbsniff.py` logs a
`bad setting` line and exits 78, and the unit does not restart it.

Example `/etc/zbsniff/zbsniff.conf`:

```
CHANNEL=15
GAIN=35
KEEP_DAYS=7
```

**Pick the gain first.** With the service stopped (libiio serves one client per Pluto):

```sh
sudo systemctl stop zbsniff
sudo -u zbsniff python3 /opt/zbsniff/zbsniff.py --gains 15,25,35,45 --secs 60
```

It reads the channel from the settings, prints the frames and the FCS-OK frames per gain and
writes no files. Take a gain with a high FCS-OK count. Too much gain saturates the receiver near
the devices: tested at 1-2 m, 15-40 dB gave ~99 % FCS OK, 50 dB saturated.

The sweep leaves the RX LO running on the channel centre, and an LO on the channel can disturb a
device right beside it (see [Gotchas](#gotchas)). Right after the sweep, start the service
(below), or power both LOs down as the unit does when it stops (with another `URI`, use it in
place of `usb:`):

```sh
sudo -u zbsniff iio_attr -u usb: -c ad9361-phy altvoltage0 powerdown 1
sudo -u zbsniff iio_attr -u usb: -c ad9361-phy altvoltage1 powerdown 1
```

Start it:

```sh
sudo systemctl enable --now zbsniff
journalctl -u zbsniff -f
```

What it writes in `DIR`:

- `zb-YYYYmmdd-HH.pcap`: the current hour (local time), LINKTYPE 195 (IEEE 802.15.4 with FCS).
  Frames with a bad FCS are kept; Wireshark marks them. After a restart the hour's file is
  continued; a torn last record is cut off first.
- `zb-YYYYmmdd-HH.pcap.gz`: finished hours. Wireshark and tshark read them as they are.
- Files older than `KEEP_DAYS` are deleted. `keep/` is never touched: hard-link the files of an
  incident there to keep them (`ln DIR/zb-20260131-06.pcap.gz DIR/keep/`). They use disk until
  you remove them.
- `status.json`, every minute: frames, FCS OK and bad since the start, the last and the current
  minute's counts, the time of the last frame, the current file, channel and gain.

Time stamps are the host's clock when a frame leaves the demodulator: a few ms late and quantized
to the buffer (`BUFFER / SAMP_RATE` = 2 ms). The order is exact.

Without a frame for 10 minutes `zbsniff.py` exits and systemd restarts it (a busy channel is
never that quiet; the radio is gone). On a very quiet channel this restart costs some 30 s of
capture now and then.

## Network key

tshark needs the network key to decrypt the NWK and APS layers. It reads it from Wireshark's key
table `zigbee_pc_keys` in its configuration directory. `zbpcap.py` points tshark at
`WIRESHARK_CONFIG_DIR` (default `/etc/zbsniff/wireshark`).

```sh
sudo ./set_key.sh
```

It asks for the key without echo and writes the table in both byte orders, `root:zbsniff` 0640 in
a 0750 directory. Paste Zigbee2MQTT's `network_key` (16 decimal numbers 0-255 split by commas,
`[1, 3, ...]` with or without the brackets, on one line) or 32 hex digits (in one piece, or whole
bytes split by `:`, `-` or spaces, `0x` prefixes allowed). Piped in, the YAML form with one `- 1`
line per number works too. Any other input is refused and nothing is written. Nothing is printed.
Settings: `KEYDIR` (default `/etc/zbsniff/wireshark`), `GROUP` (default `zbsniff`). A
non-terminal stdin is read to the end, so a file can be piped in.

To write the table by hand, keep the same permissions (`umask 027` first). The format is one
quoted line per key:

```
"00112233445566778899aabbccddeeff","Normal","my-network"
```

Rules for the key and the captures:

- Never print the key, and keep it out of the shell history and of command lines.
- Do not decode Transport Key frames verbosely (`tshark -V`, `-O zbee_aps`, Wireshark's packet
  details): with the table loaded they show the key. `zbpcap.py` asks tshark only for fixed
  fields, never key material.
- A capture opened elsewhere (Wireshark on a desktop) is as secret there as here.

## zbpcap.py

```
zbpcap.py --node NWK [--parent NWK] [--minutes N | --since 'YYYY-mm-dd HH:MM'] [--list] [--dir DIR] [file ...]
```

- `--node`: the node's short (NWK) address, required.
- `--parent`: its parent's short address. Default: the address the node's polls go to most often.
- Without files: the hour files of `--dir` (default `/var/lib/zbsniff/pcap`) that cover the
  window, by default the last 60 minutes. `--since` starts the window there; it runs to now.
- With files and no `--minutes` / `--since`: the whole files.
- `--list`: one line per frame to or from the node.

Run it as a member of the group `zbsniff` (captures and key table), e.g.
`sudo -u zbsniff python3 /opt/zbsniff/zbpcap.py ...`.

Examples:

```sh
# end device 0x1234, the last hour
python3 zbpcap.py --node 0x1234

# 0x1234 under router 0xabcd, from 06:55 to now, every frame
python3 zbpcap.py --node 0x1234 --parent 0xabcd --since '2026-01-31 06:55' --list

# a kept incident file
python3 zbpcap.py --node 0x1234 /var/lib/zbsniff/pcap/keep/zb-20260131-06.pcap.gz
```

Output (made-up numbers):

```
5210 frames in 2 file(s) since 2026-01-31 06:55:00 (812 decrypted); node 0x1234: 64 frames from it (52 polls), 18 to it, 0 with a bad FCS (left out); time stamps +-2 ms
polls: ACK heard 52/52, FP=1 6, parent 0xabcd frame before the next poll: 6/6, delays (ms, +-2) [10.0, 12.0, 8.0, 10.0, 14.0, 10.0]
frames to node, its ACK heard: 17/18 (MAC retries: 1)
frames from node asking for an ACK, ACK heard: 64/64
06:55:03.120 data-req  0x1234->0xabcd seq 41 ACK FP  12B
06:55:03.130 data      0xabcd->0x1234 seq 201 ACK     45B nwk 0x0000->0x1234 cl 0x0006 zcl 0x01
```

How it pairs: an ACK has no addresses, so it belongs to the frame right before it in capture
order when that frame asked for an ACK, has the same sequence number and came at most 20 ms
earlier. Frames with a bad FCS are counted apart and never paired. The same source and sequence
number again within 100 ms counts as a MAC retry.

"ACK heard" is a lower bound: the sniffer misses some ACKs that follow a frame at once (the sender
did not retry, so they arrived). Check the retries before you blame a link.

The field names are those of Debian 13's tshark (`wpan.fcs_ok`, `zbee_nwk.*`, `zbee_aps.*`,
`zbee_zcl.*`).

## Gotchas

- **TX is live at boot.** The Pluto powers up in FDD mode with its TX LO at 2450 MHz and -10 dB
  attenuation. Before the flowgraph starts, `zbsniff.py` powers the TX LO down, sets the TX
  attenuation to maximum and turns the DDS off, and refuses to run (systemd retries) if the TX LO
  is not down. These settings last until the Pluto is power-cycled.
- **An LO on the channel can disturb a device right beside it.** With the Pluto next to a
  battery remote and its RX LO on the channel centre, the remote's parent got no ACK for its
  downlink frames in 4 of 5 test rounds. With the LO off the channel or powered down it was
  clean, and at 1-2 m the A/B check showed no harm. Keep the Pluto 1-2 m from the devices under
  test, and compare your own failure counters with the sniffer on and off before you trust a long
  run. The unit powers both LOs down when the service stops; after a gain sweep, power them down
  by hand (see [Running it](#running-it)).
- **USB bandwidth.** An LO off the channel needs 8 MS/s, but on a shared USB 2 bus the Pluto
  dropped samples above ~5 MS/s. So the LO sits on the channel centre at 4 MS/s.
- **One client.** libiio serves one client per Pluto over USB: stop the service before the gain
  sweep or other `iio_attr` reads.
- **Wi-Fi.** Zigbee channel 11 overlaps Wi-Fi channel 1; Wi-Fi bursts show up as bad-FCS frames.

## Credits

- [gr-ieee802-15-4](https://github.com/bastibl/gr-ieee802-15-4) by Bastian Bloessl (GPL-3.0): the
  O-QPSK receiver chain and the packet sink, with the parameters of its OQPSK PHY example.
- [GNU Radio](https://www.gnuradio.org/) (GPL-3.0) and its gr-iio Pluto source.
- [libiio](https://github.com/analogdevicesinc/libiio) (Analog Devices): `iio_attr`, `iio_info`.
- [Wireshark](https://www.wireshark.org/) (GPL-2.0): tshark decodes and decrypts the captures.
- The pcap writer, the TX and LO handling and `zbpcap.py`: this project.

## License

`zbsniff.py` is GPL-3.0-or-later (SPDX header in the file): its receiver is derived from
gr-ieee802-15-4's OQPSK PHY example (GPL-3.0). The rest of this folder is under the repository's
MIT license ([LICENSE](../../LICENSE)).
