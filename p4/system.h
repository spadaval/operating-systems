/**
 * Tony Givargis
 * Copyright (C), 2023
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * system.h
 */

#ifndef _SYSTEM_H_
#define _SYSTEM_H_

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

#define ARRAY_SIZE(a) ((sizeof(a)) / (sizeof(a[0])))

#define UNUSED(s)  \
    do {           \
        (void)(s); \
    } while (0)

#define TRACE(s)                             \
    do {                                     \
        fprintf(stderr,                      \
                "error: %s:%d: %s\n",        \
                __FILE__,                    \
                __LINE__,                    \
                safe_strlen(s) ? (s) : "^"); \
    } while (0)

#define EXIT(s)     \
    do {            \
        TRACE((s)); \
        assert(0);  \
        exit(-1);   \
    } while (0)

#define FREE(p)                \
    do {                       \
        if ((p)) {             \
            free((void *)(p)); \
            (p) = NULL;        \
        }                      \
    } while (0)

// unsigned 8-bit
#define u8 uint_fast8_t
// unsigned 64-bit
#define u64 uint64_t
// An integer large enough to hold a pointer
#define usize uint64_t

uint64_t ref_time(void);

void us_sleep(uint64_t us);

void file_delete(const char *pathname);

void safe_sprintf(char *buf, size_t len, const char *format, ...);

size_t safe_strlen(const char *s);

size_t page_size(void);

void *memory_align(void *p, size_t n);

#endif /* _SYSTEM_H_ */
