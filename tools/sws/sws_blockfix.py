"""Merge two reads of one flash region: blocks that differ are re-read until two consecutive reads agree.
Usage: sws_blockfix.py READ_A READ_B START OUT [baud div]"""
import sys, os, serial, importlib.util
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("flasher", os.path.join(here, "TLSR825xComFlasher_pico.py"))
F = importlib.util.module_from_spec(spec); spec.loader.exec_module(F)
sys.path.insert(0, here); import swsfix; swsfix.install(F)
PORT = os.environ.get("SWS_PORT", "/dev/ttyACM0")  # the Debugprobe's UART (its second ACM port)
A = open(sys.argv[1], 'rb').read(); B = open(sys.argv[2], 'rb').read()
start = int(sys.argv[3], 0); out = sys.argv[4]
baud = int(sys.argv[5]) if len(sys.argv) > 5 else 921600; div = int(sys.argv[6]) if len(sys.argv) > 6 else 55
n = min(len(A), len(B)); res = bytearray(A[:n])
bad = [k for k in range(0, n, 256) if A[k:k+256] != B[k:k+256]]
print(f"{len(bad)} differing blocks of {n//256}")
sp = serial.Serial(PORT, baud); sp.setDTR(True); sp.timeout = 0.05; sp.reset_input_buffer()
for _ in range(2):
    F.rd_wr_usbcom_blk(sp, F.sws_code_end()); F.rd_sws_wr_addr_usbcom(sp, 0x0b3, bytearray([0x00])); F.rd_sws_wr_addr_usbcom(sp, 0x0d, bytearray([0x01]))
F.rd_sws_wr_addr_usbcom(sp, 0x00b2, [div])
def read_block(off, n=256):
    F.rd_sws_wr_addr_usbcom(sp, 0x0b3, bytearray([0x80])); F.rd_sws_wr_addr_usbcom(sp, 0x0d, bytearray([0x00]))
    F.rd_sws_wr_addr_usbcom(sp, 0x0c, bytearray([0x03, (off >> 16) & 0xff, (off >> 8) & 0xff, off & 0xff, 0]))
    F.rd_sws_wr_addr_usbcom(sp, 0x0d, bytearray([0x0A]))
    d = F.sws_read_data(sp, 0x0c, n)
    F.rd_sws_wr_addr_usbcom(sp, 0x0d, bytearray([0x01])); F.rd_sws_wr_addr_usbcom(sp, 0x0b3, bytearray([0x00]))
    return bytes(d) if d and len(d) == n else None
for k in bad:
    reads = [A[k:k+256], B[k:k+256]]
    for t in range(12):
        r = read_block(start + k)
        if r is not None and r in reads:
            res[k:k+256] = r; print(f"0x{start+k:06x}: agreed with {'A' if r == A[k:k+256] else ('B' if r == B[k:k+256] else 'earlier re-read')} after {t+1} re-reads"); break
        reads.append(r)
    else:
        print(f"0x{start+k:06x}: NO agreement"); sys.exit(1)
open(out, 'wb').write(res); print("written", out)
sp.close()
