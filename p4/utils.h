#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
char *dump_bytes(const void *p, size_t count);

//#define DEBUG

#ifdef DEBUG
#define log(...) fprintf(stderr, __VA_ARGS__)
#else
#define log(...) (void)(0)
#endif

#endif  // UTILS_H