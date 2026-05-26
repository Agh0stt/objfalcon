# boot.s — Multiboot header + entry point for Falcon kernel
# Assembled with: as --32 boot.s -o boot.o

.set MAGIC,    0x1BADB002
.set FLAGS,    0x0
.set CHECKSUM, -(MAGIC + FLAGS)

    .section .multiboot
    .align 4
    .long MAGIC
    .long FLAGS
    .long CHECKSUM

    .section .text
.globl _start
_start:
    cli                  # disable interrupts
    call  main           # jump into Falcon kernel
.hang:
    hlt
    jmp   .hang
