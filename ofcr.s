# flr.s — Falcon Runtime
# Pure i386 assembly, no libc, Linux int 0x80 syscalls only.
#
# Assemble:  as --32 flr.s -o flr.o
# Link user: ld -m elf_i386 flr.o code.o -o prog
#
# Public symbols:
#   _flr_print_int(n:int)           -> void
#   _flr_print_str(s:str)           -> void
#   _flr_str_len(s:str)             -> int
#   _flr_strlen(s:str)              -> int   (alias)
#   _flr_str_eq(a:str, b:str)       -> int   (1=equal)
#   _flr_int_to_str(n:int)          -> str   (static buf – use immediately)
#   _flr_str_to_int(s:str)          -> int
#   _flr_memset(ptr, val, n)        -> void
#   _flr_memcpy(dst, src, n)        -> void
#   _flr_abs(n:int)                 -> int
#   _flr_min(a:int, b:int)          -> int
#   _flr_max(a:int, b:int)          -> int
#   _flr_exit(code:int)             -> void
#   _flr_assert(cond:int, msg:str)  -> void
#   _flr_alloc(size:int)            -> ptr   (bump allocator, 1 MB heap)
#   _flr_free(ptr)                  -> void  (no-op)

    .section .text

# ─────────────────────────────────────────────────────────────────────
# _start — ELF entry point
#   Calls main(), then exits with its return value (0 for void main).
# ─────────────────────────────────────────────────────────────────────
.globl _start
_start:
    xorl  %ebp, %ebp            # mark outermost frame (ABI convention)
    call  main
    movl  %eax, %ebx            # exit code = return value of main
    movl  $1,   %eax            # sys_exit
    int   $0x80

# ─────────────────────────────────────────────────────────────────────
# memset(ptr, val, n)  — cdecl, clobbers eax/ecx/edi
# ─────────────────────────────────────────────────────────────────────
.globl _flr_memset
_flr_memset:
    pushl %ebp
    movl  %esp, %ebp
    pushl %edi
    movl  8(%ebp),  %edi    # dst
    movl  12(%ebp), %eax    # fill byte
    movl  16(%ebp), %ecx    # count
    rep stosb
    popl  %edi
    leave
    ret

# ─────────────────────────────────────────────────────────────────────
# memcpy(dst, src, n)
# ─────────────────────────────────────────────────────────────────────
.globl _flr_memcpy
_flr_memcpy:
    pushl %ebp
    movl  %esp, %ebp
    pushl %esi
    pushl %edi
    movl  8(%ebp),  %edi    # dst
    movl  12(%ebp), %esi    # src
    movl  16(%ebp), %ecx    # count
    rep movsb
    popl  %edi
    popl  %esi
    leave
    ret

# ─────────────────────────────────────────────────────────────────────
# print_str(s)  — writes s + newline to stdout (fd 1)
# ─────────────────────────────────────────────────────────────────────
.globl _flr_print_str
_flr_print_str:
    pushl %ebp
    movl  %esp, %ebp
    pushl %esi
    movl  8(%ebp), %esi     # s
    movl  %esi, %ecx
.Lfps_l:
    cmpb  $0, (%ecx)
    je    .Lfps_d
    incl  %ecx
    jmp   .Lfps_l
.Lfps_d:
    subl  %esi, %ecx        # ecx = length
    movl  %ecx, %edx
    movl  %esi, %ecx
    movl  $4,   %eax        # sys_write
    movl  $1,   %ebx        # fd=stdout
    int   $0x80
    # write newline
    movl  $4,   %eax
    movl  $1,   %ebx
    leal  .Lflr_nl, %ecx
    movl  $1,   %edx
    int   $0x80
    popl  %esi
    leave
    ret

# ─────────────────────────────────────────────────────────────────────
# print_int(n)  — converts n, writes decimal + newline to stdout
# ─────────────────────────────────────────────────────────────────────
.globl _flr_print_int
_flr_print_int:
    pushl %ebp
    movl  %esp, %ebp
    pushl %esi
    pushl %edi
    movl  8(%ebp), %eax
    leal  .Lflr_ibuf+11, %edi
    movb  $10, (%edi)           # newline sentinel at end
    decl  %edi
    testl %eax, %eax
    jge   .Lfpi_pos
    negl  %eax
    movl  $1, %esi              # negative flag
    jmp   .Lfpi_l
