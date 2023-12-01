/**
 * Tony Givargis
 * Copyright (C), 2023
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * kvdb.c
 */

#include "kvdb.h"

#include "index.h"
#include "kvraw.h"

#define MUTATE_REMOVE 1
#define MUTATE_INSERT 2
#define MUTATE_UPDATE 3
#define MUTATE_REPLACE 4

struct kvdb {
    uint64_t size;
    uint64_t waste;
    struct kvraw *kvraw;
    struct index *index;
};

static int
chain_lookup(struct kvdb *kvdb,
             const void *key,
             uint64_t key_len,
             void *val,
             uint64_t *val_len, /* in/out */
             uint64_t *off)     /* in/out */
{
    uint64_t key_len_, val_len_, off_;
    void *key_, *val_;
    char buf[256];

    off_ = (*off);
    while (off_) {
        /* speculate with a small key read into a stack buffer */

        key_ = buf;
        val_ = val;
        key_len_ = MIN(key_len, sizeof(buf));
        val_len_ = val_len ? (*val_len) : 0;
        if (kvraw_lookup(kvdb->kvraw,
                         key_,
                         &key_len_,
                         val_,
                         &val_len_,
                         &off_)) {
            TRACE(0);
            return -1;
        }

        /* key length mismatch or partial mismatch ==> no match */

        if ((key_len_ != key_len) ||
            memcmp(key_, key, MIN(key_len, sizeof(key_)))) {
            (*off) = off_;
            continue;
        }

        /* key larger than stack buffer ? */

        if (key_len_ > sizeof(key_)) {
            if (!(key_ = malloc(key_len_))) {
                TRACE(0);
                return -1;
            }
            off_ = (*off);
            val_len_ = 0; /* not needed */
            if (kvraw_lookup(kvdb->kvraw,
                             key_,
                             &key_len_,
                             val_,
                             &val_len_,
                             &off_)) {
                FREE(key_);
                TRACE(0);
                return -1;
            }
        }

        /* key match ? */

        if (!memcmp(key_, key, key_len)) {
            if (buf != key_) {
                FREE(key_);
            }
            if (val_len) {
                (*val_len) = val_len_;
            }
            break;
        }
        if (buf != key_) {
            FREE(key_);
        }
        (*off) = off_;
    }
    return 0;
}

static int /* -1|0|+1 */
mutate(struct kvdb *kvdb,
       const void *key,
       uint64_t key_len,
       void *val,
       uint64_t *val_len,
       int mode) {
    uint64_t *ref, off;
    uint64_t val_len_;
    void *val_;

    /* index */

    if (!(ref = index_update(kvdb->index, key, key_len))) {
        TRACE(0);
        return -1;
    }
    off = (*ref);

    /* chained */

    val_ = ((MUTATE_REMOVE == mode) && val_len) ? val : NULL;
    val_len_ = ((MUTATE_REMOVE == mode) && val_len) ? (*val_len) : 0;
    if (chain_lookup(kvdb, key, key_len, val_, &val_len_, &off)) {
        TRACE(0);
        return -1;
    }

    /* mutate */

    if (MUTATE_REMOVE == mode) {
        if (!off || !val_len_) {
            return +1; /* invalid key */
        }
        if (kvraw_append(kvdb->kvraw,
                         key,
                         key_len,
                         0,
                         0,
                         ref)) {
            TRACE(0);
            return -1;
        }
        if (val_len) {
            (*val_len) = val_len_;
        }
        --kvdb->size;
        ++kvdb->waste;
    } else if (MUTATE_INSERT == mode) {
        if (off && val_len_) {
            return +1; /* key exists */
        }
        if (kvraw_append(kvdb->kvraw,
                         key,
                         key_len,
                         val,
                         (*val_len),
                         ref)) {
            TRACE(0);
            return -1;
        }
        ++kvdb->size;
    } else if (MUTATE_UPDATE == mode) {
        if (!off || !val_len_) {
            ++kvdb->size;
        } else {
            ++kvdb->waste;
        }
        if (kvraw_append(kvdb->kvraw,
                         key,
                         key_len,
                         val,
                         (*val_len),
                         ref)) {
            TRACE(0);
            return -1;
        }
    } else if (MUTATE_REPLACE == mode) {
        if (!off || !val_len_) {
            return +1; /* invalid key */
        }
        if (kvraw_append(kvdb->kvraw,
                         key,
                         key_len,
                         val,
                         (*val_len),
                         ref)) {
            TRACE(0);
            return -1;
        }
        ++kvdb->waste;
    }
    return 0;
}

