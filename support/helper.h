#ifndef HELPER_H
#define HELPER_H
#include <stdlib.h>

void fisher_yates_shuffle(size_t *array, size_t size);
size_t rand_in_range(size_t min, size_t max);

#endif // HELPER_H