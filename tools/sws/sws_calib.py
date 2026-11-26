"""Calibrate SWS read decoding: per bit position, which UART-bit mask predicts the data bit for all 256 values."""
import sys, os, serial, importlib.util
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("flasher", os.path.join(here, "TLSR825xComFlasher_pico.py"))
F = importlib.util.module_from_spec(spec); spec.loader.exec_module(F)
PORT = os.environ.get("SWS_PORT", "/dev/ttyACM0")  # the Debugprobe's UART (its second ACM port)
baud = int(sys.argv[1]); div = int(sys.argv[2])
sp = serial.Serial(PORT, baud); sp.setDTR(True); sp.timeout = 0.05; sp.reset_input_buffer()
F.rd_sws_wr_addr_usbcom(sp, 0x00b2, [div])
pat = bytes(range(256)); ADDR = 0x048000
F.rd_sws_wr_addr_usbcom(sp, ADDR, bytearray(pat))
# raw read, like sws_read_data but keep blk
sp.timeout = 0.01
F.rd_wr_usbcom_blk(sp, F.sws_rd_addr(ADDR))
raws = []
for i in range(256):
    sp.write([0xfe]); blk = sp.read(9)
    if len(blk) < 9: blk += sp.read(10 - len(blk))
    raws.append(bytes(blk))
F.rd_wr_usbcom_blk(sp, F.sws_code_end())
sp.close()
ok = all(len(r) >= 9 for r in raws); print("all 9-byte responses:", ok)
for k in range(8):                      # data bit 7-k is in raws[i][k]
    good = []
    for m in range(8):
        mask = 1 << m
        if all(((r[k] & mask) == 0) == bool((v >> (7 - k)) & 1) for v, r in enumerate(raws)):
            good.append(hex(mask))
    print(f"bit {7-k} (blk[{k}]): masks that decode all 256 correctly: {good}")
print("example raw for 0x00, 0xff, 0xaa:", raws[0].hex(), raws[255].hex(), raws[0xaa].hex())
