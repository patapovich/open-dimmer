#!/usr/bin/env python3
"""The stock Zigbee firmware facts that docs/BOOTLOADER.md section 15 rests on ("Open Dimmer
behind the stock Tuya ZTU"), pinned against the stock ZTU dump: what that firmware does with the
frames of the v2 bootloader and app that it does not know.

The dump is the public 1 MB image that docs/stock-firmware-analysis.md names (stock ZTU of the
_TZE284_jtbgusdc, Tuya_AVATTO_ZDMS16-2_TS0601_TZE284_jtbgusdc_ZTU.bin). It is not in this repo:
give its path in STOCK_ZTU_DUMP, or the tests are skipped.

    STOCK_ZTU_DUMP=/path/to/ztu.bin python3 tests/test_stock_ztu_dump.py

Pinned (section 11 table, section 15):
  - the byte parser 0xC858 and its 10-entry state table at 0x487E0 (55 AA 02, len <= 246, a
    100 ms inter-byte timer 0x2E);
  - the frame checks: len <= 255 at 0xC9EE and (cmd - 1) <= 0x39 at 0xCA0A (`cmp r3, #57`,
    encoding ab39), both failing to 0xC8E4;
  - the 58-entry command table at 0x48808: handlers only for 0x01, 0x03, 0x05, 0x06, 0x08, 0x0B,
    0x0D, 0x0E, 0x20, 0x24-0x27, 0x29, 0x2C, 0x36-0x38, 0x3A, and 0xC8E4 for the rest (so 0xA1,
    0xA2 and 0xC0 fail the range check, and 0x02, 0x04, 0x28 ... find no handler);
  - 0xC8E4 stops timer 0x2E and returns: no branch to the TX queue 0xDCD8 in 0xC8E4..0xC8F0, and
    on every path to its return no call but the timer's (no TX, no pending-ack entry 0xB898, no
    NV write);
  - its bootloader region 0x0-0x6A0C holds no Telink UART register literal (0x800090-0x80009F),
    the app region has them from 0x22878 on: no request reaches a serial loader;
  - the 0x05 handler 0xD60C is not gated by the product-info flag 0x842428: the flag is read at
    0xC45A, 0xC750, 0xCE24, 0xD956 and 0xD9FE, written at 0xDBB0 (in the 0x01 handler 0xDA5A),
    and none of these is reached from 0xD60C (its own branches and every call, 8 levels deep).
The facts are written down in docs/stock-firmware-analysis.md, "Stock ZTU and unknown commands".
Addresses are file offsets (the Telink TLSR8258 runs the image from flash address 0). The code
is tc32 (Telink's Thumb-like set): only the few encodings below are decoded.
"""
import hashlib
import os
import struct
import unittest

DUMP = os.environ.get("STOCK_ZTU_DUMP", "")
DUMP_SHA256 = "f4afb7a8b5597b7e1a7882c7b59a7dbd0423c2eaf04386ddda68b9a18572f991"   # the file read in 2026-11

PARSER = 0xC858                 # UART RX byte parser
STATE_TABLE, STATES = 0x487E0, 10
STATE_HANDLERS = (0xC8F2, 0xC906, 0xC91C, 0xC932, 0xC94E, 0xC960, 0xC972, 0xC98A, 0xC892, 0xC8B4)
CMD_TABLE, CMDS = 0x48808, 58   # commands 0x01..0x3A
DROP = 0xC8E4                   # a frame it does not take: timer 0x2E stopped, back to the parser
HANDLED = {0x01, 0x03, 0x05, 0x06, 0x08, 0x0B, 0x0D, 0x0E, 0x20, 0x24, 0x25, 0x26, 0x27, 0x29,
           0x2C, 0x36, 0x37, 0x38, 0x3A}
TX_QUEUE, PENDING_ACK = 0xDCD8, 0xB898
TIMER_STOP, TIMER_START = 0x11354, 0x11480
EPILOGUE = 0xC878               # restores the registers and returns
BOOT_REGION = (0x0, 0x6A0C)     # the Tuya bootloader; the app (KNLT) starts at 0x8000
APP_REGION = (0x8000, 0x8000 + 0x42214)
UART_REGS = (0x800090, 0x80009F)
FIRST_APP_UART_LITERAL = 0x22878
DP_REPORT, PRODUCT_INFO = 0xD60C, 0xDA5A   # handlers of 0x05 and 0x01
INFO_FLAG = 0x842428            # set by the 0x01 handler (the product info arrived)
INFO_FLAG_READS = {0xC45A, 0xC750, 0xCE24, 0xD956, 0xD9FE}
INFO_FLAG_WRITES = {0xDBB0}


