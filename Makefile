# Open Dimmer: open-source firmware for the PY32F002A dimming MCU of the AVATTO ZDMS16-2.
# SDK (CMSIS + LL drivers) is used straight from the py32f0-template checkout.

SDK      ?= $(HOME)/projects/py32f0-template/Libraries
PACK     ?= $(HOME)/projects/zdms16-py32/pack/Puya.PY32F0xx_DFP.1.1.7-opt.pack
PYOCD    ?= $(HOME)/projects/zdms16-py32/.venv/bin/pyocd
PROJECT  := zdms16_py32
# make VERSION_BYTE=0x81 BUILD=build-x: another version byte (tests of updates). Use a new
# BUILD directory: a change of flags alone does not rebuild the objects.
# make SIM=1: virtual 50 Hz zero cross from TIM16 instead of PA4 (bench, no mains)
SIM      ?= 0
# make MEASURE=1: measurement build, outputs always off, zero-cross / input statistics
# reported every 10 s (version 2.0.15 by default)
MEASURE  ?= 0
# make DIAG=1: normal outputs, the same statistics reported every 10 s (2.0.14 by default)
# MEASURE and DIAG builds decline every update the module's Zigbee firmware offers (it offers
# its carried release a few seconds after mains-on, which would replace them before any
# statistics), so they stay whatever their version byte. The way back to a normal build is
# SWD: make flash-all. Their image header flags (MEASURE 0x02, DIAG 0x04) make
# tools/embed_ztu.py refuse them, so no Zigbee firmware ever carries one.
DIAG     ?= 0
# Build number (src/tuya.h, docs/BOOTLOADER.md section 2): FW_BUILD is the release build, a
# literal in src/tuya.h that no make variable overrides (tools/release/release_check.py reads it).
# make FW_TEST=n (1..99): a spare test build FW_BUILD + n, below the next release (FW_BUILD + 100),
# so a unit that ran it always takes that release. make FW_PREV=1 FW_TEST=n: FW_BUILD - 100 + n,
# below this release (bench tests B3, B11: e.g. 299 below release 300). The version byte stays:
# test builds bump only the build. Each has its own BUILD directory (as a VERSION_BYTE build needs
# one), and tools/mkimage.py --test n checks that the image's build is a release build + n, so a
# test build never has the header of a release.
FW_TEST  ?=
FW_PREV  ?=
# make LINK_WDG_TEST=1 FW_PREV=1 FW_TEST=n: bench test B11 (spare, SWD): the link watchdog fires
# after 2 min without a frame in a run of 5 min, and sees no frame at all (src/link_core.h). Only
# below the release, so a unit on the release never takes such an image, even carried by mistake.
LINK_WDG_TEST ?=
# make OFFER_DECLINE_TEST=1 FW_PREV=1 FW_TEST=n: bench test B3 (spare, SWD): the app declines
# every update offer (a broken update path); the bootloader's power-on probe must still install
# the release the module carries. Below the release, like LINK_WDG_TEST.
OFFER_DECLINE_TEST ?=
# make CRASH_TEST=1 FW_TEST=n: bench test B4 (spare, SWD): the app faults 1 s after its start (a
# crash loop); above the carried release, so the module never offers it that release, and after 3
# unconfirmed runs the bootloader's rescue window must install it (older images allowed there).
CRASH_TEST ?=
# make STOCKLIKE_TEST=1 FW_TEST=n [STOCKLIKE_PINS_MS=ms]: bench test T5 (spare, SWD, then the stock
# option bytes): an app the module takes for the factory firmware (it reports version 1.0.7, so the
# module never asks who it is and treats it as stock) that, like the factory firmware, makes the SWD
# pins PA13 / PA14 outputs (low) STOCKLIKE_PINS_MS after its start (default 1500; 0 = never). For
# the module's SWD install rehearsal (this repository has no copy of the factory firmware). Above the
# carried release (FW_TEST, no FW_PREV), so the bootloader's power-on probe never replaces it.
STOCKLIKE_TEST ?=
STOCKLIKE_PINS_MS ?= 1500
ifneq ($(FW_BUILD),)
  $(error FW_BUILD is the release build in src/tuya.h; test builds: make FW_TEST=n [FW_PREV=1])
