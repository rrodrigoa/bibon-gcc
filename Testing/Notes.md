# Verifying a custom malloc (TLSF) with gdb

## 1. What we confirmed

- **The custom allocator is the one in use.** The static binary defines one `malloc`, `free`, `calloc` and `realloc` each, so there is no libc `malloc` competing with it.
- **`malloc` reaches TLSF.** The actual call chain at runtime is `main` → `malloc` → `__libc_malloc` → TLSF's `malloc`. `malloc` itself is only a `jmp` to `__libc_malloc`.
- **Lazy initialization works.** On the first call, `__libc_malloc` maps a control block with `tlsf_mmap`, zeroes it, and records it as initialized.
- **The empty-pool fallback works.** The first TLSF `malloc` returns NULL because the pool is empty. The wrapper then asks the OS for a block, hands it to TLSF with `TLSF_AddMemoryBlock`, and the retry succeeds.
- **Arguments are passed correctly.** TLSF `malloc` received the control block and the exact size `main` asked for.
- **The OS request is page-rounded.** The size requested from the OS was the allocation size plus headroom, rounded up to whole pages.
- **The pool is handed over correctly.** The address and size given to `TLSF_AddMemoryBlock` were exactly what the OS map call returned.
- **The returned pointer comes from the pool.** The second TLSF `malloc` returned a non-zero, 16-byte-aligned pointer inside the pool, offset a little from the pool start (likely header overhead).
- **The program output is correct.**

## 2. How to replicate

### 2.1 Test program

A minimal `main.c`:

- allocate a small `int` array with `malloc`
- fill it and print it
- `free` it

Build it statically with debug symbols and no optimization, so the allocator is linked into the binary and gdb can follow it:

```
$TARGET-gcc -g -O0 -static main.c -o hello
./hello
```

Correct output only shows that the memory is usable. It does not show which allocator provided it, so the checks below are needed.

### 2.2 Static checks (before running anything)

**Are the custom functions linked in?**

```
$TARGET-nm hello | grep -wE "malloc|free|calloc|realloc"
$TARGET-nm hello | grep -i tlsf
```

- Each of `malloc`, `free`, `calloc` and `realloc` should be defined exactly once (type `T`).
- The allocator's own functions should be present too. Being present does not prove they are called.

**Where does `malloc` go?**

```
$TARGET-objdump -d hello --disassemble=malloc
```

Here `malloc` was only a `jmp` to another function, so follow it:

```
$TARGET-objdump -d hello --disassemble=<function it jumps to>
```

Read the disassembly and note the sequence of `call` instructions. In this library the wrapper does the following:

1. takes a lock
2. checks an "initialized" flag and, on first use, maps a control block and zeroes it
3. calls the allocator's `malloc` with the control block and the requested size
4. if that returns NULL, asks the OS for a new block, adds it to the pool, and retries
5. unlocks and returns

Writing this expected sequence down first gives you something concrete to check in gdb.

### 2.3 Runtime checks (gdb)

```
gdb ./hello
(gdb) break main
(gdb) run
```

Stopping at `main` first avoids confusion from any allocations made during startup.

Then set breakpoints on the key functions from the disassembly (map, allocate, request OS block, add block, free):

```
(gdb) break <os map function>
(gdb) break <allocator malloc>
(gdb) break <request OS block function>
(gdb) break <add memory block function>
(gdb) break <allocator free>
(gdb) continue
```

At each stop, check:

| Check | Command | What to look for |
|---|---|---|
| Who called it | `bt` | The wrapper (`__libc_malloc` here) and `main`. |
| Arguments | `info registers rdi rsi rdx` | x86-64 passes the first three arguments in these. |
| Return value | `finish`, then `print/x $rax` | The return value of the function. |

**Expected order on the first `malloc` from `main`:**

1. The OS map function runs to create the **control block**.
2. The allocator's `malloc` runs. Its first argument is the control block and its second is your requested size. It **returns 0**, because the pool is empty.
3. The "request OS block" function runs, asking for your size plus some headroom.
4. The OS map function runs again. The request is **rounded up to whole pages** and returns the **pool address**.
5. The "add memory block" function receives the control block, the **same pool address**, and the rounded size.
6. The allocator's `malloc` runs again and returns a **non-zero pointer inside the pool**.

**What "working" looks like:**

- The pool address passed to "add memory block" equals what the OS map function returned.
- The final pointer lies within `[pool, pool + size)`.
- The final pointer is suitably aligned (16 bytes on x86-64).
- In `main`, `print arr` equals that pointer.
- `free(arr)` in `main` hits the allocator's free, with the control block and `arr` as arguments.

### 2.4 gdb notes

- `bt` may show `?? ()` frames and a "corrupt stack?" warning. The allocator functions have no unwind info, so gdb guesses the frame layout. Ignore the junk frames and look at the ones that name real functions.
- If `finish` misbehaves, read the return address with `x/gx $rsp` and run `break *<address>`.
- `info breakpoints` shows how many times each breakpoint was hit.
- A breakpoint that is never hit means that function is not used by this program.

## 3. Not yet confirmed

- [ ] `arr` equals the returned pointer (`print arr` in `main`).
- [ ] `free` reaches the allocator's free with the right arguments.
- [ ] `calloc` and `realloc` route correctly (disassemble them, then add calls to `main` and trace).
- [ ] A freed block is reused by the next allocation of a similar size.
- [ ] Adjacent freed blocks are coalesced.
- [ ] Allocations larger than the initial pool trigger pool growth.
- [ ] Edge cases: `malloc(0)`, `free(NULL)`, absurdly large requests (should return NULL).
- [ ] Failure of the OS map call is handled. Currently the wrapper would still call the allocator with a NULL control block.
- [ ] A random alloc/free stress test with data pattern checks finds no overlap or corruption.