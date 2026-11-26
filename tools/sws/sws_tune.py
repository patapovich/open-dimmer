"""Find a stable SWS divider: for each swsdiv, read flash 0x8000..+256 N times, count mismatches."""
import sys, os, time, serial, importlib.util
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("flasher", os.path.join(here, "TLSR825xComFlasher_pico.py"))
F = importlib.util.module_from_spec(spec); spec.loader.exec_module(F)
PORT = os.environ.get("SWS_PORT", "/dev/ttyACM0")  # the Debugprobe's UART (its second ACM port)
baud = int(sys.argv[1]) if len(sys.argv) > 1 else 921600
lo, hi = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (40, 80)
N = int(sys.argv[4]) if len(sys.argv) > 4 else 4
if baud > 1000000: F.bit8mask = 0x40
if baud > 3000000: F.bit8mask = 0x80
sp = serial.Serial(PORT, baud); sp.setDTR(True); sp.timeout = 0.05; sp.reset_input_buffer()
def read_block(off, n=256):
    F.rd_sws_wr_addr_usbcom(sp, 0x0b3, bytearray([0x80]))
    F.rd_sws_wr_addr_usbcom(sp, 0x0d, bytearray([0x00]))
    F.rd_sws_wr_addr_usbcom(sp, 0x0c, bytearray([0x03, (off >> 16) & 0xff, (off >> 8) & 0xff, off & 0xff, 0]))
    F.rd_sws_wr_addr_usbcom(sp, 0x0d, bytearray([0x0A]))
    d = F.sws_read_data(sp, 0x0c, n)
    F.rd_sws_wr_addr_usbcom(sp, 0x0d, bytearray([0x01]))
    F.rd_sws_wr_addr_usbcom(sp, 0x0b3, bytearray([0x00]))
    return bytes(d) if d else None
res = {}
for div in range(lo, hi + 1):
    F.rd_sws_wr_addr_usbcom(sp, 0x00b2, [div])
    x = F.sws_read_data(sp, 0x00b2, 1)
    if not x or x[0] != div:
        print(f"div {div}: no readback"); continue
    reads = [read_block(0x8000) for _ in range(N)]
    if any(r is None for r in reads):
        print(f"div {div}: read error"); continue
    ref = reads[0]
    diff = sum(sum(1 for a, b in zip(ref, r) if a != b) for r in reads[1:])
    res[div] = diff
    print(f"div {div}: SW-CLK~{baud*div/2e6:.1f}MHz mismatches {diff} first {ref[:8].hex()}", flush=True)
good = [d for d, v in res.items() if v == 0]
print("stable divs:", good)
if good:
    mid = good[len(good)//2]; print("choose", mid)
sp.close()
