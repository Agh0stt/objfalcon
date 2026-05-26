#!/bin/bash

FIXED=${FIXED:-./falconc}
FLR=./flr.o
EXAMPLES=examples

pass=0; fail=0; fail_list=""

for fl in $(ls $EXAMPLES/*.fl | sort); do
    name=$(basename $fl)
    out=$($FIXED $fl -o /tmp/t.s 2>/tmp/terr.txt && \
          as --32 /tmp/t.s -o /tmp/t.o 2>>/tmp/terr.txt && \
          ld --allow-multiple-definition -m elf_i386 $FLR /tmp/t.o -o /tmp/t 2>>/tmp/terr.txt && \
          timeout 3 /tmp/t 2>&1 | head -10)
    rc=$?
    err=$(cat /tmp/terr.txt | grep -v "^falconc:" | grep -v "warning:" | head -1)
    if [ $rc -eq 0 ] && [ -n "$out" -o -z "$err" ]; then
        echo "PASS $name: $(echo $out | tr '\n' ' ' | cut -c1-60)"
        pass=$((pass+1))
    else
        echo "FAIL $name: ${err:-exit $rc}"
        fail=$((fail+1))
        fail_list="$fail_list $name"
    fi
done

echo ""
echo "Results: $pass passed, $fail failed"
if [ -n "$fail_list" ]; then
    echo "Failed:$fail_list"
    exit 1
fi

exit 0
