CC      = gcc
CFLAGS  = -O2 -Wno-unused-function -Wno-unused-variable -Wno-unused-result

all: ofcc ofcr.o

ofcc: ofcc.c
	$(CC) $(CFLAGS) -o ofcc ofcc.c

ofcr.o: ofcr.s
	as --32 ofcr.s -o ofcr.o

# run a .ofc or .fl file: make run F=hello.ofc
run: ofcc ofcr.o
	./ofcc $(F) -o /tmp/_ofc_out.s
	as --32 /tmp/_ofc_out.s -o /tmp/_ofc_out.o
	ld -m elf_i386 --allow-multiple-definition ofcr.o /tmp/_ofc_out.o -o /tmp/_ofc_bin
	/tmp/_ofc_bin

# 64-bit run
run64: ofcc ofcr.o
	./ofcc $(F) -arch x86-64-linux -o /tmp/_ofc_out.s
	as /tmp/_ofc_out.s -o /tmp/_ofc_out.o
	ld /tmp/_ofc_out.o -o /tmp/_ofc_bin
	/tmp/_ofc_bin

test: ofcc ofcr.o
	@pass=0; fail=0; \
	for f in examples/*.fl examples/ofc/*.ofc; do \
	    ./ofcc "$$f" -o /tmp/t.s 2>/tmp/err.txt && \
	    as --32 /tmp/t.s -o /tmp/t.o 2>>/tmp/err.txt && \
	    ld -m elf_i386 --allow-multiple-definition ofcr.o /tmp/t.o -o /tmp/t 2>>/tmp/err.txt && \
	    /tmp/t >/dev/null 2>&1 && pass=$$((pass+1)) || { fail=$$((fail+1)); echo "FAIL: $$f"; cat /tmp/err.txt; }; \
	done; \
	echo "--- $$pass passed, $$fail failed ---"

clean:
	rm -f ofcc ofcr.o /tmp/_ofc_out.s /tmp/_ofc_out.o /tmp/_ofc_bin

.PHONY: all run run64 test clean
