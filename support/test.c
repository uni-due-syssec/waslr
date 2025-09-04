#include <stdint.h>

#include "waslr.h"
#include "../../implementation/support/common.h"

int main() {
    uint32_t sizes[6] = {1, 8, 14, 33, 50, 2550, 70000};
    for (int i=0; i<5; i++) {
        // only test Small objects first
        if (sizes[i] > 256) {
            console("LOs not supported yet");
            continue;
        }
        console_uintptr("ALLOC: ", sizes[i]);
        char* ptr = (char *)malloc(sizes[i]);
        console_uintptr("Ptr:", (uintptr_t) ptr);
    }
    // no need to free yet
}