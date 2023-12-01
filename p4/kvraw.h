/**
 * Tony Givargis
 * Copyright (C), 2023
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * kvraw.h
 */

#ifndef _KVRAW_H_
#define _KVRAW_H_

#include "system.h"

struct kvraw;

struct kvraw *kvraw_open(const char *pathname, bool enable_persistence);

void kvraw_close(struct kvraw *kvraw);

int kvraw_lookup(struct kvraw *kvraw,
                 void *key,
                 uint64_t *key_len, /* in/out */
                 void *val,
                 uint64_t *val_len, /* in/out */
                 uint64_t *off);    /* in/out */

int kvraw_append(struct kvraw *kvraw,
                 const void *key,
                 uint64_t key_len,
                 const void *val,
                 uint64_t val_len,
                 uint64_t *off);

void kvraw_saveindex(struct kvraw *kvraw, u8 *buf, u64 buf_len);

u8 *kvraw_getindex(struct kvraw *kvraw, /*out*/ u64 *len);

#endif /* _KVRAW_H_ */
