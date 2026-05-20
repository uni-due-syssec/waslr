#ifndef WASLR_H
#define WASLR_H

#include <stdint.h>
#include <stddef.h>

/*typedef __SIZE_TYPE__ size_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef __UINT8_TYPE__ uint8_t;*/

#define NULL ((void *) 0)


void* malloc(size_t size);
void free(void *ptr);

#endif // WASLR_H