def load_dump():
    if not DUMP:
        raise unittest.SkipTest("STOCK_ZTU_DUMP is not set (path of the stock ZTU dump)")
    if not os.path.isfile(DUMP):
        raise unittest.SkipTest(f"STOCK_ZTU_DUMP={DUMP}: no such file")
    with open(DUMP, "rb") as f:
        return f.read()


def hw(d, addr):
    return struct.unpack_from("<H", d, addr)[0]


def word(d, addr):
    return struct.unpack_from("<I", d, addr)[0]


def literal(d, addr):
    """The word a pc-relative `tloadr rX, [pc, #imm]` (0x08XX..0x0FXX) at addr loads."""
    ins = hw(d, addr)
    assert 0x0800 <= ins <= 0x0FFF, hex(ins)
    return word(d, ((addr + 4) & ~3) + (ins & 0xFF) * 4)


def sext(v, bits):
    return v - (1 << bits) if v >> (bits - 1) & 1 else v


def decode(d, addr):
    """(length, kind, target) of the tc32 instruction at addr; kind is 'bl', 'b', 'bcond',
    'ret' (pop with pc, or mov pc), or None for one that does not change the flow."""
    ins = hw(d, addr)
    if 0x9000 <= ins <= 0x97FF:                     # tjl: a pair, like Thumb's BL
        lo = hw(d, addr + 2)
        if not 0x9800 <= lo <= 0x9FFF:
            raise AssertionError(f"0x{addr:X}: tjl without its second half")
        return 4, "bl", addr + 4 + (sext(ins & 0x7FF, 11) << 12) + ((lo & 0x7FF) << 1)
    if 0x8000 <= ins <= 0x87FF:                     # tj: unconditional
        return 2, "b", addr + 4 + (sext(ins & 0x7FF, 11) << 1)
    if 0xC000 <= ins <= 0xCDFF:                     # tj<cond>
        return 2, "bcond", addr + 4 + (sext(ins & 0xFF, 8) << 1)
    if ins & 0xFF00 == 0x6D00 or ins & 0xFF87 == 0x0687:   # tpop {.., pc} / tmov pc, rX
        return 2, "ret", None
    return 2, None, None


def words_in(d, lo, hi, pred):
    """Word-aligned 32-bit values in [lo, hi) for which pred holds: {address: value}."""
    return {a: word(d, a) for a in range(lo & ~3, hi - 3, 4) if pred(word(d, a))}


def literal_loads(d, lo, hi, value):
    """Addresses of the pc-relative loads in [lo, hi) that load value."""
    pools = set(words_in(d, lo, hi, lambda w: w == value))
    out = set()
    for addr in range(lo, hi, 2):
        ins = hw(d, addr)
        if 0x0800 <= ins <= 0x0FFF and ((addr + 4) & ~3) + (ins & 0xFF) * 4 in pools:
            out.add(addr)
    return out


def reachable(d, start, depth, limit=6000):
    """Every instruction address on the paths from start (its branches; a table jump `mov pc,
    r3` ends a path), and those of the functions it calls, depth calls deep."""
    seen, done, frontier = set(), set(), {start}
    for _ in range(depth + 1):
        calls = set()
        for f in frontier - done:
            done.add(f)
            todo, body = [f], set()
            while todo:
                addr = todo.pop()
                while addr not in body and len(body) < limit:
                    body.add(addr)
                    try:
                        n, kind, target = decode(d, addr)
                    except AssertionError:          # not code (a literal pool)
                        break
                    if kind == "bl":
                        calls.add(target)
                    elif kind == "b":
                        todo.append(target)
                        break
                    elif kind == "bcond":
                        todo.append(target)
                    elif kind == "ret" or hw(d, addr) == 0x069F:
                        break
                    addr += n
            seen |= body
        frontier = calls
    return seen


