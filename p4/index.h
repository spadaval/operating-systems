/**
 * Tony Givargis
 * Copyright (C), 2023
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * index.h
 */

#ifndef _INDEX_H_
#define _INDEX_H_

#include "system.h"

struct index;

struct index *index_open(void);

void index_close(struct index *index);

uint64_t *index_update(struct index *index, const void *key, uint64_t key_len);

uint64_t *index_lookup(struct index *index, const char *key, uint64_t key_len);

u8 *index_serialize(struct index *index, /*out*/ u64 *size);

struct index *index_deserialize(u8 *buf, u64 entries);

void index_print(struct index *index);

#endif /* _INDEX_H_ */
