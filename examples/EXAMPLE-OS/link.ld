/* link.ld — Falcon bare-metal kernel linker script */
OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

SECTIONS {
    . = 0x100000;       /* load at 1 MB — standard multiboot load address */

    .text ALIGN(4096) : {
        *(.multiboot)   /* multiboot header must be in first 8KB */
        *(.text)
    }

    .rodata ALIGN(4096) : {
        *(.rodata*)
    }

    .data ALIGN(4096) : {
        *(.data)
    }

    .bss ALIGN(4096) : {
        *(COMMON)
        *(.bss)
    }
}
