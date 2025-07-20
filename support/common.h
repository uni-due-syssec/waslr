#ifndef COMMON_H

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// exporting functions to host
#define WASM_EXPORT(name) __attribute__((export_name(#name))) name

void consolef(const char *, ...);

extern void debug_early(uintptr_t);
extern void console(void *);
extern void console_var(char *, void *);
extern void console_uintptr(char *, uintptr_t);
extern void console_uintarray(char *, uint32_t* arr, uint32_t length);

#endif // !COMMON_H
