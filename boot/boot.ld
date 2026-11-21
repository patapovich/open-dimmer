/*
******************************************************************************
**
**  File        : LinkerScript.ld
**
**  Abstract    : Linker script for the zdms16 bootloader: 4 KB at 0x08000000,
**                .RamFunc copied to RAM with .data
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

/* What src/flash_drv.c lets the bootloader write: the app area up to BOOT_APP_MAX (14 KB) and
 * the hidden flash after it (v3, src/ota_proto.h), never sector 0 and never the settings ring
 * between them (FLASH_HOLE_START..FLASH_HOLE_END, the bootloader's build flags). */
_flash_write_start = 0x08001000;
_flash_write_end   = 0x08008000;

/* Highest address of the user mode stack */
_estack = ORIGIN(RAM) + LENGTH(RAM);    /* end of RAM */
/*
  Generate a link error if heap and stack don't fit into RAM.
  These numbers affect the USED size of RAM
*/
_Min_Heap_Size = 0x0;   /* required amount of heap: 256 bytes */
_Min_Stack_Size = 0x200;  /* required amount of stack: 512 bytes */

/* Specify the memory areas */
MEMORY
{
  RAM (xrw)      : ORIGIN = 0x20000000, LENGTH = 3K - 32   /* last 32 bytes: OTA mailbox */
  FLASH (rx)     : ORIGIN = 0x08000000, LENGTH = 4K
  /* Last 4 KB sector: settings ring (src/store.c), never code. */
  SETTINGS (r)   : ORIGIN = 0x08004000, LENGTH = 4K
}

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
  } >FLASH

  /* boot_id_t (src/ota_proto.h) right after the 4-word vector table, at 0x08000010: the app
   * reads it there. Its own section, so boot.bin stays contiguous. */
  .boot_id :
  {
    KEEP(*(.boot_id))
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
  } >FLASH

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

ASSERT(ADDR(.boot_id) == 0x08000010 && SIZEOF(.boot_id) == 16 && k_boot_id == 0x08000010,
       "boot_id_t must sit right after the 4-word vector table")
ASSERT(_flash_write_start == ORIGIN(FLASH) + LENGTH(FLASH) && _flash_write_end == 0x08005000 + 0x3000,
       "the bootloader writes the app area 0x08001000..0x08004800 and the hidden flash to 0x08008000 only")
