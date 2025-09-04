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
__attribute__((used))
void __waslr_init(unsigned int seed) {
  debug_early(1);
  srand(seed);
  //grow_wasm_memory(PAGE_SIZE * 10);
}


// rand returns int
// min must always be smaller than max. Our implementation ensures that so we do not explicitly check
// max range is RAND_INT, which is more than enough for our uses
size_t rand_in_range(size_t min, size_t max) {
    return min + (rand() % (max - min + 1));
}

/*int rand_in_range(int min, int max) {
  return min + rand() % (max - min + 1);
}*/

// Just a simple shuffling algorithm for testing
void fisher_yates_shuffle(size_t *array, size_t size) {
  for (size_t i = size - 1; i > 0; i--) {
    size_t j = rand() % (i + 1);
    size_t temp = array[i];
    array[i] = array[j];
    array[j] = temp;
  }
}

struct header_page {
    uint8_t headers[PAGE_SIZE];
};

void WASM_EXPORT(test_size)() {
  // go to (random) header page
  //  - keep track of number of pages allocated 
  //  - calc how many header pages there are and pick one randomly
  size_t usable_pages = __builtin_wasm_memory_size(0)-1;
  console_uintptr("usable pages: ", usable_pages);
  // name is misleading: its the total - 1 but adding 1 here would just cause unnecessary additions/subtractions in the following lines
  size_t total_hpages = usable_pages / 256;
  size_t selected_header_index = total_hpages > 0 ? rand_in_range(0, total_hpages) : 0;
  size_t headerpage_idx = 1+(selected_header_index * 256);

  struct header_page *headerpage = (struct header_page *) (headerpage_idx * PAGE_SIZE);

  console_uintptr("HEADER PAGE: ", (uintptr_t)headerpage);
  int chunk_index = rand_in_range(0, usable_pages*256);

  size_t attempts = 0;
  while (headerpage->headers[chunk_index] != 0 && attempts < PAGE_SIZE) {
    chunk_index = (chunk_index+1) % PAGE_SIZE;
    attempts++;
  }

  // get corresponding data page + chunk
    if (attempts == PAGE_SIZE) {
    // no free chunk found
    // TODO: go to next page
    console("NO FREE CHUNK FOUND ON PAGE");
  } 

  // get corresponding data page + chunk
  size_t random_chunk_page_idx = chunk_index / 256;
  size_t random_chunk_idx = chunk_index % 256;  

  // assume headerpage points to the correct header page at this point
  // offset = page_offset * PAGE_SIZE + chunk_offset * CHUNK_SIZE
  size_t offset = ((headerpage_idx + random_chunk_page_idx) * PAGE_SIZE) + (random_chunk_idx * 256);
  // page_to_search = 1 , 256, ...
  // random_chunk_page = 

  console_uintptr("PAGE: ", random_chunk_page_idx);
  console_uintptr("CHUNK: ", random_chunk_idx);
  console_uintptr("=> OFFSET: ", offset);
}

void WASM_EXPORT(print_symbols)() {
    console_uintptr("__heap_base: ", (uintptr_t)&__heap_base);
    console_uintptr("__heap_end: ", (uintptr_t)&__heap_end);
    console_uintptr("__global_base: ", (uintptr_t)&__global_base);
    console_uintptr("__data_end: ", (uintptr_t)&__data_end);
    console_uintptr("__stack_high: ", (uintptr_t)&__stack_high);
    console_uintptr("__stack_low: ", (uintptr_t)&__stack_low);
}