.Lfpi_pos:
    xorl  %esi, %esi
.Lfpi_l:
    movl  $10, %ecx
    xorl  %edx, %edx
    divl  %ecx
    addb  $48, %dl
    movb  %dl, (%edi)
    decl  %edi
    testl %eax, %eax
    jne   .Lfpi_l
    testl %esi, %esi
    je    .Lfpi_nom
    movb  $45, (%edi)           # '-'
    decl  %edi
.Lfpi_nom:
    incl  %edi
    leal  .Lflr_ibuf+12, %edx
    subl  %edi, %edx            # length including newline
    movl  %edi, %ecx
    movl  $4, %eax
    movl  $1, %ebx
    int   $0x80
    popl  %edi
    popl  %esi
    leave
    ret

# ─────────────────────────────────────────────────────────────────────
# exit(code)
# ─────────────────────────────────────────────────────────────────────
.globl _flr_exit
_flr_exit:
    movl  4(%esp), %ebx
    movl  $1, %eax
    int   $0x80

# ─────────────────────────────────────────────────────────────────────
# strlen(s) / str_len(s)  -> int
# ─────────────────────────────────────────────────────────────────────
.globl _flr_strlen
_flr_strlen:
    movl  4(%esp), %ecx
    movl  %ecx, %eax
.Lflrsl:
    cmpb  $0, (%ecx)
    je    .Lflrsld
    incl  %ecx
    jmp   .Lflrsl
.Lflrsld:
    subl  4(%esp), %ecx
    movl  %ecx, %eax
    ret

.globl _flr_str_len
_flr_str_len:
    jmp   _flr_strlen

# ─────────────────────────────────────────────────────────────────────
# str_eq(a, b) -> int  (1 = equal, 0 = different)
# ─────────────────────────────────────────────────────────────────────
.globl _flr_str_eq
_flr_str_eq:
    pushl %ebp
    movl  %esp, %ebp
    pushl %esi
    pushl %edi
    movl  8(%ebp),  %esi
    movl  12(%ebp), %edi
.Lfseq:
    movzbl (%esi), %eax
    movzbl (%edi), %ecx
    cmpl  %ecx, %eax
    jne   .Lfseq_no
    testl %eax, %eax
    je    .Lfseq_yes
    incl  %esi
    incl  %edi
    jmp   .Lfseq
.Lfseq_yes:
    movl  $1, %eax
    jmp   .Lfseq_ret
.Lfseq_no:
    xorl  %eax, %eax
.Lfseq_ret:
    popl  %edi
    popl  %esi
    leave
    ret

# ─────────────────────────────────────────────────────────────────────
# int_to_str(n) -> str  (pointer into static buffer — use before next call)
# ─────────────────────────────────────────────────────────────────────
.globl _flr_int_to_str
_flr_int_to_str:
    pushl %ebp
    movl  %esp, %ebp
    pushl %esi
    pushl %edi
    movl  8(%ebp), %eax
    leal  .Lflr_ibuf+11, %edi
    movb  $0, (%edi)            # NUL terminator
    decl  %edi
    testl %eax, %eax
    jge   .Lfits_p
    negl  %eax
    movl  $1, %esi
    jmp   .Lfits_l
.Lfits_p:
    xorl  %esi, %esi
.Lfits_l:
    movl  $10, %ecx
    xorl  %edx, %edx
    divl  %ecx
    addb  $48, %dl
    movb  %dl, (%edi)
    decl  %edi
    testl %eax, %eax
    jne   .Lfits_l
    testl %esi, %esi
    je    .Lfits_n
    movb  $45, (%edi)           # '-'
    decl  %edi
.Lfits_n:
    incl  %edi
    movl  %edi, %eax            # return pointer to start of string
    popl  %edi
    popl  %esi
    leave
    ret

# ─────────────────────────────────────────────────────────────────────
# str_to_int(s) -> int
# ─────────────────────────────────────────────────────────────────────
.globl _flr_str_to_int
_flr_str_to_int:
    pushl %ebp
    movl  %esp, %ebp
    pushl %esi
    movl  8(%ebp), %esi
    xorl  %eax, %eax
    xorl  %ecx, %ecx
    cmpb  $45, (%esi)           # '-'?
    jne   .Lfsti_l
    movl  $1, %ecx
    incl  %esi
