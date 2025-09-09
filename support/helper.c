#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "common.h"
#include "helper.h"
#include "walloc.h"
/**
 * 1024 <-- global_base
 * 1672 <-- data_end
 * 
 * 1680 <-- stack_low
 * 
 *  - randomize stack frames in this area -
 * 
 * 67216 <-- heap_base, stack_pointer, stack_high
 *  
 *  - randomize heap allocations in this area - 
 * 
 * 131072 <-- heap_end
 */


extern unsigned char __heap_base; // heap start
extern unsigned char __heap_end; // heap end

extern unsigned char __global_base; // start of data section
extern unsigned char __data_end; // end of data section

// Not defined as weak since we target LLVM 19
extern unsigned char __stack_high; // stack start (top)
extern unsigned char __stack_low; // stack end (bottom)

// we import the seed from the host.
// did not figure out how to force an import for a global (import attributes are only valid for functions)
// therefore we define the imported global in llvm

// can either export or mark as used to force retention of the symbol until linking
/*__attribute__((used))
void __waslr_init(unsigned int seed) {
  debug_early(1);
  srand(seed);
  //grow_wasm_memory(PAGE_SIZE * 10);
}*/

void WASM_EXPORT(print_symbols)() {
    console_uintptr("__heap_base: ", (uintptr_t)&__heap_base);
    console_uintptr("__heap_end: ", (uintptr_t)&__heap_end);
    console_uintptr("__global_base: ", (uintptr_t)&__global_base);
    console_uintptr("__data_end: ", (uintptr_t)&__data_end);
    console_uintptr("__stack_high: ", (uintptr_t)&__stack_high);
    console_uintptr("__stack_low: ", (uintptr_t)&__stack_low);
}

