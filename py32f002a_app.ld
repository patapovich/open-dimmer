/*
******************************************************************************
**
**  File        : LinkerScript.ld
**
**  Abstract    : Linker script for PY32F002A series (zdms16 app: 12 KB code +
**                4 KB settings sector, .RamFunc copied to RAM with .data;
**                app at 0x08001000 behind the bootloader, header at +0xC0)
**                Set heap size, stack size and stack location according
**                to application requirements.
**                Set memory bank area and size if external memory is used.
**
**  Distribution: The file is distributed “as is,” without any warranty
**                of any kind.
**
*****************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)

_settings_start = ORIGIN(SETTINGS);
_settings_end   = ORIGIN(SETTINGS) + LENGTH(SETTINGS);

/* Write range of the shared flash driver (src/flash_drv.c, docs/BOOTLOADER.md section 9,
   item 7): the app writes its settings ring and nothing else, never the bootloader sector
   or its own code. boot/boot.ld gives the bootloader the app area instead. */
_flash_write_start = ORIGIN(SETTINGS);
_flash_write_end   = ORIGIN(SETTINGS) + LENGTH(SETTINGS);

/* Highest address of the user mode stack */
_estack = ORIGIN(RAM) + LENGTH(RAM);    /* end of RAM */
/*
  Generate a link error if heap and stack don't fit into RAM.
  These numbers affect the USED size of RAM
*/
_Min_Heap_Size = 0x100;   /* required amount of heap: 256 bytes */
_Min_Stack_Size = 0x200;  /* required amount of stack: 512 bytes */

/* Specify the memory areas */
MEMORY
{
  RAM (xrw)      : ORIGIN = 0x20000000, LENGTH = 3K - 32   /* last 32 bytes: OTA mailbox */
  FLASH (rx)     : ORIGIN = 0x08001000, LENGTH = 12K   /* after the 4 KB bootloader */
  /* Last 4 KB sector: settings ring (src/store.c), never code. */
  SETTINGS (r)   : ORIGIN = 0x08004000, LENGTH = 4K
  /* The hidden flash past the 20 KB (a v3 bootloader, docs/BOOTLOADER.md section 18): used only
     when the build points HIDDEN_DATA at it (make HIDDEN=1). */
  FLASH_HI (rx)  : ORIGIN = 0x08005000, LENGTH = 12K
}

/* Where the constant data goes: the app area, or the hidden flash (HIDDEN=1). Constants only,
   written once per update: the settings ring stays in the guaranteed flash. More (main(), 4.9 KB
   with LTO) would fit there, but the image's app area is padded to 14 KB before the hidden bytes
   and the ZTU carries the padding too: first a ZTU that stores the image without it. */
REGION_ALIAS("HIDDEN_DATA", FLASH);

/* Define output sections */
SECTIONS
{
  /* SRAM vector table */
  .ram_vector :
  {
    *(.ram_vector)
  } >RAM

  /* The startup code goes first into FLASH */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) /* Startup code */
    . = ALIGN(4);
    KEEP(*(.app_header)) /* ota_proto.h app_header_t, right after the vectors */
    . = ALIGN(4);
  } >FLASH

  /* The program code and other data goes into FLASH */
  .text :
  {
    . = ALIGN(4);
    *(.text)           /* .text sections (code) */
    *(.text*)          /* .text* sections (code) */
    *(.glue_7)         /* glue arm to thumb code */
    *(.glue_7t)        /* glue thumb to arm code */
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;        /* define a global symbols at end of code */
  } >FLASH

  /* Constant data goes into FLASH */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)         /* .rodata sections (constants, strings, etc.) */
    *(.rodata*)        /* .rodata* sections (constants, strings, etc.) */
    . = ALIGN(4);
  } >HIDDEN_DATA

  .ARM.extab   : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array     :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH
  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH
  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  /* used by the startup to initialize data */
  _sidata = LOADADDR(.data);

  /* Initialized data sections goes into RAM, load LMA copy after code */
  .data : 
  {
    . = ALIGN(4);
    _sdata = .;        /* create a global symbol at data start */
    *(.data)           /* .data sections */
    *(.RamFunc)        /* code that must run from RAM (flash programming) */
    *(.RamFunc*)
    *(.data*)          /* .data* sections */

    . = ALIGN(4);
    _edata = .;        /* define a global symbol at data end */
  } >RAM AT> FLASH

  
  /* Uninitialized data section */
  . = ALIGN(4);
  .bss :
  {
    /* This is used by the startup in order to initialize the .bss secion */
    _sbss = .;         /* define a global symbol at bss start */
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)

    . = ALIGN(4);
    _ebss = .;         /* define a global symbol at bss end */
    __bss_end__ = _ebss;
  } >RAM

  /* User_heap_stack section, used to check that there is enough RAM left */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /* Remove information from the standard libraries */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}



/* The settings records may lie anywhere in the last sector above this app's own image (src/store.c
 * scan rule, docs/BOOTLOADER.md section 1): a ring that moved up (RING_HIGH) still finds the
 * records of the ring below it; code is never taken for a record. */
_store_scan_start = MAX(0x08004000, ALIGN(LOADADDR(.data) + SIZEOF(.data), 128));

ASSERT(g_app_header == ORIGIN(FLASH) + 0xC0, "app header must follow the 48-entry vector table")
ASSERT(_store_scan_start <= _settings_start && _store_scan_start % 128 == 0,
       "the settings scan starts above the code and at or below the ring")
ASSERT(_flash_write_start >= ORIGIN(FLASH) + LENGTH(FLASH) && _flash_write_start < _flash_write_end &&
       _flash_write_end <= 0x08005000 && _flash_write_start % 128 == 0 && _flash_write_end % 128 == 0,
       "flash write range: whole pages above the app's code, inside the 20 KB flash")
