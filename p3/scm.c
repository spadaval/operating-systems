/**
 * Tony Givargis
 * Copyright (C), 2023
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * scm.c
 */

#define _GNU_SOURCE

#include "scm.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/**
 * Needs:
 *   fstat()
 *   S_ISREG()
 *   open()
 *   close()
 *   sbrk()
 *   mmap()
 *   munmap()
 *   msync()
 */

/* research the above Needed API and design accordingly */

scm* scm_open(const char* pathname, int t) {
    scm* s;
    int flags, fd;
    void* result;

    s = (scm*)(malloc(sizeof(scm)));
    s->base = (void*)(0x104000000);
    s->capacity = 10 * page_size();
    s->utilized = 0;

    flags = O_CREAT | O_RDWR;
    fd = open(pathname, flags);
    if (t) truncate(pathname, 0);
    truncate(pathname, s->capacity);
    chmod(pathname, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

    brk(s->base + s->capacity + 5 * page_size());
    result = mmap(s->base, s->capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, 0);

    printf("mmap returned %p\n", result);
    fflush(stdout);

    if (result == MAP_FAILED) {
        printf("mmap failed (%s)\n", strerror(errno));
        exit(errno);
    }

    printf("Assign %d bytes at %p (current brk at %p)\n", s->capacity, (void*)s->base, sbrk(0));
    fflush(stdout);

    /* n = (int*)scm_malloc(s, 4);
    printf("n=%p\n", (void*)n);
    *n = 10;
     printf("n=%d", *n);
    */

    return s;
}

void scm_close(scm* scm) {
    msync(scm->base, scm->capacity, MS_SYNC);
    munmap(scm->base, scm->capacity);
}

void* scm_malloc(scm* scm, size_t n) {
    u8* pos = scm->base + scm->utilized;
    printf("Allocating %lu bytes at %p\n", n, (void*)pos);
    fflush(stdout);
    scm->utilized += n;
    return (void*)pos;
}

char* scm_strdup(scm* scm, const char* s) {
    char* dup = (char*)scm_malloc(scm, strlen(s) + 1);
    strcpy(dup, s);
    return dup;
}

void scm_free(scm* scm, void* p) {
    UNUSED(scm);
    UNUSED(p);
}

size_t scm_utilized(const scm* scm) {
    return scm->utilized;
}

size_t scm_capacity(const scm* scm) {
    return scm->capacity;
}

void* scm_mbase(scm* scm) {
    return scm->base;
}