static struct kvdb *
open(const char *pathname, bool enable_persistence) {
    struct kvdb *kvdb;
    printf("opening %s with persistence %d \n", pathname, enable_persistence);
    assert(safe_strlen(pathname));

    if (!(kvdb = malloc(sizeof(struct kvdb)))) {
        TRACE("out of memory");
        return NULL;
    }
    memset(kvdb, 0, sizeof(struct kvdb));
    if (!(kvdb->kvraw = kvraw_open(pathname, enable_persistence)) ||
        !(kvdb->index = index_open())) {
        kvdb_close(kvdb);
        TRACE(0);
        return NULL;
    }
    if (enable_persistence) {
        u64 buf_len = 0;
        u8 *buf = kvraw_getindex(kvdb->kvraw, &buf_len);
        kvdb->index = index_deserialize(buf, buf_len);
        printf("Loaded index with %ld bytes\n", buf_len);
        index_print(kvdb->index);
    }
    return kvdb;
}

struct kvdb *kvdb_open(const char *pathname) {
    return open(pathname, false);
}

struct kvdb *kvdb_open_persistent(const char *pathname) {
    return open(pathname, true);
}

void kvdb_close(struct kvdb *kvdb) {
    if (kvdb) {
        // persist the index
        u64 size = 0;
        u8 *index_buf = index_serialize(kvdb->index, &size);
        printf("Saving index with %ld bytes\n", size);
        index_print(kvdb->index);
        kvraw_saveindex(kvdb->kvraw, index_buf, size);

        free(index_buf);

        kvraw_close(kvdb->kvraw);
        index_close(kvdb->index);
        memset(kvdb, 0, sizeof(struct kvdb));
    }
    FREE(kvdb);
}

int /* -1|0|+1 */
kvdb_remove(struct kvdb *kvdb,
            const void *key,
            uint64_t key_len,
            void *val,
            uint64_t *val_len) {
    assert(kvdb);
    assert(key);
    assert(key_len && (KVDB_MAX_KEY_LEN >= key_len));
    assert(!val_len || !!(*val_len) || val);

    return mutate(kvdb, key, key_len, val, val_len, MUTATE_REMOVE);
}

int /* -1|0|+1 */
kvdb_insert(struct kvdb *kvdb,
            const void *key,
            uint64_t key_len,
            const void *val,
            uint64_t val_len) {
    assert(kvdb);
    assert(key);
    assert(key_len && (KVDB_MAX_KEY_LEN >= key_len));
    assert(val);
    assert(val_len && (KVDB_MAX_VAL_LEN >= val_len));

    return mutate(kvdb,
                  key,
                  key_len,
                  (void *)val,
                  &val_len,
                  MUTATE_INSERT);
}

int /* -1|0|+1 */
kvdb_update(struct kvdb *kvdb,
            const void *key,
            uint64_t key_len,
            const void *val,
            uint64_t val_len) {
    assert(kvdb);
    assert(key);
    assert(key_len && (KVDB_MAX_KEY_LEN >= key_len));
    assert(val);
    assert(val_len && (KVDB_MAX_VAL_LEN >= val_len));

    return mutate(kvdb,
                  key,
                  key_len,
                  (void *)val,
                  &val_len,
                  MUTATE_UPDATE);
}

int /* -1|0|+1 */
kvdb_replace(struct kvdb *kvdb,
             const void *key,
             uint64_t key_len,
             const void *val,
             uint64_t val_len) {
    assert(kvdb);
    assert(key);
    assert(key_len && (KVDB_MAX_KEY_LEN >= key_len));
    assert(val);
    assert(val_len && (KVDB_MAX_VAL_LEN >= val_len));

    return mutate(kvdb,
                  key,
                  key_len,
                  (void *)val,
                  &val_len,
                  MUTATE_REPLACE);
}

int /* -1|0|+1 */
kvdb_lookup(struct kvdb *kvdb,
            const void *key,
            uint64_t key_len,
            void *val,
            uint64_t *val_len) {
    const uint64_t *ref;
    uint64_t val_len_;
    uint64_t off;
    void *val_;

    assert(kvdb);
    assert(key);
    assert(key_len && (KVDB_MAX_KEY_LEN >= key_len));
    assert(!val_len || !(*val_len) || val);

    /* index */
    ref = index_lookup(kvdb->index, key, key_len);
    if (!ref || !(*ref)) {
        return +1; /* invalid key */
    }
    off = (*ref);

    /* chained */

    val_ = val_len ? val : NULL;
    val_len_ = val_len ? (*val_len) : 0;
    if (chain_lookup(kvdb, key, key_len, val_, &val_len_, &off)) {
        return -1;
    }
    if (!off || !val_len_) {
        return +1; /* invalid key */
    }
    if (val_len) {
        (*val_len) = val_len_;
    }
    return 0;
}

uint64_t
kvdb_size(const struct kvdb *kvdb) {
    assert(kvdb);

    return kvdb->size;
}

uint64_t
kvdb_waste(const struct kvdb *kvdb) {
    assert(kvdb);

    return kvdb->waste;
}
