# implementation

## Approach
- wasm globals like __stack_pointer are added very late in the wasm backend and not present in LLVM IR -> Define __stack_pointer as external so it is resolved at link time, and add instrumentation that overwrites its value with each module instantiation

## Status
- Adds instrumentation to overwrite the stack pointer global with each instantation. Calls are added as global constructors and are present in the binary, but the code does not seem to execute at runtime.

- Since the pass runs once on each module, calls to this overwrite function are inserted once for each module. Enabling LTO seems like the solution but it appears that LTO does not work as expected when compiling to web assembly? At least the pass does not run when using LTO hooks

## Build the pass

Tested with LLVM 19.1.7.

In `waslr_pass`: 

run `build.sh`(Possibly need to change LLVM_ROOT in `CMakeLists.txt`)
