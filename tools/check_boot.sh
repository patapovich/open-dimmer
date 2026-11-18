#!/bin/sh
# The v2 bootloader's layout, as the app and the tools rely on it (docs/BOOTLOADER.md section 11):
# boot_id_t (16 bytes) at 0x08000010 with its content, the write range src/flash_drv.c gets from
# boot.ld, boot.bin within its 4 KB sector, and .bss plus 512 bytes of stack below the mailbox.
# Usage: check_boot.sh build-boot/boot.elf build-boot/boot.bin
set -e
elf=$1
bin=$2
syms=$(arm-none-eabi-nm -S "$elf")

# address and size of a symbol ("-" for an absolute symbol without a size)
sym() {
    echo "$syms" | awk -v n="$1" '$NF == n { if (NF == 4) print $1 " " $2; else print $1 " -" }'
}

fail() {
    echo "check_boot: $1" >&2
    exit 1
}

[ "$(sym k_boot_id)" = "08000010 00000010" ] || fail "k_boot_id is $(sym k_boot_id), not 16 bytes at 08000010"
[ "$(sym _flash_write_start)" = "08001000 -" ] || fail "_flash_write_start is $(sym _flash_write_start)"
[ "$(sym _flash_write_end)" = "08004800 -" ] || fail "_flash_write_end is $(sym _flash_write_end)"
# "ODBL", version 2, features 0x0F, reserved 0xFFFF, app_max 0x3800, mailbox 0x20000BE0
arm-none-eabi-objdump -s -j .boot_id "$elf" | grep -q ' 8000010 4f44424c 020fffff 00380000 e00b0020 ' ||
    fail "boot_id content: $(arm-none-eabi-objdump -s -j .boot_id "$elf" | tail -1)"
ebss=$(sym _ebss | cut -d' ' -f1)
[ $((0x$ebss + 512)) -le $((0x20000BE0)) ] || fail "_ebss $ebss + 512 bytes of stack reach the mailbox"
n=$(stat -c %s "$bin")
[ "$n" -le 4096 ] || fail "boot.bin is $n bytes"
echo "bootloader layout ok: boot_id at 0x08000010, writes 0x08001000..0x08004800, $n of 4096 bytes, .bss to 0x$ebss"
