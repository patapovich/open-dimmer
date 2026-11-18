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
# make FW_TEST=n (1..99): a spare test build with the build number FW_BUILD + n (FW_BUILD in
# src/tuya.h is the release build; docs/BOOTLOADER.md section 2). It stays below the next release
# (FW_BUILD + 100), so a unit that ran it always takes that release. The version byte stays: test
# builds bump only the build. Its own BUILD directory (build-test<n>, as a VERSION_BYTE build
# needs one), and tools/mkimage.py checks that the image's build is the release build + n.
FW_TEST  ?=
ifneq ($(FW_TEST),)
  ifeq ($(filter $(FW_TEST),$(shell seq 1 99)),)
    $(error FW_TEST=$(FW_TEST): a test build takes 1..99 (FW_BUILD + n stays below the next release))
  endif
  ifeq ($(SIM),1)
    $(error FW_TEST=$(FW_TEST) with SIM=1: a SIM build has the fixed build 0xFFFE)
  endif
endif
BUILD    ?= build$(if $(filter 1,$(SIM)),-sim)$(if $(filter 1,$(MEASURE)),-measure)$(if $(filter 1,$(DIAG)),-diag)$(if $(FW_TEST),-test$(FW_TEST))

CC       := arm-none-eabi-gcc
OBJCOPY  := arm-none-eabi-objcopy
SIZE     := arm-none-eabi-size

LL_SRCS  := py32f0xx_ll_rcc.c py32f0xx_ll_utils.c py32f0xx_ll_gpio.c py32f0xx_ll_usart.c \
            py32f0xx_ll_tim.c py32f0xx_ll_exti.c
