"""Ground-truth test of SWS read decoding: write a known 256 B pattern to SRAM, read it back.
Usage: sws_ramtest.py baud div_lo div_hi"""
import sys, os, serial, importlib.util
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("flasher", os.path.join(here, "TLSR825xComFlasher_pico.py"))
F = importlib.util.module_from_spec(spec); spec.loader.exec_module(F)
sys.path.insert(0, here); import swsfix; swsfix.install(F)
PORT = os.environ.get("SWS_PORT", "/dev/ttyACM0")  # the Debugprobe's UART (its second ACM port)
baud = int(sys.argv[1]); lo, hi = int(sys.argv[2]), int(sys.argv[3])
if baud > 1000000: F.bit8mask = 0x40
sp = serial.Serial(PORT, baud); sp.setDTR(True); sp.timeout = 0.05; sp.reset_input_buffer()
for _ in range(2):
    F.rd_wr_usbcom_blk(sp, F.sws_code_end()); F.rd_sws_wr_addr_usbcom(sp, 0x0b3, bytearray([0x00])); F.rd_sws_wr_addr_usbcom(sp, 0x0d, bytearray([0x01]))
pat = bytes(range(256))
ADDR = 0x048000
for div in range(lo, hi + 1):
    F.rd_sws_wr_addr_usbcom(sp, 0x00b2, [div])
    x = F.sws_read_data(sp, 0x00b2, 1)
    if not x or x[0] != div:
        continue
    F.rd_sws_wr_addr_usbcom(sp, ADDR, bytearray(pat))
    r = F.sws_read_data(sp, ADDR, 256)
    if r is None:
        print(f"div {div}: read error"); continue
    r = bytes(r); bad = [i for i in range(256) if r[i] != pat[i]]
    bits = 0
    for i in bad: bits |= r[i] ^ pat[i]
    print(f"div {div}: wrong bytes {len(bad)} wrong-bit-mask 0x{bits:02x}", flush=True)
sp.close()