def calls_until_return(d, start, limit=64):
    """Every call (tjl target) on every path from start until it returns."""
    calls, seen, todo = set(), set(), [start]
    while todo:
        addr = todo.pop()
        for _ in range(limit):
            if addr in seen:
                break
            seen.add(addr)
            n, kind, target = decode(d, addr)
            if kind == "bl":
                calls.add(target)
            elif kind == "b":
                todo.append(target)
                break
            elif kind == "bcond":
                todo.append(target)
            elif kind == "ret":
                break
            addr += n
        else:
            raise AssertionError(f"no return within {limit} instructions from 0x{start:X}")
    return calls


class StockZtuDump(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.d = load_dump()

    def test_the_file(self):
        """The 1 MB dump with the Telink app header at 0x8000; a different file gets a note."""
        self.assertEqual(len(self.d), 1 << 20)
        self.assertEqual(self.d[0x8008:0x800C], b"KNLT")
        if hashlib.sha256(self.d).hexdigest() != DUMP_SHA256:
            print(f"note: {DUMP} is not the dump these facts were read from (sha256 {DUMP_SHA256})")

    def test_parser_state_table(self):
        """0xC858 jumps through the 10 entries at 0x487E0 (state <= 9) into its own body: 55, AA,
        02, then the frame bytes; length up to 246; a 100 ms inter-byte timer 0x2E."""
        self.assertEqual(hw(self.d, 0xC870), 0xAB09)              # cmp r3, #9 (the state)
        self.assertEqual(literal(self.d, 0xC88C), STATE_TABLE)
        self.assertEqual(hw(self.d, 0xC890), 0x069F)              # mov pc, r3
        table = struct.unpack_from(f"<{STATES}I", self.d, STATE_TABLE)
        self.assertEqual(table, STATE_HANDLERS)
        self.assertTrue(all(PARSER < t < 0xCA18 for t in table))
        self.assertEqual(hw(self.d, 0xC8F2), 0xA855)              # state 0: cmp r0, #0x55
        self.assertEqual(hw(self.d, 0xC906), 0xA8AA)              # state 1: cmp r0, #0xAA
        self.assertEqual(hw(self.d, 0xC91C), 0xA802)              # state 2: cmp r0, #0x02
        self.assertEqual(hw(self.d, 0xC9A8), 0xABF6)              # length: cmp r3, #246
        self.assertEqual(hw(self.d, 0xC942), 0xA02E)              # timer 0x2E ...
        self.assertEqual(hw(self.d, 0xC944), 0xA164)              # ... 100 ms
        self.assertEqual(decode(self.d, 0xC948)[1:], ("bl", TIMER_START))

    def test_frame_checks_fail_to_0xC8E4(self):
        """len <= 255 (0xC9EE) and (cmd - 1) <= 0x39 (0xCA0A, `cmp r3,#57` = ab39); a frame that
        fails either goes to 0xC8E4. So 0xA1, 0xA2 and 0xC0 never reach the command table."""
        self.assertEqual(hw(self.d, 0xC9EE), 0xAAFF)              # cmp r2, #255
        self.assertEqual(decode(self.d, 0xC9F0)[1], "bcond")      # bls: within
        self.assertEqual(decode(self.d, 0xC9F2)[1:], ("b", DROP))
        self.assertEqual(hw(self.d, 0xCA04), 0xBB01)              # subs r3, #1 (cmd - 1)
        self.assertEqual(hw(self.d, 0xCA0A), 0xAB39)
        self.assertEqual(self.d[0xCA0A:0xCA0C], bytes.fromhex("39ab"))
        self.assertEqual(decode(self.d, 0xCA0C), (2, "bcond", 0xCA10))
        self.assertEqual(decode(self.d, 0xCA0E)[1:], ("b", DROP))
        self.assertEqual(literal(self.d, 0xCA12), CMD_TABLE)
        self.assertEqual(hw(self.d, 0xCA16), 0x069F)              # mov pc, r3
        for cmd in (0xA0, 0xA1, 0xA2, 0xA3, 0xC0):
            self.assertGreater(cmd - 1, 0x39)

    def test_command_table(self):
        """58 entries for 0x01..0x3A: a handler only for the listed commands, 0xC8E4 for the rest."""
        table = struct.unpack_from(f"<{CMDS}I", self.d, CMD_TABLE)
        for cmd, target in enumerate(table, 1):
            if cmd in HANDLED:
                self.assertNotEqual(target, DROP, f"cmd 0x{cmd:02X}")
                self.assertTrue(0xC000 <= target < 0xE000, f"cmd 0x{cmd:02X}: 0x{target:X}")
            else:
                self.assertEqual(target, DROP, f"cmd 0x{cmd:02X}")
        self.assertEqual(sum(t == DROP for t in table), 39)

    def test_drop_path_sends_nothing(self):
        """0xC8E4: no branch to the TX queue 0xDCD8 in 0xC8E4..0xC8F0, and up to its return no
        call but the timer's: nothing is sent, queued for an ack or written."""
        addr = DROP
        while addr <= 0xC8F0:
            n, kind, target = decode(self.d, addr)
            if kind:
                self.assertNotEqual(target, TX_QUEUE, f"0x{addr:X}")
            addr += n
        self.assertEqual(decode(self.d, 0xC8E6)[1:], ("bl", TIMER_STOP))
        self.assertEqual(hw(self.d, 0xC8E4), 0xA02E)              # timer 0x2E
        calls = calls_until_return(self.d, DROP)
        self.assertEqual(calls, {TIMER_STOP, TIMER_START})
        self.assertNotIn(TX_QUEUE, calls)
        self.assertNotIn(PENDING_ACK, calls)
        self.assertEqual(decode(self.d, EPILOGUE + 0x10), (2, "ret", None))   # tpop {r4-r7, pc}


    def test_no_uart_in_the_bootloader_region(self):
        """0x0-0x6A0C holds no literal of a Telink UART register (0x800090-0x80009F); the app has
        them from 0x22878 on. So no request of ours reaches a serial loader."""
        uart = lambda w: UART_REGS[0] <= w <= UART_REGS[1]
        self.assertEqual(words_in(self.d, *BOOT_REGION, uart), {})
        app = words_in(self.d, *APP_REGION, uart)
        self.assertEqual(min(app), FIRST_APP_UART_LITERAL)
        self.assertEqual(app[FIRST_APP_UART_LITERAL], 0x800096)

    def test_dp_reports_not_gated_by_product_info(self):
        """The 0x05 handler 0xD60C does not read the product-info flag 0x842428, which only the
        0x01 handler sets: DP reports of an MCU that never answers 0x01 (the health DPs 130..137
        of Open Dimmer) are passed on."""
        table = struct.unpack_from(f"<{CMDS}I", self.d, CMD_TABLE)
        self.assertEqual((table[0x05 - 1], table[0x01 - 1]), (DP_REPORT, PRODUCT_INFO))
        loads = literal_loads(self.d, *APP_REGION, INFO_FLAG)
        self.assertEqual(loads, INFO_FLAG_READS | INFO_FLAG_WRITES)
        for a in INFO_FLAG_READS:
            self.assertEqual(hw(self.d, a + 2) & 0xF800, 0x4800, f"0x{a:X}: tloadrb")   # read
        for a in INFO_FLAG_WRITES:
            self.assertEqual(hw(self.d, a + 2) & 0xF800, 0x4000, f"0x{a:X}: tstorerb")  # write
        self.assertLessEqual(INFO_FLAG_WRITES, reachable(self.d, PRODUCT_INFO, 0))   # the 0x01 handler sets it
        reach = reachable(self.d, DP_REPORT, 8)
        self.assertGreater(len(reach), 1000)
        self.assertFalse(INFO_FLAG_READS & reach)


class Decoder(unittest.TestCase):
    """The tc32 decoding above on encodings from the dump's disassembly (no dump needed)."""

    def test_branches(self):
        def at(addr, *halfwords):
            d = bytearray(addr + 8)
            for i, h in enumerate(halfwords):
                struct.pack_into("<H", d, addr + 2 * i, h)
            return decode(bytes(d), addr)
        self.assertEqual(at(0xC8E6, 0x9004, 0x9D35), (4, "bl", 0x11354))
        self.assertEqual(at(0xDAFA, 0x97FE, 0x9EF3), (4, "bl", 0xC8E4))
        self.assertEqual(at(0xC8F0, 0x8027), (2, "b", 0xC942))
        self.assertEqual(at(0xCA0E, 0x8769), (2, "b", 0xC8E4))
        self.assertEqual(at(0xC8EE, 0xC0C3), (2, "bcond", 0xC878))
        self.assertEqual(at(0xC888, 0x6DF0), (2, "ret", None))
        self.assertEqual(at(0xC890, 0x069F), (2, "ret", None))
        self.assertEqual(at(0xC8E4, 0xA02E), (2, None, None))


if __name__ == "__main__":
    unittest.main()