SRCS     := $(wildcard src/*.c) \
            $(SDK)/CMSIS/Device/PY32F0xx/Source/system_py32f0xx.c \
            $(addprefix $(SDK)/PY32F0xx_LL_Driver/Src/,$(LL_SRCS))
ASRCS    := $(SDK)/CMSIS/Device/PY32F0xx/Source/gcc/startup_py32f002a.s
LDSCRIPT := py32f002a_app.ld

INCLUDES := -Isrc -I$(SDK)/CMSIS/Core/Include -I$(SDK)/CMSIS/Device/PY32F0xx/Include \
            -I$(SDK)/PY32F0xx_LL_Driver/Inc
DEFS     := -DPY32F002Ax5 -DUSE_FULL_LL_DRIVER -DSIM_ZC=$(SIM) -DMEASURE=$(MEASURE) -DDIAG_REPORT=$(DIAG) $(if $(VERSION_BYTE),-DFW_VERSION=$(VERSION_BYTE),$(if $(filter 1,$(MEASURE)),-DFW_VERSION=0x8F,$(if $(filter 1,$(DIAG)),-DFW_VERSION=0x8E))) \
            $(if $(FW_TEST),-DFW_TEST=$(FW_TEST))
ARCH     := -mcpu=cortex-m0plus -mthumb
CFLAGS   := $(ARCH) $(DEFS) $(INCLUDES) -Os -g -Wall -Wextra -Wno-unused-parameter \
            -ffunction-sections -fdata-sections -std=c11 -MMD -MP
LDFLAGS  := $(ARCH) -T$(LDSCRIPT) -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(PROJECT).map \
            --specs=nano.specs --specs=nosys.specs

OBJS     := $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o))) $(addprefix $(BUILD)/,$(notdir $(ASRCS:.s=.o)))

# Bootloader (4 KB at 0x08000000): the same for every app build. BOOTLOADER keeps the
# shared src/flash_drv.c as the bootloader was released with (boot.bin byte-identical) until
# the bootloader batch (fix plan phase 3) takes the app's flash driver fixes in.
BOOT_BUILD := build-boot
BOOT_SRCS  := boot/boot.c src/flash_drv.c
BOOT_OBJS  := $(addprefix $(BOOT_BUILD)/,$(notdir $(BOOT_SRCS:.c=.o)))
BOOT_CFLAGS := $(CFLAGS) -DBOOTLOADER -ffreestanding -fno-tree-loop-distribute-patterns
BOOT_LDFLAGS := $(ARCH) -Tboot/boot.ld -Wl,--gc-sections -Wl,-Map=$(BOOT_BUILD)/boot.map -nostdlib -lgcc

vpath %.c $(sort $(dir $(SRCS) $(BOOT_SRCS)))
vpath %.s $(sort $(dir $(ASRCS)))

# App image (what the Zigbee module carries and sends), bootloader, and the
# whole flash for the first SWD flashing.
all: $(BUILD)/$(PROJECT).img $(BOOT_BUILD)/boot.bin $(BUILD)/$(PROJECT)_full.bin

$(BUILD) $(BOOT_BUILD):
	mkdir -p $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) $(ARCH) -c $< -o $@

$(BOOT_BUILD)/%.o: %.c | $(BOOT_BUILD)
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

$(BOOT_BUILD)/%.o: %.s | $(BOOT_BUILD)
	$(CC) $(ARCH) -c $< -o $@

$(BUILD)/$(PROJECT).elf: $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@
	$(SIZE) $@

$(BUILD)/$(PROJECT).bin: $(BUILD)/$(PROJECT).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/$(PROJECT).img: $(BUILD)/$(PROJECT).bin tools/mkimage.py tools/fwimage.py
	python3 tools/mkimage.py $< $@ $(if $(FW_TEST),--test $(FW_TEST))

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

$(BUILD)/$(PROJECT)_full.bin: $(BUILD)/$(PROJECT).img $(BOOT_BUILD)/boot.bin tools/mkimage.py tools/fwimage.py
	python3 tools/mkimage.py $< --full $(BOOT_BUILD)/boot.bin $@

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
# (DBGMCU APBFZ1 DBG_IWDG_STOP): the v2 bootloader starts it at every boot.
PYOCD_T        = -t py32f002ax5 --pack $(PACK) -f 400000
BOOT_WORD_ZERO := -c "write32 0x20000BF8 0" -c "write32 0x20000BFC 0"
IWDG_FREEZE    := -c "write32 0x40015808 0x1000"
flash flash-all: $(BUILD)/$(PROJECT)_full.bin
	@[ "$$(stat -c %s $<)" -le 16384 ] || { \
	    echo "$< reaches the settings sector (0x08004000), which pyocd would erase: tools/swd/install.sh ERASE_SETTINGS=1" >&2; exit 1; }
	-$(PYOCD) commander $(PYOCD_T) -O connect_mode=attach $(IWDG_FREEZE) $(BOOT_WORD_ZERO) >/dev/null 2>&1
	$(PYOCD) load $(PYOCD_T) --format bin --base-address 0x08000000 $<
	$(PYOCD) commander $(PYOCD_T) -O connect_mode=attach $(IWDG_FREEZE) -c halt $(BOOT_WORD_ZERO) -c reset

clean:
	rm -rf build build-sim build-boot

# Header dependencies (an object is rebuilt when a header it includes changes)
-include $(OBJS:.o=.d) $(BOOT_OBJS:.o=.d)

.PHONY: all flash flash-all clean

# phase.c on the PC (tests/test_phase.c) against fake registers (tests/fake), once per
# build variant. Its fader setters are renamed to hooks that check the interrupts are off.
FADER_HOOKS := $(foreach f,set set_floor set_profile hold_start hold_start_from_dark hold_stop hold_cancel,\
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
	cc $(HOST_TEST_FLAGS) tests/test_dimmer.c tests/host_stubs.c src/dimmer.c src/dim_core.c \
	   -o $(BUILD)/host/test_dimmer
	$(BUILD)/host/test_dimmer
	cc $(HOST_TEST_FLAGS) tests/test_controls.c tests/host_stubs.c src/controls.c src/dimmer.c \
	   src/dim_core.c src/input_core.c src/gesture_core.c -o $(BUILD)/host/test_controls
	$(BUILD)/host/test_controls
	cc $(HOST_CFLAGS) tests/test_store.c src/store.c -o $(BUILD)/host/test_store
	$(BUILD)/host/test_store
	c++ -std=c++11 -Wall -Wextra -Werror -Wno-attributes -Wno-int-to-pointer-cast -Itests/host -Isrc \
	   -x c++ src/flash_drv.c tests/test_flash_drv.cpp -o $(BUILD)/host/test_flash_drv
	$(BUILD)/host/test_flash_drv
	c++ -std=c++11 -Wall -Wextra -Werror -Wno-attributes -Wno-int-to-pointer-cast -Itests/host -Isrc \
	   -DBOOTLOADER -x c++ src/flash_drv.c tests/test_flash_drv.cpp \
	   -o $(BUILD)/host/test_flash_drv_boot
	$(BUILD)/host/test_flash_drv_boot
	@# ota_proto.h v2 (shared with the bootloaders and the ZTU): with the host mailbox and with the
	@# chip's address
	cc $(HOST_CFLAGS) tests/test_ota_proto.c -o $(BUILD)/host/test_ota_proto
	$(BUILD)/host/test_ota_proto
	cc -std=c11 -Wall -Wextra -Werror -Isrc tests/test_ota_proto.c -o $(BUILD)/host/test_ota_proto_chip
	$(BUILD)/host/test_ota_proto_chip
	cc $(HOST_CFLAGS) tests/test_tuya.c src/tuya.c -o $(BUILD)/host/test_tuya
	$(BUILD)/host/test_tuya
	cc $(HOST_CFLAGS) -DMEASURE=1 -DFW_VERSION=0x8F tests/test_tuya.c src/tuya.c -o $(BUILD)/host/test_tuya_measure
	$(BUILD)/host/test_tuya_measure
	cc $(HOST_CFLAGS) -DDIAG_REPORT=1 -DFW_VERSION=0x8E tests/test_tuya.c src/tuya.c -o $(BUILD)/host/test_tuya_diag
	$(BUILD)/host/test_tuya_diag
	cc $(HOST_CFLAGS) tests/test_uart.c src/uart.c -o $(BUILD)/host/test_uart
	$(BUILD)/host/test_uart
	cc $(HOST_CFLAGS) tests/test_health.c src/health.c -o $(BUILD)/host/test_health
	$(BUILD)/host/test_health
	cc $(HOST_CFLAGS) tests/test_contact_burst.c src/tuya.c src/dimmer.c src/dim_core.c \
	   -o $(BUILD)/host/test_contact_burst
	$(BUILD)/host/test_contact_burst
	python3 tests/test_tools.py

.PHONY: test
