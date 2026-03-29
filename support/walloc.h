#ifndef WALLOC_H
#define WALLOC_H

#include <stdlib.h>

#define PAGE_SIZE 65536

void* malloc(size_t size);
void free(void *ptr);

#endif // WALLOC_H