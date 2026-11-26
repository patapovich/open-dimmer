"""Verified SWS flash dump: each 256 B block is read until two consecutive reads agree.
Usage: sws_dump_verified.py OUT.bin [baud] [div] [start] [size]"""
import sys, os, time, serial, importlib.util
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("flasher", os.path.join(here, "TLSR825xComFlasher_pico.py"))
F = importlib.util.module_from_spec(spec); spec.loader.exec_module(F)
sys.path.insert(0, here); import swsfix; swsfix.install(F)
PORT = os.environ.get("SWS_PORT", "/dev/ttyACM0")  # the Debugprobe's UART (its second ACM port)
out = sys.argv[1]
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 460800
div = int(sys.argv[3]) if len(sys.argv) > 3 else 110
start = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0
size = int(sys.argv[5], 0) if len(sys.argv) > 5 else 0x100000
single = len(sys.argv) > 6 and sys.argv[6] == 'single'
if baud > 1000000: F.bit8mask = 0x40
sp = serial.Serial(PORT, baud); sp.setDTR(True); sp.timeout = 0.05; sp.reset_input_buffer()
F.rd_sws_wr_addr_usbcom(sp, 0x00b2, [div])
x = F.sws_read_data(sp, 0x00b2, 1)
if not x or x[0] != div:
    sys.exit(f"SWS speed readback failed at div {div}: {x}")
def read_block(off, n=256):
    F.rd_sws_wr_addr_usbcom(sp, 0x0b3, bytearray([0x80]))
    F.rd_sws_wr_addr_usbcom(sp, 0x0d, bytearray([0x00]))
    F.rd_sws_wr_addr_usbcom(sp, 0x0c, bytearray([0x03, (off >> 16) & 0xff, (off >> 8) & 0xff, off & 0xff, 0]))
    F.rd_sws_wr_addr_usbcom(sp, 0x0d, bytearray([0x0A]))
    d = F.sws_read_data(sp, 0x0c, n)
    F.rd_sws_wr_addr_usbcom(sp, 0x0d, bytearray([0x01]))
    F.rd_sws_wr_addr_usbcom(sp, 0x0b3, bytearray([0x00]))
    return bytes(d) if d and len(d) == n else None
t0 = time.time(); retries = 0
with open(out, 'wb') as f:
    for off in range(start, start + size, 256):
        prev = read_block(off)
        if single and prev is not None:
            f.write(prev); continue
        for attempt in range(12):
            cur = read_block(off)
            if cur is not None and cur == prev:
                break
            retries += 1; prev = cur
        else:
            sys.exit(f"block 0x{off:06x}: no two equal reads in 12 tries")
        f.write(cur)
        if (off - start) % 0x10000 == 0:
            el = time.time() - t0; done = off - start + 256
            print(f"0x{off:06x} {done*100//size}% {el:.0f}s retries {retries}", flush=True)
print(f"done {size} B in {time.time()-t0:.0f}s, block retries {retries}")
sp.close()
