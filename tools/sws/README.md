# Reading a Telink Zigbee module over SWS with a Raspberry Pi Debugprobe

These scripts read the whole flash of a Telink TLSR8258 Zigbee module (the ZTU of the AVATTO
ZDMS16-2, the ZT3L of the Tuya TS110E and others) over its single-wire debug interface (SWS),
with a **Raspberry Pi Debugprobe** (or a Pico running the Debugprobe firmware) as the USB-UART
bridge. They build on pvvx's [TlsrComSwireWriter](https://github.com/pvvx/TlsrComSwireWriter)
(The Unlicense) and add what the Debugprobe needs:

- DTR stays set: the Debugprobe forwards its UART only while DTR is set
  (`TLSR825xComFlasher_pico.py`, otherwise pvvx's file unchanged).
- A corrected bit decoder (`swsfix.py`): with this bridge pvvx's decoder misreads data bits 6 and
  5; sampling every bit with mask 0x20 reads them right (calibrated against a known pattern in
  the chip's RAM).
- A verified dump (`sws_dump_verified.py`): about 1 % of the 256-byte blocks come back wrong at
  random, so every block is read until two reads in a row agree.

Used to read the factory firmware of a TS110E (`_TZ3210_3mpwqzuu`) ZT3L module: bootloader and
app read back with a valid CRC.

> [!WARNING]
> **Mains voltage.** Disconnect all mains wires before you open a module; power it only from
> the probe's 3.3 V while it is open.

> [!CAUTION]
> **A dump holds secrets.** The flash of a module that has been in a network holds its network
> key and link keys, and the Tuya factory area holds the module's UUID and auth key. Keep the
> file to yourself (e.g. `chmod 600`); do not attach it to issues or upload it.

## Wiring

| Debugprobe | Module |
|---|---|
| UART TX (GP4) | SWS, **through a 1 kΩ resistor** |
| UART RX (GP5) | SWS, directly (the same node as the resistor's module end) |
| 3.3 V | module VCC |
| GND | module GND |

- Nothing else may be connected to the SWS node.
- SWS on the ZT3L: pin 17. On the ZDMS16-2's ZTU: the 4th pad from the antenna end, on the
  D3/D7 edge.
- RST (ZT3L pin 1): touch it to GND briefly while the script starts, if the chip does not
  answer at once. An RTS line on RST also works.

## Use

Python 3 with `pyserial`. Set the Debugprobe's UART port (its second ACM device, e.g.
`/dev/serial/by-id/usb-Raspberry_Pi_Debugprobe_on_Pico__CMSIS-DAP__…-if01`):

```sh
export SWS_PORT=/dev/serial/by-id/usb-Raspberry_Pi_Debugprobe_on_Pico__CMSIS-DAP__XXXX-if01
```

1. **Check the decoding** (writes a test pattern to RAM, reads it back):
   `python3 sws_ramtest.py 921600 50 60` - wanted: a divider with "wrong bytes 0".
   `sws_calib.py BAUD DIV` shows which sampling mask decodes each bit.
2. **Pick a stable SWS speed** (reads the same flash block several times):
   `python3 sws_tune.py 921600 40 80` - prints the dividers with no mismatches. Known good:
   921600 baud with divider 55, or 460800 baud with divider 109-110.
3. **Dump** the 1 MB flash:
   `python3 sws_dump_verified.py dump.bin 921600 55` (it prints its progress every 64 KB).
4. Two earlier reads of the same area can be merged with
   `sws_blockfix.py READ_A READ_B START OUT [baud div]`: blocks that differ are read again
   until two reads agree.

A double read cannot catch an error that repeats in the same way every time: run step 1 first.

## Credits

pvvx (TlsrComSwireWriter), Aaron Christophel and Pila (the TLSR825x flasher). The Debugprobe
changes and the verification scripts: this project.
