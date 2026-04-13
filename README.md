# WASLR

## Setup

### 1. Build LLVM
In `waslr-llvm/`, run `build.sh`.
Afterwards, run `ninja` in the `waslr-llvm/build/` directory.

`clang` and other binaries will be located in `waslr-llvm/build/bin/`.

### 2. Add builtins
We only need the compiler builtins, which we provide a prebuilt version of in `support/`.
To make the builtins available to the newly built toolchain, copy `support/libclang_rt.builtins-wasm32-waslr.a` into `build/lib/clang/20/lib/wasi/libclang_rt.builtins-wasm32.a` (make sure to rename the file)

#### Optional: Build Compiler Runtime Libraries
If you want to build the builtins and other compiler runtime libraries yourself, run `build-compiler-rt.sh`.
Afterwards, run `ninja`in the `waslr-llvm/build-compiler-rt` directory.

Finally, copy `build-compiler-rt/lib/linux/libclang_rt.builtins-wasm32.a` into `build/lib/clang/20/lib/wasi/`.

### 3. Build wasi libc 
In `support/wasi-libc/`, run `build.sh`.

A sysroot will be generated in `support/wasi-libc/sysroot/`.
 
## Usage

- clang location: `waslr-llvm/build/bin/clang`
- Set the target: `--target=wasm32-wasi`
- Set the sysroot: `--sysroot=path/to/support/wasi-libc/sysroot`
- Compiler flags: `-fwaslr -fno-builtin-calloc -fno-builtin-malloc -fno-builtin-realloc -fno-builtin-free`
- Linker flag: `-Wl,--waslr`
- Compile & Link `support/waslr.c`
    - To ensure it overrides the default allocator symbols, it should be placed early/first in the link order