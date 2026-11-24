#include <stdio.h>
#include <stdatomic.h>
#include "quiver_protocol.h"

int main(void)
{
    if (ATOMIC_INT_LOCK_FREE != 2) {
        fputs("32-bit atomics are not always lock-free on this target\n", stderr);
        return 1;
    }
    if (ATOMIC_LLONG_LOCK_FREE != 2) {
        fputs("64-bit atomics are not always lock-free on this target\n", stderr);
        return 1;
    }
    printf("QNX-Quiver BAR2 protocol v%u: %u bytes, %u slots/ring (%u usable)\n",
           (unsigned)QV_PROTOCOL_VERSION, (unsigned)sizeof(qv_bar2_t),
           (unsigned)QV_RING_SLOTS, (unsigned)QV_RING_CAPACITY);
    return 0;
}