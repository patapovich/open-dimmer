# Own firmware for the PY32F002A dimming MCU of the AVATTO ZDMS16-2.
# SDK (CMSIS + LL drivers) is used straight from the py32f0-template checkout.

SDK      ?= $(HOME)/projects/py32f0-template/Libraries
PACK     ?= $(HOME)/projects/zdms16-py32/pack/Puya.PY32F0xx_DFP.1.1.7-opt.pack
PYOCD    ?= $(HOME)/projects/zdms16-py32/.venv/bin/pyocd
PROJECT  := zdms16_py32
# make SIM=1: virtual 50 Hz zero cross from TIM16 instead of PA4 (bench, no mains)
SIM      ?= 0
BUILD    := build$(if $(filter 1,$(SIM)),-sim)

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
DEFS     := -DPY32F002Ax5 -DUSE_FULL_LL_DRIVER -DSIM_ZC=$(SIM)
ARCH     := -mcpu=cortex-m0plus -mthumb
CFLAGS   := $(ARCH) $(DEFS) $(INCLUDES) -Os -g -Wall -Wextra -Wno-unused-parameter \
            -ffunction-sections -fdata-sections -std=c11 -MMD -MP
LDFLAGS  := $(ARCH) -T$(LDSCRIPT) -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(PROJECT).map \
            --specs=nano.specs --specs=nosys.specs

OBJS     := $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o))) $(addprefix $(BUILD)/,$(notdir $(ASRCS:.s=.o)))

# Bootloader (2 KB at 0x08000000): the same for every app build
BOOT_BUILD := build-boot
BOOT_SRCS  := boot/boot.c src/flash_drv.c
BOOT_OBJS  := $(addprefix $(BOOT_BUILD)/,$(notdir $(BOOT_SRCS:.c=.o)))
BOOT_CFLAGS := $(CFLAGS) -ffreestanding -fno-tree-loop-distribute-patterns
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

$(BUILD)/$(PROJECT).img: $(BUILD)/$(PROJECT).bin tools/mkimage.py
	python3 tools/mkimage.py $< $@

$(BOOT_BUILD)/boot.elf: $(BOOT_OBJS)
	$(CC) $(BOOT_LDFLAGS) $^ -o $@
	$(SIZE) $@

$(BOOT_BUILD)/boot.bin: $(BOOT_BUILD)/boot.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/$(PROJECT)_full.bin: $(BUILD)/$(PROJECT).img $(BOOT_BUILD)/boot.bin
	python3 tools/mkimage.py $< --full $(BOOT_BUILD)/boot.bin $@

# The module must be powered from the probe, never from mains, while wired.
# flash: the app only (the bootloader stays); flash-all: bootloader + app.
flash: $(BUILD)/$(PROJECT).img
	$(PYOCD) load -t py32f002ax5 --pack $(PACK) -f 400000 --format bin --base-address 0x08000800 $<
	$(PYOCD) reset -t py32f002ax5 --pack $(PACK) -f 400000

flash-all: $(BUILD)/$(PROJECT)_full.bin
	$(PYOCD) load -t py32f002ax5 --pack $(PACK) -f 400000 --format bin --base-address 0x08000000 $<
	$(PYOCD) reset -t py32f002ax5 --pack $(PACK) -f 400000

clean:
	rm -rf build build-sim build-boot

# Header dependencies (an object is rebuilt when a header it includes changes)
-include $(OBJS:.o=.d) $(BOOT_OBJS:.o=.d)

.PHONY: all flash flash-all clean

# Hardware-free logic on the PC
test:
	mkdir -p $(BUILD)/host
	cc -std=c11 -Wall -Wextra -Werror -Isrc tests/test_dim_core.c src/dim_core.c -o $(BUILD)/host/test_dim_core
	$(BUILD)/host/test_dim_core
	cc -std=c11 -Wall -Wextra -Werror -Isrc tests/test_input_core.c src/input_core.c -o $(BUILD)/host/test_input_core
	$(BUILD)/host/test_input_core

.PHONY: test
