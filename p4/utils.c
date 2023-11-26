
#include "utils.h"

#include <stdlib.h>

#include "system.h"

char *dump_bytes(const void *p, size_t count) {
    char *buf = (char *)malloc(count * 4 + 1);
    const unsigned char *src = (unsigned char *)p;
    char *dest = buf;
    while (count-- > 0) {
        dest += snprintf(dest, 999, "%.2X", *src++);
        if (count % 2 == 0)
            *dest++ = ' ';
    }
    *dest = '\0';  // return an empty sting for an empty memory chunk
    return buf;
}
