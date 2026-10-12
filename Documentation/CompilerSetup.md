# ToolChain Setup

This guide details the step-by-step process for bootstrapping a GCC cross-compiler toolchain from scratch. This toolchain solves the "chicken-and-egg" problem: building a custom standard library (`bibon-libc` with a custom TLSF allocator) requires a compiler, but a standard compiler requires a C library to build. 

By building the toolchain in isolated stages, we can successfully compile native host binaries (for WSL testing) that link against our custom RTOS allocator instead of the host's `glibc`.

Note: The GCC installed lacks support for 32-bit architectures (multilib) since your WSL environment may be missing the 32-bit C library headers [Running on a 64-bit architecture assumption]

## Skip To The Script?
--STILL WORKING ON IT--

Use build_toolchain.sh to skip through the process below. 
1. Make it executable with chmod +x buildtoolchain.sh 
2. Run ./build_toolchain.sh

## 0. Prerequisites

Before beginning, install the required build dependencies on your Ubuntu/WSL host:

```bash
sudo apt update
sudo apt install build-essential bison flex libgmp3-dev libmpc-dev libmpfr-dev texinfo
```

## 1. Environment Setup

Set up the environment variables. **Note:** If you close your terminal, you must re-run these exports or add them to your `~/.bashrc`.

```bash
export TARGET=x86_64-linux-gnu
export PREFIX="$HOME/bibon-toolchain"
export SYSROOT="$PREFIX/sysroot"
export PATH="$PREFIX/bin:$PATH"
```

## 2. Source Preparation

Download and extract the necessary source code for Binutils and GCC into your home directory.

```bash
cd $HOME
wget https://ftp.gnu.org/gnu/binutils/binutils-2.41.tar.xz
tar -xf binutils-2.41.tar.xz

wget https://ftp.gnu.org/gnu/gcc/gcc-13.2.0/gcc-13.2.0.tar.xz
tar -xf gcc-13.2.0.tar.xz

# Fetch GCC prerequisites (GMP, MPFR, MPC)
cd gcc-13.2.0
contrib/download_prerequisites
```

## 3. Stage 0: Binutils

Build the assembler (`as`) and linker (`ld`) configured for the target architecture.

```bash
cd $HOME
mkdir build-binutils
cd build-binutils
../binutils-2.41/configure --target=$TARGET --prefix=$PREFIX --with-sysroot=$SYSROOT --disable-nls --disable-werror
make -j$(nproc)
make install
```

## 4. Stage 1: Freestanding GCC

Build a bare-metal C compiler. The `--without-headers` flag ensures it builds without expecting a standard C library to exist yet. 

```bash
cd $HOME
mkdir build-gcc-stage1
cd build-gcc-stage1
../gcc-13.2.0/configure \
  --target=$TARGET \
  --prefix=$PREFIX \
  --disable-nls \
  --enable-languages=c \
  --without-headers \
  --disable-multilib \
  --disable-bootstrap

make -j$(nproc) all-gcc all-target-libgcc
make install-gcc install-target-libgcc
```

## 5. Build Custom C Library (bibon-libc)

Use the newly built Stage 1 compiler to compile the custom standard library and TLSF allocator, then install it into the toolchain's sysroot.

```bash
cd $HOME
# Clone your repository (replace URL with actual URL if starting fresh)
# git clone <your-repository-url> bibon-libc
cd bibon-libc

# Clean any existing build artifacts
make clean || true

# Configure using the Stage 1 compiler
CC=$TARGET-gcc ./configure --prefix=$SYSROOT

# Build and install into sysroot
make -j$(nproc)
make install
```

### 5.1 Sysroot Fix
GCC expects standard headers in `usr/include`, but our library installed them in `include`. Create symlinks to bridge this gap:

```bash
mkdir -p $SYSROOT/usr
ln -sf ../include $SYSROOT/usr/include
ln -sf ../lib $SYSROOT/usr/lib
```

## 6. Stage 2: Final GCC

Now that the custom C library is in the sysroot, build the final fully-featured C compiler. 

