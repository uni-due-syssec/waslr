# WASLR

## Setup

Run `make` in the root of this project to build a toolchain, standard libraries, and compiler-rt in `waslr-sdk/`

## Usage

The toolchain's binaries are provided in `waslr-sdk/bin/`.

### Required Compiler Flags
- `--target=wasm32-wasi`
- `--sysroot=<...>/waslr-sdk/share/wasi-sysroot`
- `-fwaslr`

It is also advised to disable default allocator builtins: `-fno-builtin-calloc -fno-builtin-malloc -fno-builtin-realloc -fno-builtin-free`

### Required Linker Flags
- `-Wl,--waslr`

### Linking the WASLR Runtime 
Compile & link the allocator/runtime from: `waslr-sdk/share/waslr-rt/` 

Important: To ensure the runtime overrides default allocator symbols, it should be placed early/first in the linker order.

### WASM Host Requirements
The host has to provide a seed for the RNG through the imported global `__waslr_seed`

## Changing the Allocator Chunk Size
To adjust the allocator's chunk size, edit `waslr-sdk/share/waslr-rt/waslr.c`:
1) Set the chunk size
- Update `CHUNK_SIZE_LOG2` to the base-2 logarithm of the desired chunk size (e.g., 8 for 256 byte chunks)
- For example, for 256-byte chunks:
`#define CHUNK_SIZE_LOG2 8`
2) Update the small object size classes
- Modify `FOR_EACH_SMALL_OBJECT_GRANULES(M)` to include all power-of-two sizes up to and including `CHUNK_SIZE / 8` (8 = Granule size).
- Example for 256-byte chunks:
```
    #define FOR_EACH_SMALL_OBJECT_GRANULES(M) \
        M(1) M(2) M(4) M(8) M(16) M(32)
```