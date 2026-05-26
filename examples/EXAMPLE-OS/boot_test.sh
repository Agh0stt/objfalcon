#!/usr/bin/env bash
set -euo pipefail

compiler="${TMPDIR:-/tmp}/falconc_boot_$$"
gcc -O2 -Wall -o "$compiler" ../../falconc.c
make FALCONC="$compiler"

dump="${TMPDIR:-/tmp}/falcon_vga_$$.bin"
log="${TMPDIR:-/tmp}/falcon_qemu_$$.log"

(
    sleep 1
    printf 'pmemsave 0xb8000 160 "%s"\n' "$dump"
    printf 'quit\n'
) | qemu-system-i386 -kernel kernel.elf -display none -monitor stdio -serial none >"$log" 2>&1

if [ ! -s "$dump" ]; then
    echo "QEMU boot test failed: VGA dump was not created"
    echo "Monitor log: $log"
    exit 1
fi

screen=$(
    od -An -tu1 -N160 "$dump" |
    awk '{ for (i = 1; i <= NF; i++) { n++; if (n % 2 == 1 && $i >= 32 && $i < 127) printf "%c", $i } } END { print "" }'
)

echo "$screen"

case "$screen" in
    *"Hello, World!"*)
        echo "QEMU boot test passed: VGA contains Hello, World!"
        ;;
    *)
        echo "QEMU boot test failed: expected text was not found in VGA memory"
        echo "VGA dump: $dump"
        echo "Monitor log: $log"
        exit 1
        ;;
esac
