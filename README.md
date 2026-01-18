# implementation

## Prerequisites
TBA

## Setup
- Disclaimer: This setup process is based on my memories and has not been tested yet but will be final once we set it up from scratch again :D

- Paths in the build scripts need to be updated

1. Build LLVM
- waslr-llvm/build.sh

2. Build libclang_rt.builtins-wasm32.a
- waslr-llvm/build-compiler-rt.sh 
- copy into waslr-llvm/build/lib/clang/20/lib/wasi

3. Build wasi libc
- support/wasi-libc/build.sh
 
## Usage

- waslr clang flag
- waslr linker flag
- set sysroot to custom wasi sysroot
- link waslr.c 
- 
## Limitations
- VLA
- max allocation size (depends on chunk size)
    - 256 byte chunk size: 255 data pages (16711680 bytes)
    - 512 byte chunk size: 511 data pages (33488896 bytes)
    - ...
- ...TBA

## TODO 

### Cleanup
- Optional: Replace llvm and wasi libc with git submodules and only keep changed files in this repo
    - setup: copy changes into the cloned original

- After testing:
    - Remove debug prints
    - possibly make setup easier by replacing hardcoded paths in scripts with env variables?
- Add RustRunner to repo?
- cleanup waslr
- cleanup examples dir
- wasi libc setup?

### Possible Allocator Improvements
- Large Object allocations: Store the size in the header page
    - when searching free memory, this would allow us to not have to check each byte
    - impl:
        - for an allocation that requires N chunks, we currently write N bytes into the header, where the first byte is an identifier for the start of a LO
        - we could use the next 2 bytes after the identifier to record the LO size
    - this change would not affect the number of writes necessary to update headers, but the number of read operations to check the headerpage (in case of allocations/frees)
    - but this would raise the min. number of chunks for a large object from 2 to 4 (start byte + 2 bytes for size + end byte)

- improved check when growing memory
    - we grow memory if no headerpage can fit the allocation
    - we search the last headerpage again, if some of the new memory is managed by it 
    - but we should also check if the new memory is enough to fit the allocation. If not, we do not need to search again
- memorize max possible allocation
    - so that we can prevent searching an entire headerpage if we already know the allocation wont fit
    - pretty sure there is no efficient way to do this. It always ends up requiring to check the entire headerpage after each allocation/free, which would defeat the entire purpose of this improvement