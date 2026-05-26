# Makefile — Falcon Hello World kernel

FALCONC  = ../../falconc
AS       = as --32
LD       = ld -m elf_i386

all: kernel.elf

falconc:
	gcc -O2 -o ../../falconc ../../falconc.c

kernel.s: kernel.fl
	$(FALCONC) kernel.fl --freestanding -o kernel.s

kernel.o: kernel.s
	$(AS) kernel.s -o kernel.o

boot.o: boot.s
	$(AS) boot.s -o boot.o

kernel.elf: boot.o kernel.o link.ld
	$(LD) -T link.ld boot.o kernel.o -o kernel.elf

run: kernel.elf
	qemu-system-i386 -kernel kernel.elf

clean:
	rm -f *.o kernel.s kernel.elf

.PHONY: all run clean falconc