```bash
cd $HOME
mkdir build-gcc-final
cd build-gcc-final
../gcc-13.2.0/configure \
  --target=$TARGET \
  --prefix=$PREFIX \
  --with-sysroot=$SYSROOT \
  --disable-nls \
  --enable-languages=c \
  --disable-multilib \
  --disable-libstdcxx \
  --disable-bootstrap

make -j$(nproc)
make install
```

## 7. Verification

Verify that the Stage 2 compiler works and actually links against the custom TLSF allocator. 

1. **Create a test file (`hello.c`):**
```c
#include <stdio.h>
#include <stdlib.h>

int main() {
    printf("Hello from bibon-libc!\n");
    void* ptr = malloc(1024);
    if (ptr) {
        printf("Allocated at: %p\n", ptr);
        free(ptr);
    }
    return 0;
}
```

2. **Compile Statically:**
*You must compile statically to bypass the host OS dynamic loader.*
```bash
$TARGET-gcc -static hello.c -o hello
./hello
```

3. **Verify Symbols:**
```bash
$TARGET-nm hello | grep -i tlsf
```
*(If TLSF symbols are listed, the integration was successful!)*

---

## 🛑 Troubleshooting & Known Issues

During the development of this toolchain, several critical roadblocks were encountered. Here is the historical log of issues and the exact flags used to solve them.

### 1. Missing 32-bit Multilib Headers
* **Error:** `configure: error: I suspect your system does not have 32-bit development libraries`
* **Cause:** By default, GCC tries to build support for both 64-bit and 32-bit targets. The WSL environment was missing 32-bit glibc headers.
* **Solution:** Added `--disable-multilib` to the GCC `configure` step to enforce a strict 64-bit build.

### 2. Missing Musl Architecture Headers (bits/alltypes.h)
* **Error:** `fatal error: bits/alltypes.h: No such file or directory`
* **Cause:** Attempting to manually compile `bibon-libc` bypassed musl's dynamic header generation script.
* **Solution:** Ran the repository's native `./configure` script, passing `CC=$TARGET-gcc` to inject the Stage 1 compiler and properly generate the `config.mak` and headers.

### 3. GCC Sysroot Path Mismatch
* **Error:** `The directory that should contain system headers does not exist: sysroot/usr/include`
* **Cause:** GCC's `fixincludes` script hardcodes the search path to `/usr/include`, but `bibon-libc` installed headers directly to `/include`.
* **Solution:** Created a `usr` folder in the sysroot and symlinked `include` and `lib` into it (`ln -s ../include usr/include`).

### 4. C++ Standard Library (`libstdc++`) Crash
* **Error:** `error: '_ISupper' was not declared in this scope` / `missing binary operator before token "("` on `__GLIBC_PREREQ`
* **Cause:** GCC tried building the C++ standard library, which requires `glibc`-specific internal macros. Our custom musl-based `bibon-libc` did not have these.
* **Solution:** Dropped C++ entirely for the host-testing phase. Added `--enable-languages=c` and `--disable-libstdcxx` to the Stage 2 configuration.

### 5. The Host-Target Bootstrap Loop
* **Error:** `error: Specified CC_FOR_BUILD doesn't seem to work` (Failed on Stage 2 building GMP)
* **Cause:** Because the toolchain target (`x86_64-linux-gnu`) matched the WSL host architecture, GCC assumed we were building a replacement compiler for the host Ubuntu OS and attempted a massive 3-stage bootstrap using our tiny RTOS library.
* **Solution:** Added the `--disable-bootstrap` kill-switch to GCC configurations to force it to act strictly as a cross-compiler.

### 6. Invalid ELF Header on Execution
* **Error:** `./diagnose_tlsf_test: error while loading shared libraries: /lib/x86_64-linux-gnu/libc.so: invalid ELF header`
* **Cause:** Executing a dynamic musl binary on a glibc-based Ubuntu host confuses the dynamic loader, as musl's `.so` file is often a text linker script, not a raw binary.
* **Solution:** Bypassed the host's dynamic loader entirely by appending `-static` to compiler commands (e.g., `make check CC="$TARGET-gcc -static"`). This embedded the library and allocator directly into the test binaries.