.Lfsti_l:
    movzbl (%esi), %edx
    cmpl  $48, %edx
    jl    .Lfsti_d
    cmpl  $57, %edx
    jg    .Lfsti_d
    imull $10, %eax
    subl  $48, %edx
    addl  %edx, %eax
    incl  %esi
    jmp   .Lfsti_l
.Lfsti_d:
    testl %ecx, %ecx
    je    .Lfsti_r
    negl  %eax
.Lfsti_r:
    popl  %esi
    leave
    ret

# ─────────────────────────────────────────────────────────────────────
# abs(n) -> int
# ─────────────────────────────────────────────────────────────────────
.globl _flr_abs
_flr_abs:
    movl  4(%esp), %eax
    testl %eax, %eax
    jge   .Labs_ok
    negl  %eax
.Labs_ok:
    ret

# ─────────────────────────────────────────────────────────────────────
# min(a, b) -> int
# ─────────────────────────────────────────────────────────────────────
.globl _flr_min
_flr_min:
    movl  4(%esp), %eax
    movl  8(%esp), %ecx
    cmpl  %ecx, %eax
    jle   .Lmin_ok
    movl  %ecx, %eax
.Lmin_ok:
    ret

# ─────────────────────────────────────────────────────────────────────
# max(a, b) -> int
# ─────────────────────────────────────────────────────────────────────
.globl _flr_max
_flr_max:
    movl  4(%esp), %eax
    movl  8(%esp), %ecx
    cmpl  %ecx, %eax
    jge   .Lmax_ok
    movl  %ecx, %eax
.Lmax_ok:
    ret

# ─────────────────────────────────────────────────────────────────────
# assert(cond, msg)  — prints "assertion failed: <msg>\n" to stderr
#                       then calls sys_exit(1) if cond == 0
# ─────────────────────────────────────────────────────────────────────
.globl _flr_assert
_flr_assert:
    pushl %ebp
    movl  %esp, %ebp
    movl  8(%ebp), %eax
    testl %eax, %eax
    jne   .Lassert_ok
    # write "assertion failed: " to stderr (fd 2)
    movl  $4,  %eax
    movl  $2,  %ebx
    leal  .Lflr_assert_msg, %ecx
    movl  $19, %edx
    int   $0x80
    # write user message
    movl  12(%ebp), %esi
    movl  %esi, %ecx
.Lasssl:
    cmpb  $0, (%ecx)
    je    .Lasssd
    incl  %ecx
    jmp   .Lasssl
.Lasssd:
    subl  %esi, %ecx
    movl  %ecx, %edx
    movl  %esi, %ecx
    movl  $4,   %eax
    movl  $2,   %ebx
    int   $0x80
    # write newline
    movl  $4,  %eax
    movl  $2,  %ebx
    leal  .Lflr_nl, %ecx
    movl  $1,  %edx
    int   $0x80
    # exit(1)
    movl  $1, %ebx
    movl  $1, %eax
    int   $0x80
.Lassert_ok:
    leave
    ret

# ─────────────────────────────────────────────────────────────────────
# alloc(size) -> ptr   bump heap allocator over 1 MB BSS region
# free(ptr)           no-op
# ─────────────────────────────────────────────────────────────────────
.globl _flr_alloc
_flr_alloc:
    pushl %ebp
    movl  %esp, %ebp
    movl  _flr_heap_ptr, %eax
    movl  8(%ebp), %ecx
    addl  $3,  %ecx
    andl  $-4, %ecx             # align to 4 bytes
    addl  %ecx, _flr_heap_ptr
    leave
    ret

.globl _flr_free
_flr_free:
    ret

# ─────────────────────────────────────────────────────────────────────
# Data
# ─────────────────────────────────────────────────────────────────────
    .section .data
    .align 4
.Lflr_ibuf:
    .space 24                   # scratch buffer for int<->str conversions
.Lflr_nl:
    .byte 10                    # newline character
.Lflr_assert_msg:
    .ascii "assertion failed: "
_flr_heap_ptr:
    .long _flr_heap_start

    .section .bss
    .align 4
_flr_heap_start:
    .space 1048576              # 1 MB bump heap
