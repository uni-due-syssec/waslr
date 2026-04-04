#ifndef WALLOC_H
#define WALLOC_H

#include <stdlib.h>

#define PAGE_SIZE 65536

void* malloc(size_t size);
void free(void *ptr);
void* realloc(void* ptr, size_t new_size);
void* calloc(size_t num, size_t size);

#endif // WALLOC_H