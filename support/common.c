#include "common.h"

// To be called from the host, to allocate memory from outside the module
void *WASM_EXPORT(Walloc)(size_t size) { return malloc(size); }
void WASM_EXPORT(Wfree)(void *ptr) { return free(ptr); }