endif
# The same refusals as src/tuya.h's #errors (same words), here before anything is built.
ifneq ($(filter-out 0 1,$(LINK_WDG_TEST)),)
  $(error LINK_WDG_TEST=$(LINK_WDG_TEST): LINK_WDG_TEST=1 (bench test B11) or 0)
endif
ifeq ($(LINK_WDG_TEST),1)
  ifneq ($(FW_PREV),1)
    $(error LINK_WDG_TEST builds lie below the release: make LINK_WDG_TEST=1 FW_PREV=1 FW_TEST=n)
  endif
  ifneq ($(filter 1,$(SIM) $(MEASURE) $(DIAG)),)
    $(error LINK_WDG_TEST is a normal build's bench switch (not with SIM, MEASURE or DIAG))
  endif
endif
ifneq ($(filter-out 0 1,$(OFFER_DECLINE_TEST) $(CRASH_TEST)),)
  $(error OFFER_DECLINE_TEST / CRASH_TEST take 1 (bench tests B3 / B4) or 0)
endif
ifeq ($(OFFER_DECLINE_TEST),1)
  ifneq ($(FW_PREV),1)
    $(error OFFER_DECLINE_TEST builds lie below the release: make OFFER_DECLINE_TEST=1 FW_PREV=1 FW_TEST=n)
  endif
endif
ifeq ($(CRASH_TEST),1)
  ifeq ($(FW_TEST),)
    $(error CRASH_TEST builds lie above the release: make CRASH_TEST=1 FW_TEST=n)
  endif
  ifeq ($(FW_PREV),1)
    $(error CRASH_TEST builds lie above the release: make CRASH_TEST=1 FW_TEST=n (no FW_PREV))
  endif
endif
ifneq ($(filter-out 0 1,$(STOCKLIKE_TEST)),)
  $(error STOCKLIKE_TEST takes 1 (bench test T5) or 0)
endif
ifeq ($(STOCKLIKE_TEST),1)
  ifeq ($(FW_TEST),)
    $(error STOCKLIKE_TEST builds lie above the release: make STOCKLIKE_TEST=1 FW_TEST=n)
  endif
  ifneq ($(FW_PREV),)
    $(error STOCKLIKE_TEST builds lie above the release: make STOCKLIKE_TEST=1 FW_TEST=n (no FW_PREV))
  endif
  ifneq ($(filter 1,$(SIM) $(MEASURE) $(DIAG) $(LINK_WDG_TEST) $(OFFER_DECLINE_TEST) $(CRASH_TEST)),)
    $(error STOCKLIKE_TEST is a normal build's bench switch (alone, not with SIM, MEASURE, DIAG or another bench test))
  endif
  VERSION_BYTE ?= 0x47
endif
ifneq ($(filter 1,$(OFFER_DECLINE_TEST) $(CRASH_TEST)),)
  ifneq ($(filter 1,$(SIM) $(MEASURE) $(DIAG) $(LINK_WDG_TEST)),)
    $(error OFFER_DECLINE_TEST / CRASH_TEST are normal builds' bench switches (one at a time, not with SIM, MEASURE, DIAG or LINK_WDG_TEST))
  endif
  ifeq ($(OFFER_DECLINE_TEST)$(CRASH_TEST),11)
    $(error OFFER_DECLINE_TEST and CRASH_TEST: one bench test at a time)
  endif
endif
ifneq ($(FW_TEST),)
  ifeq ($(filter $(FW_TEST),$(shell seq 1 99)),)
    $(error FW_TEST=$(FW_TEST): a test build takes 1..99 (FW_BUILD + n stays below the next release))
  endif
  ifeq ($(SIM),1)
    $(error FW_TEST=$(FW_TEST) with SIM=1: a SIM build has the fixed build 0xFFFE)
  endif
endif
ifneq ($(FW_PREV),)
  ifneq ($(FW_PREV),1)
    $(error FW_PREV=$(FW_PREV): make FW_PREV=1 FW_TEST=n (1..99) or leave FW_PREV unset)
  endif
  ifeq ($(FW_TEST),)
    $(error FW_PREV=1 alone would be the previous release: make FW_PREV=1 FW_TEST=n (1..99), the build FW_BUILD - 100 + n)
  endif
endif
BUILD    ?= build$(if $(filter 1,$(SIM)),-sim)$(if $(filter 1,$(MEASURE)),-measure)$(if $(filter 1,$(DIAG)),-diag)$(if $(FW_PREV),-prev)$(if $(FW_TEST),-t$(FW_TEST))$(if $(filter 1,$(LINK_WDG_TEST)),-linktest)$(if $(filter 1,$(OFFER_DECLINE_TEST)),-decline)$(if $(filter 1,$(CRASH_TEST)),-crash)$(if $(filter 1,$(STOCKLIKE_TEST)),-stocklike)

CC       := arm-none-eabi-gcc
OBJCOPY  := arm-none-eabi-objcopy
SIZE     := arm-none-eabi-size

LL_SRCS  := py32f0xx_ll_rcc.c py32f0xx_ll_utils.c py32f0xx_ll_gpio.c py32f0xx_ll_usart.c \
            py32f0xx_ll_tim.c py32f0xx_ll_exti.c
SRCS     := $(wildcard src/*.c) \
            $(SDK)/CMSIS/Device/PY32F0xx/Source/system_py32f0xx.c \
            $(addprefix $(SDK)/PY32F0xx_LL_Driver/Src/,$(LL_SRCS))
ASRCS    := $(SDK)/CMSIS/Device/PY32F0xx/Source/gcc/startup_py32f002a.s
# The settings ring: from release M (2.1.14) the sector's upper half (0x08004800, 2 KB, 16 pages),
# with RING_HIGH in the header, so apps up to 14 KB fit (docs/BOOTLOADER.md section 1); the app
# still finds the records of the 4 KB ring below it (src/store.c). RING_HIGH=0: the layout up to
# 2.1.13 (12 KB, the ring at 0x08004000). The linker script of the high layout is made from the
# low one.
# A test build below the release (FW_PREV) of release M itself lies below M: the low layout.
RELEASE_BUILD := $(shell sed -n 's/^\#define FW_BUILD  *\([0-9][0-9]*\)$$/\1/p' src/tuya.h)
RING_HIGH ?= $(if $(and $(filter 1,$(FW_PREV)),$(filter 400,$(RELEASE_BUILD))),0,1)
ifneq ($(filter-out 0 1,$(RING_HIGH)),)
  $(error RING_HIGH=$(RING_HIGH): 0 or 1)
endif
LDSCRIPT := $(if $(filter 1,$(RING_HIGH)),$(BUILD)/app_ring_high.ld,py32f002a_app.ld)

INCLUDES := -Isrc -I$(SDK)/CMSIS/Core/Include -I$(SDK)/CMSIS/Device/PY32F0xx/Include \
            -I$(SDK)/PY32F0xx_LL_Driver/Inc
DEFS     := -DPY32F002Ax5 -DUSE_FULL_LL_DRIVER -DSIM_ZC=$(SIM) -DMEASURE=$(MEASURE) -DDIAG_REPORT=$(DIAG) $(if $(VERSION_BYTE),-DFW_VERSION=$(VERSION_BYTE),$(if $(filter 1,$(MEASURE)),-DFW_VERSION=0x8F,$(if $(filter 1,$(DIAG)),-DFW_VERSION=0x8E))) \
            $(if $(FW_TEST),-DFW_TEST=$(FW_TEST)) $(if $(FW_PREV),-DFW_PREV=$(FW_PREV)) \
            $(if $(filter 1,$(LINK_WDG_TEST)),-DLINK_WDG_TEST=1) -DRING_HIGH=$(RING_HIGH) \
            $(if $(filter 1,$(OFFER_DECLINE_TEST)),-DOFFER_DECLINE_TEST=1) $(if $(filter 1,$(CRASH_TEST)),-DCRASH_TEST=1) \
            $(if $(filter 1,$(STOCKLIKE_TEST)),-DSTOCKLIKE_TEST=1 -DSTOCKLIKE_PINS_MS=$(STOCKLIKE_PINS_MS)u)
ARCH     := -mcpu=cortex-m0plus -mthumb
# Data stays in common sections (no -fdata-sections): GCC then reaches a file's statics from one
# anchor instead of a literal per variable (-188 bytes in the normal build); unused data is
# negligible here. Functions keep their own sections for --gc-sections.
CFLAGS   := $(ARCH) $(DEFS) $(INCLUDES) -Os -g -Wall -Wextra -Wno-unused-parameter \
            -ffunction-sections -std=c11 -MMD -MP
LDFLAGS  := $(ARCH) -T$(LDSCRIPT) -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(PROJECT).map \
            --specs=nano.specs --specs=nosys.specs
# The app (not the bootloader, which stays byte for byte as installed) is optimized as a whole at
# link time: -384 bytes. The vector table, the header and the boot_id are kept by the linker
# script and the handlers by their names; src/libc_min.c keeps its loops by a per-function
# attribute that LTO carries, and CI checks that mem* call nothing.
APP_LTO  := -flto

OBJS     := $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o))) $(addprefix $(BUILD)/,$(notdir $(ASRCS:.s=.o)))

# Bootloader v2 (4 KB at 0x08000000, docs/BOOTLOADER.md): the same for every app build, and
# frozen once installed, so it builds without a single warning. It shares src/flash_drv.c with
# the app (one driver; the linker script sets what each may write). Its data stays in common
# sections (-fno-data-sections, as the app's): all of it is used, and GCC then reaches the
# statics from one anchor instead of a literal per variable (-132 bytes).
BOOT_BUILD := build-boot
BOOT_SRCS  := boot/boot.c src/flash_drv.c
BOOT_OBJS  := $(addprefix $(BOOT_BUILD)/,$(notdir $(BOOT_SRCS:.c=.o)))
BOOT_CFLAGS := $(CFLAGS) -Werror -fno-data-sections -ffreestanding -fno-tree-loop-distribute-patterns
BOOT_LDFLAGS := $(ARCH) -Tboot/boot.ld -Wl,--gc-sections -Wl,-Map=$(BOOT_BUILD)/boot.map -nostdlib -lgcc

vpath %.c $(sort $(dir $(SRCS) $(BOOT_SRCS)))
vpath %.s $(sort $(dir $(ASRCS)))

# App image (what the Zigbee module carries and sends), bootloader, and the
# whole flash for the first SWD flashing.
all: $(BUILD)/$(PROJECT).img $(BOOT_BUILD)/boot.bin $(BUILD)/$(PROJECT)_full.bin

$(BUILD) $(BOOT_BUILD):
	mkdir -p $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) $(APP_LTO) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) $(ARCH) -c $< -o $@

$(BOOT_BUILD)/%.o: %.c | $(BOOT_BUILD)
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

$(BOOT_BUILD)/%.o: %.s | $(BOOT_BUILD)
	$(CC) $(ARCH) -c $< -o $@

$(BUILD)/app_ring_high.ld: py32f002a_app.ld | $(BUILD)
	sed -e 's/^\(  FLASH (rx)     : ORIGIN = 0x08001000, LENGTH = \)12K/\114K/' \
	    -e 's/^\(  SETTINGS (r)   : ORIGIN = \)0x08004000, LENGTH = 4K/\10x08004800, LENGTH = 2K/' $< > $@
	grep -q 'LENGTH = 14K' $@ && grep -q 'ORIGIN = 0x08004800, LENGTH = 2K' $@

# A flip of RING_HIGH in the same build dir rebuilds everything (the flag is in the objects and
# the layout in the link): a stamp per value.
RING_STAMP := $(BUILD)/.ring_high_$(RING_HIGH)
$(RING_STAMP): | $(BUILD)
	rm -f $(BUILD)/.ring_high_*
	touch $@
$(OBJS): $(RING_STAMP)

$(BUILD)/$(PROJECT).elf: $(OBJS) $(LDSCRIPT)
	$(CC) $(LDFLAGS) -Os $(APP_LTO) $(OBJS) -o $@
	$(SIZE) $@

$(BUILD)/$(PROJECT).bin: $(BUILD)/$(PROJECT).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/$(PROJECT).img: $(BUILD)/$(PROJECT).bin tools/mkimage.py tools/fwimage.py
	python3 tools/mkimage.py $< $@ $(if $(FW_TEST),--test $(FW_TEST)) $(if $(filter 1,$(STOCKLIKE_TEST)),--stocklike)

# The bootloader's flash bytes (text + data) from the ELF: a warning above 3584 (512 bytes left
# of its 4 KB sector; CI warns the same, tools/check_elf.py).
BOOT_WARN_ABOVE := 3584
$(BOOT_BUILD)/boot.elf: $(BOOT_OBJS)
	$(CC) $(BOOT_LDFLAGS) $^ -o $@
	$(SIZE) $@
	@n=$$($(SIZE) -B $@ | awk 'NR == 2 { print $$1 + $$2 }'); \
	 echo "bootloader: $$n of 4096 bytes"; \
	 if [ "$$n" -gt 4096 ]; then echo "error: the bootloader does not fit its 4 KB sector" >&2; exit 1; fi; \
	 if [ "$$n" -gt $(BOOT_WARN_ABOVE) ]; then \
	     echo "warning: the bootloader has only $$((4096 - n)) bytes of its sector free (warning above $(BOOT_WARN_ABOVE))" >&2; fi

$(BOOT_BUILD)/boot.bin: $(BOOT_BUILD)/boot.elf
	$(OBJCOPY) -O binary $< $@

# The bootloader's layout as the app and the tools rely on it (docs/BOOTLOADER.md section 11):
# boot_id_t at 0x08000010 with its content, the write range of boot.ld, the size, and RAM below
# the mailbox with 512 bytes of stack (tools/check_boot.sh). boot.ld asserts the first two at
# link time too.
check-boot: $(BOOT_BUILD)/boot.bin
	sh tools/check_boot.sh $(BOOT_BUILD)/boot.elf $(BOOT_BUILD)/boot.bin

$(BUILD)/$(PROJECT)_full.bin: $(BUILD)/$(PROJECT).img $(BOOT_BUILD)/boot.bin tools/mkimage.py tools/fwimage.py
	python3 tools/mkimage.py $< --full $(BOOT_BUILD)/boot.bin $@ $(if $(filter 1,$(STOCKLIKE_TEST)),--stocklike)

# The module must be powered from the probe, never from mains, while wired.
# Always the whole image (bootloader + app), so the two always match. pyocd
# erases whole 4 KB sectors: the bootloader has the first one to itself (with
# the old 2 KB layout an app-only load wiped it, 2026-11-17). The image ends
# below the settings sector (0x08004000), which is not touched; an image over 12 KB
# would erase it, so this refuses one (tools/swd/install.sh takes it with ERASE_SETTINGS=1).
# A debugger reset is a software reset and keeps the RAM, so the boot word pair of the
# mailbox (0x20000BF8, 0x20000BFC) is written 0 before every reset (pyocd load resets
# after writing; then the final reset): the flashed build starts with fresh tries
# (docs/BOOTLOADER.md section 3). The watchdog is frozen while the core is halted
# (DBGMCU APBFZ1 DBG_IWDG_STOP): the v2 bootloader starts it at every boot, and pyocd load
# halts the core to program it. If that first step fails, the load goes on with a warning.
PYOCD_T        = -t py32f002ax5 --pack $(PACK) -f 400000
BOOT_WORD_ZERO := -c "write32 0x20000BF8 0" -c "write32 0x20000BFC 0"
IWDG_FREEZE    := -c "write32 0x40015808 0x1000"
flash flash-all: $(BUILD)/$(PROJECT)_full.bin
	@[ "$$(stat -c %s $<)" -le 16384 ] || { \
	    echo "$< reaches the settings sector (0x08004000), which pyocd would erase: tools/swd/install.sh ERASE_SETTINGS=1" >&2; exit 1; }
	$(PYOCD) commander $(PYOCD_T) -O connect_mode=attach -O resume_on_disconnect=false $(IWDG_FREEZE) $(BOOT_WORD_ZERO) >/dev/null 2>&1 || \
	    echo "warning: the watchdog freeze and the boot word clear before the load failed: the load halts a core whose watchdog may run, and its reset keeps the old boot word" >&2
	$(PYOCD) load $(PYOCD_T) --format bin --base-address 0x08000000 $<
	$(PYOCD) commander $(PYOCD_T) -O connect_mode=attach $(IWDG_FREEZE) -c halt $(BOOT_WORD_ZERO) -c reset

clean:
	rm -rf build build-sim build-boot

# Header dependencies (an object is rebuilt when a header it includes changes)
-include $(OBJS:.o=.d) $(BOOT_OBJS:.o=.d)

.PHONY: all flash flash-all clean check-boot

# phase.c on the PC (tests/test_phase.c) against fake registers (tests/fake), once per
# build variant. Its fader setters are renamed to hooks that check the interrupts are off.
FADER_HOOKS := $(foreach f,set set_timed set_floor set_profile hold_start hold_start_from_dark hold_stop hold_cancel,\
                 -Ddim_fader_$(f)=checked_dim_fader_$(f))
HOST_CC     := cc -std=c11 -Wall -Wextra -Werror
define phase_test
	$(HOST_CC) -Itests/fake -Isrc $(2) $(FADER_HOOKS) -c src/phase.c -o $(BUILD)/host/phase_$(1).o
	$(HOST_CC) -Itests/fake -Isrc $(2) tests/test_phase.c src/dim_core.c $(BUILD)/host/phase_$(1).o \
	   -o $(BUILD)/host/test_phase_$(1)
	$(BUILD)/host/test_phase_$(1)
endef

# Hardware-free logic on the PC: the real sources against stand-in device headers (tests/host,
# HOST_TEST: the mailbox is a variable). dimmer.c and controls.c also link the host stubs
# (tests/host_stubs.*); the flash driver test is built as C++ so that its register model sees
# every access.
HOST_CFLAGS     := -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -DHOST_TEST -Itests/host -Isrc
HOST_TEST_FLAGS := $(HOST_CFLAGS) -D_DEFAULT_SOURCE -Itests
define link_wdg_test
	cc $(HOST_TEST_FLAGS) -O2 $(2) tests/test_link_wdg.c tests/host_boot.c src/tuya.c src/bootif.c \
	   -o $(BUILD)/host/test_link_wdg_$(1)
	$(BUILD)/host/test_link_wdg_$(1)
endef
define tuya_test
	cc $(HOST_TEST_FLAGS) $(2) tests/test_tuya.c tests/host_boot.c src/tuya.c src/bootif.c \
	   -o $(BUILD)/host/test_tuya_$(1)
	$(BUILD)/host/test_tuya_$(1)
endef
test:
	mkdir -p $(BUILD)/host
	cc -std=c11 -Wall -Wextra -Werror -Isrc tests/test_dim_core.c src/dim_core.c -o $(BUILD)/host/test_dim_core
	$(BUILD)/host/test_dim_core
	$(call phase_test,normal,-DSIM_ZC=0 -DMEASURE=0 -DDIAG_REPORT=0)
	$(call phase_test,sim,-DSIM_ZC=1 -DMEASURE=0 -DDIAG_REPORT=0)
	$(call phase_test,diag,-DSIM_ZC=0 -DMEASURE=0 -DDIAG_REPORT=1)
	$(call phase_test,measure,-DSIM_ZC=0 -DMEASURE=1 -DDIAG_REPORT=0)
	cc -std=c11 -Wall -Wextra -Werror -Isrc tests/test_input_core.c src/input_core.c -o $(BUILD)/host/test_input_core
	$(BUILD)/host/test_input_core
	cc -std=c11 -Wall -Wextra -Werror -Isrc tests/test_gesture_core.c src/gesture_core.c -o $(BUILD)/host/test_gesture_core
	$(BUILD)/host/test_gesture_core
	cc -std=c11 -Wall -Wextra -Werror -Dmemset=lm_memset -Dmemcpy=lm_memcpy -Dmemmove=lm_memmove \
	   -Dmemcmp=lm_memcmp tests/test_libc_min.c src/libc_min.c -o $(BUILD)/host/test_libc_min
	$(BUILD)/host/test_libc_min
	@# dimmer.c and controls.c with the host stubs
	cc $(HOST_TEST_FLAGS) tests/test_dimmer.c tests/host_stubs.c tests/host_boot.c src/dimmer.c \
	   src/dim_core.c src/bootif.c -o $(BUILD)/host/test_dimmer
	$(BUILD)/host/test_dimmer
	cc $(HOST_TEST_FLAGS) tests/test_controls.c tests/host_stubs.c tests/host_boot.c src/controls.c \
	   src/dimmer.c src/dim_core.c src/input_core.c src/gesture_core.c src/bootif.c \
	   -o $(BUILD)/host/test_controls
	$(BUILD)/host/test_controls
	cc $(HOST_CFLAGS) tests/test_store.c src/store.c -o $(BUILD)/host/test_store
	$(BUILD)/host/test_store
	cc $(HOST_CFLAGS) -Wno-unused-function -DRING_HIGH_TEST tests/test_store.c src/store.c -o $(BUILD)/host/test_store_high
	$(BUILD)/host/test_store_high
	cc $(HOST_CFLAGS) -Wno-unused-function -DRING_HIGH_TEST -DSCAN_OFF=1024 tests/test_store.c src/store.c -o $(BUILD)/host/test_store_high_code
	$(BUILD)/host/test_store_high_code
	@# the write range linked as the app's settings region (absolute symbols: no PIE)
	c++ -std=c++11 -Wall -Wextra -Werror -Wno-attributes -Wno-int-to-pointer-cast -fno-pie -no-pie -Itests/host \
	   -Isrc -x c++ src/flash_drv.c tests/test_flash_drv.cpp -o $(BUILD)/host/test_flash_drv \
	   -Wl,--defsym,_flash_write_start=0x08004000 -Wl,--defsym,_flash_write_end=0x08005000
	$(BUILD)/host/test_flash_drv
	@# ota_proto.h v2 (shared with the bootloaders and the ZTU): with the host mailbox and with the
	@# chip's address
	cc $(HOST_CFLAGS) tests/test_ota_proto.c -o $(BUILD)/host/test_ota_proto
	$(BUILD)/host/test_ota_proto
	cc -std=c11 -Wall -Wextra -Werror -Isrc tests/test_ota_proto.c -o $(BUILD)/host/test_ota_proto_chip
	$(BUILD)/host/test_ota_proto_chip
	@# tuya.c with bootif.c (offers, identity, confirmation) in every build kind
	$(call tuya_test,normal,)
	$(call tuya_test,fwtest,-DFW_TEST=7)
	$(call tuya_test,prev,-DFW_PREV=1 -DFW_TEST=99)
	$(call tuya_test,sim,-DSIM_ZC=1)
	$(call tuya_test,measure,-DMEASURE=1 -DFW_VERSION=0x8F)
	$(call tuya_test,diag,-DDIAG_REPORT=1 -DFW_VERSION=0x8E)
	@# the link watchdog (link_core.h through tuya.c and bootif.c) in every build kind and B11's
	$(call link_wdg_test,normal,)
	$(call link_wdg_test,b11,-DLINK_WDG_TEST=1 -DFW_PREV=1 -DFW_TEST=99)
	$(call link_wdg_test,sim,-DSIM_ZC=1)
	$(call link_wdg_test,measure,-DMEASURE=1 -DFW_VERSION=0x8F)
	$(call link_wdg_test,diag,-DDIAG_REPORT=1 -DFW_VERSION=0x8E)
	cc $(HOST_CFLAGS) tests/test_uart.c src/uart.c -o $(BUILD)/host/test_uart
	$(BUILD)/host/test_uart
	cc $(HOST_CFLAGS) -DRING_HIGH=1 tests/test_health.c src/health.c -o $(BUILD)/host/test_health
	$(BUILD)/host/test_health
	cc $(HOST_CFLAGS) tests/test_contact_burst.c src/tuya.c src/dimmer.c src/dim_core.c \
	   -o $(BUILD)/host/test_contact_burst
	$(BUILD)/host/test_contact_burst
	@# the stock Zigbee firmware facts of docs/BOOTLOADER.md section 15 (skipped without STOCK_ZTU_DUMP)
	python3 tests/test_stock_ztu_dump.py
	python3 tests/test_tools.py
	@# the build number rules of src/tuya.h and this Makefile (what must not compile)
	python3 tests/test_build_rules.py
	$(MAKE) --no-print-directory test-boot

# The v2 bootloader on the PC (docs/BOOTLOADER.md section 11): boot/boot.c unmodified against
# the hook device header tests/boot/fake, the models in tests/boot/harness.c, synthetic images
# from tests/boot/mkimages.py. Twice: with a flash model (power cuts, pages that never verify),
# and with the real src/flash_drv.c against tests/host/flash_model.hpp (C++), linked with
# boot.ld's write range. Plain cc: no ARM toolchain, no zlib. Fixed mappings at the chip's
# addresses need Linux >= 4.17 (MAP_FIXED_NOREPLACE) and absolute symbols a non-PIE link.
BOOT_HOST      := $(BUILD)/host/boot
BOOT_HOST_CC   := cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -fno-pie -O1 -g
BOOT_RANGE     := -Wl,--defsym,_flash_write_start=0x08001000 -Wl,--defsym,_flash_write_end=0x08004800
test-boot:
	mkdir -p $(BOOT_HOST)
	python3 tests/boot/mkimages.py $(BOOT_HOST)/img
	$(BOOT_HOST_CC) -Itests/boot/fake -Isrc -Dmain=boot_main -c boot/boot.c -o $(BOOT_HOST)/boot.o
	$(BOOT_HOST_CC) -Itests/boot/fake -Isrc -c tests/boot/harness.c -o $(BOOT_HOST)/harness.o
	$(BOOT_HOST_CC) -no-pie $(BOOT_HOST)/harness.o $(BOOT_HOST)/boot.o -o $(BOOT_HOST)/harness
	$(BOOT_HOST_CC) -Itests/boot/fake -Isrc -DREAL_FLASH_DRV -c tests/boot/harness.c -o $(BOOT_HOST)/harness_drv.o
	c++ -std=c++11 -Wall -Wextra -Werror -Wno-attributes -Wno-int-to-pointer-cast -fno-pie -O1 -g -Itests/host \
	   -Isrc -Dcrc32_update=drv_crc32_update -x c++ -c src/flash_drv.c -o $(BOOT_HOST)/flash_drv.o
	c++ -std=c++11 -Wall -Wextra -Werror -fno-pie -O1 -g -Itests/host -Isrc -c tests/boot/drv_model.cpp \
	   -o $(BOOT_HOST)/drv_model.o
	c++ -no-pie $(BOOT_HOST)/harness_drv.o $(BOOT_HOST)/boot.o $(BOOT_HOST)/flash_drv.o \
	   $(BOOT_HOST)/drv_model.o $(BOOT_RANGE) -o $(BOOT_HOST)/harness_drv
	$(BOOT_HOST)/harness_drv $(BOOT_HOST)/img
	$(BOOT_HOST)/harness $(BOOT_HOST)/img

# The build directory and the defines of a build (make -s print-defs FW_TEST=7): for
# tests/test_build_rules.py, which compiles src/tuya.h with them.
print-defs:
	@echo $(BUILD) $(DEFS)

.PHONY: test test-boot print-defs
