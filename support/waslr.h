#ifndef WASLR_H
#define WASLR_H

#include <stdint.h>
#include <stddef.h>

/*typedef __SIZE_TYPE__ size_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef __UINT8_TYPE__ uint8_t;*/

#define NULL ((void *) 0)
// exporting functions to host
#define WASM_EXPORT(name) __attribute__((export_name(#name))) name

void* malloc(size_t size);
void free(void *ptr);

// For testing
extern void debug_early(uintptr_t);
extern void console_uintptr(char *, uintptr_t);
/*extern void debug4(size_t offset);
extern void console(void *);
extern void console_var(char *, void *);
extern void console_uintarray(char *, uint32_t* arr, uint32_t length);

extern void alloc_error(uintptr_t headerpage_idx, size_t offset, uint8_t from_kind, uint8_t to_kind);
extern void flush_errors();
*/
size_t rand_in_range(size_t min, size_t max);

#endif // WASLR_H