"""Corrected SWS read decoding for the Pico debugprobe bridge (calibrated 2026-11-24 against an SRAM pattern):
every data bit is sampled with UART-bit mask 0x20 (pvvx uses 0x10 for bits 6..0, which misreads bits 6/5 here)."""
MASK = 0x20
def install(F, mask=MASK):
    def sws_decode_blk(blk):
        if len(blk) == 9 and (blk[8] & 0xfe) == 0xfe:
            data = 0
            for el in range(8):
                data = (data << 1) | (1 if (blk[el] & mask) == 0 else 0)
            return data
        return None
    F.sws_decode_blk = sws_decode_blk
