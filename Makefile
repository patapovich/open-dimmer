# Own firmware for the PY32F002A dimming MCU of the AVATTO ZDMS16-2.
# SDK (CMSIS + LL drivers) is used straight from the py32f0-template checkout.

SDK      ?= $(HOME)/projects/py32f0-template/Libraries
PACK     ?= $(HOME)/projects/zdms16-py32/pack/Puya.PY32F0xx_DFP.1.1.7-opt.pack
PYOCD    ?= $(HOME)/projects/zdms16-py32/.venv/bin/pyocd
PROJECT  := zdms16_py32
BUILD    := build

CC       := arm-none-eabi-gcc
OBJCOPY  := arm-none-eabi-objcopy
SIZE     := arm-none-eabi-size

LL_SRCS  := py32f0xx_ll_rcc.c py32f0xx_ll_utils.c py32f0xx_ll_gpio.c py32f0xx_ll_usart.c \
            py32f0xx_ll_tim.c py32f0xx_ll_exti.c
SRCS     := $(wildcard src/*.c) \
            $(SDK)/CMSIS/Device/PY32F0xx/Source/system_py32f0xx.c \
            $(addprefix $(SDK)/PY32F0xx_LL_Driver/Src/,$(LL_SRCS))
ASRCS    := $(SDK)/CMSIS/Device/PY32F0xx/Source/gcc/startup_py32f002a.s
LDSCRIPT := $(SDK)/LDScripts/py32f002ax5.ld

INCLUDES := -Isrc -I$(SDK)/CMSIS/Core/Include -I$(SDK)/CMSIS/Device/PY32F0xx/Include \
            -I$(SDK)/PY32F0xx_LL_Driver/Inc
DEFS     := -DPY32F002Ax5 -DUSE_FULL_LL_DRIVER
ARCH     := -mcpu=cortex-m0plus -mthumb
CFLAGS   := $(ARCH) $(DEFS) $(INCLUDES) -Os -g -Wall -Wextra -Wno-unused-parameter \
            -ffunction-sections -fdata-sections -std=c11
LDFLAGS  := $(ARCH) -T$(LDSCRIPT) -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(PROJECT).map \
            --specs=nano.specs --specs=nosys.specs

OBJS     := $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o))) $(addprefix $(BUILD)/,$(notdir $(ASRCS:.s=.o)))
vpath %.c $(sort $(dir $(SRCS)))
vpath %.s $(sort $(dir $(ASRCS)))

all: $(BUILD)/$(PROJECT).bin

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) $(ARCH) -c $< -o $@

$(BUILD)/$(PROJECT).elf: $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@
	$(SIZE) $@

$(BUILD)/$(PROJECT).bin: $(BUILD)/$(PROJECT).elf
	$(OBJCOPY) -O binary $< $@

# The module must be powered from the probe, never from mains, while wired.
flash: $(BUILD)/$(PROJECT).bin
	$(PYOCD) load -t py32f002ax5 --pack $(PACK) -f 400000 --base-address 0x08000000 $<
	$(PYOCD) reset -t py32f002ax5 --pack $(PACK) -f 400000

clean:
	rm -rf $(BUILD)

.PHONY: all flash clean
