# ObjFalcon

ObjFalcon (also: **Ofc** / **FalconC++**) is a fork of Falcon targeting a C/Objective-C-inspired syntax while keeping Falcon's low-level DNA intact. Single C file compiler, pure-assembly runtime, no libc, no LLVM.

File extension: `.ofc` (also accepts `.fl` for Falcon compatibility)

---

## What's new over Falcon

### Syntax
- **Semicolons** as statement terminators (alongside newlines — both work)
- **C-style function definitions**: `int add(int a, int b) { return a + b; }`
- **C-style variable declarations**: `int x = 42;`
- **Ternary operator**: `a ? b : c`
- **`switch/case/default`**
- **`do { } while (cond)`**
- **`asm("...")`** — inline assembly (emits raw asm string)
- **`for` loops** accept both `,` and `;` separators

### Preprocessor
- `#define NAME value`
- `#include "file"` / `#include <file>`
- `#ifdef NAME` / `#ifndef NAME` / `#endif`
- `#undef NAME`

### New keywords
`char`, `unsigned`, `enum`, `switch`, `case`, `default`, `do`, `sizeof`, `asm`

---

## Getting started

```bash
# Build
gcc -O2 -o ofcc ofcc.c
as --32 ofcr.s -o ofcr.o

# Compile and run
make run F=examples/ofc/01_csyntax.ofc

# Run all tests
make test
```

---

## Syntax examples

### C-style function
```c
int add(int a, int b) {
    return a + b;
}

int main() {
    int x = add(10, 20);
    print(x);
    return 0;
}
```

### Ternary
```c
int max = a > b ? a : b;
```

### Switch
```c
switch (x) {
    case 1:
        print(1)
        break
    case 2:
        print(2)
        break
    default:
        print(0)
        break
}
```

### Do-while
```c
int i = 0;
do {
    i += 1;
} while (i < 5)
```

### Inline asm
```c
asm("nop");
asm("cli");
asm("hlt");
```

### Preprocessor
```c
#define MAX 100
#ifdef DEBUG
    print("debug mode")
#endif
```

### Mixed Falcon/C syntax
Both styles work in the same file:
```c
import "std"

int factorial(int n) {
    return n <= 1 ? 1 : n * factorial(n - 1);
}

int main() {
    for (i: int = 1, i <= 10, i += 1) {
        print(factorial(i));
    }
    return 0;
}
```

---

## Inherited from Falcon

All Falcon features are preserved: structs, typedefs, arrays, pointers, long/float/double, hardware intrinsics (`__syscall`, `__inb/__outb`, `__cli/__sti/__hlt`, `__rdtsc`, `__peek/__poke`), freestanding mode (`--freestanding`), x86-32 and x86-64 targets, import system, full type checker.

See Falcon's README for the full language reference — everything there applies to ObjFalcon.

---

## Compiler flags

| Flag | Description |
|------|-------------|
| `-o <file>` | Output file |
| `-arch x86-32-linux` | 32-bit target (default) |
| `-arch x86-64-linux` | 64-bit target |
| `--freestanding` | No runtime, no sys_exit |
| `-I<path>` | Add import search path |

---

## Examples

| File | Feature |
|------|---------|
| `examples/ofc/01_csyntax.ofc` | C-style functions and vars |
| `examples/ofc/02_semicolons.ofc` | Semicolons as terminators |
| `examples/ofc/03_ternary.ofc` | Ternary operator |
| `examples/ofc/04_switch.ofc` | Switch/case/default |
| `examples/ofc/05_dowhile.ofc` | Do-while loop |
| `examples/ofc/06_define.ofc` | #define |
| `examples/ofc/07_inlineasm.ofc` | Inline asm |
| `examples/ofc/08_mixed_syntax.ofc` | Mixed Falcon+C syntax |

All 27 original Falcon examples also pass unchanged.
