/**
 * Tony Givargis
 * Copyright (C), 2023
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * logfs.c
 */

#include "logfs.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>

#include "device.h"
#include "utils.h"

#define WCACHE_BLOCKS 32
#define RCACHE_BLOCKS 256

/**
 * Needs:
 *   pthread_create()
 *   pthread_join()
 *   pthread_mutex_init()
 *   pthread_mutex_destroy()
 *   pthread_mutex_lock()
 *   pthread_mutex_unlock()
 *   pthread_cond_init()
 *   pthread_cond_destroy()
 *   pthread_cond_wait()
 *   pthread_cond_signal()
 */

#define u8 uint_fast8_t
#define u64 uint64_t

///////////////
// WriteBuffer
//////////////

typedef struct WriteBuffer {
    struct device *device;
    int block_size;
    // current page/block number in device
    int current_block;

    // the main buffer
    u8 *buf;
    int buf_size;

    // indexes into the buffer
    // where we append into the buffer
    int append_head;
    // where we read from the buffer to write to disk
    int write_head;

    // flags
    bool shutdown;
    bool is_full;

    // mutex protecting the data in this struct
    pthread_mutex_t access_mutex;

    pthread_t write_thread;
    pthread_mutex_t write_cond_mutex;
    pthread_cond_t write_waiting_for_data_to_flush;

    // append happens in the caller's thread, so no thread for it
    pthread_mutex_t append_cond_mutex;
    pthread_cond_t append_waiting_for_space;
} WriteBuffer;

static void *worker_loop(WriteBuffer *buf);

WriteBuffer *wb_init(struct device *block) {
    WriteBuffer *wb = malloc(sizeof(WriteBuffer));
    wb->device = block;
    wb->current_block = 0;
    wb->block_size = device_block(block);

    int buf_size = device_block(block) * WCACHE_BLOCKS;
    wb->buf = calloc(device_block(block), WCACHE_BLOCKS);
    wb->buf_size = buf_size;

    wb->append_head = 0;
    wb->write_head = 0;

    wb->shutdown = false;
    wb->is_full = false;

    pthread_mutex_init(&wb->access_mutex, NULL);

    pthread_mutex_init(&wb->append_cond_mutex, NULL);
    pthread_cond_init(&wb->append_waiting_for_space, NULL);

    pthread_mutex_init(&wb->write_cond_mutex, NULL);
    pthread_cond_init(&wb->write_waiting_for_data_to_flush, NULL);
    pthread_create(&wb->write_thread, NULL, (void *(*)(void *))worker_loop, wb);

    return wb;
}

/**
 * Get the current amount of used space in the write buffer.
 * Assumes that the caller holds the access_mutex.
 */
static inline u64 wb_usedspace(WriteBuffer *buf) {
    int size = buf->append_head - buf->write_head;
    // We use a circular buffer, so we need to add this correction
    if (size < 0) {
        size += buf->buf_size;
    }
    return size;
}

/**
 * Get the block size of the attached block device.
 * Assumes that the caller holds the access_mutex (though it shouldn't matter).
 */
static inline u64 wb_block(WriteBuffer *buf) {
    return device_block(buf->device);
}

/**
 * Get the current amount of free space in the write buffer.
 * Assumes that the caller holds the access_mutex.
 */
static inline u64 wb_freespace(WriteBuffer *buf) {
    return buf->buf_size - wb_usedspace(buf);
}

/**
 * Read a specific block of data from the write buffer.
 * Returns True if the block was read successfully, False otherwise.
 */
// TODO fix
bool wb_read(WriteBuffer *wb, int address, u8 *buffer, int size) {
    pthread_mutex_lock(&wb->access_mutex);
    int wb_start = wb->current_block * wb->block_size;
    int wb_end = wb_start + wb_usedspace(wb);

    if (address < wb_start || address > wb_end) {
        // printf("Not in wb\n");
        pthread_mutex_unlock(&wb->access_mutex);
        return false;
    }

    // printf("Reading %d...%d[%d bytes] (wb=%d) from write buffer\n", address, address + size, size, wb->buf_size);
    int wb_offset = address - wb_start;
    int location = (wb->write_head + wb_offset) % wb->buf_size;
    assert(location >= 0 && location < wb->buf_size);
    if (location + size > wb->buf_size) {
        // wraparound read necessary
        int first_part_len = wb->buf_size - location;
        printf("wb[%d..%d]+wb[%d..%d]\n", location, location + first_part_len, 0, size - first_part_len);
        memcpy(buffer, wb->buf + location, first_part_len);
        memcpy(buffer + first_part_len, wb->buf, size - first_part_len);
    } else {
        printf("wb[%d..%d]\n", location, location + size);
        memcpy(buffer, wb->buf + location, size);
    }

    pthread_mutex_unlock(&wb->access_mutex);
    return true;
}

void wb_append(WriteBuffer *wb, const u8 *data, u64 size) {
    // If the buffer is full, wake up a thread and wait for it to flush.
    if (wb_freespace(wb) < size) {
        pthread_mutex_lock(&wb->access_mutex);
        wb->is_full = true;
        pthread_mutex_unlock(&wb->access_mutex);
        while (true) {
            pthread_cond_signal(&wb->write_waiting_for_data_to_flush);
            pthread_mutex_lock(&wb->append_cond_mutex);
            printf("Waiting for space...");
            pthread_cond_wait(&wb->append_waiting_for_space, &wb->append_cond_mutex);
            pthread_mutex_unlock(&wb->append_cond_mutex);
            if (wb_freespace(wb) >= size) {
                break;
            }
        }
    }

    pthread_mutex_lock(&wb->access_mutex);
    memcpy(wb->buf + wb->append_head, data, size);
    wb->append_head = (wb->append_head + size) % wb->buf_size;

    // printf("%ld bytes in wb (append=%d, write=%d)...", wb_usedspace(buf), buf->append_head, buf->write_head);

    if (wb_usedspace(wb) >= (u64)wb->block_size) {
        pthread_cond_signal(&wb->write_waiting_for_data_to_flush);
    } else {
    }
    pthread_mutex_unlock(&wb->access_mutex);
    // printf("done.\n");
}

void wb_flush(WriteBuffer *wb, bool flush_partial) {
    // printf("Flushing...");
    // fflush(stdout);
    pthread_mutex_lock(&wb->access_mutex);
    // printf("got access lock...");
    // fflush(stdout);

    int blocks_written = 0;
    int target_blocks = wb_usedspace(wb) / wb->block_size;
    UNUSED(target_blocks);

    while (wb_usedspace(wb) >= (u64)wb->block_size) {
        if ((wb->write_head + wb->block_size) >= wb->buf_size) {
            // the page we want to write is wrapped around the circular buffer.
            // We need to recombine them.
            u8 *virtual_page = malloc(wb->block_size);
            memset(virtual_page, 0, wb->block_size);
            int end_fragment_size = wb->buf_size - wb->write_head;
            memcpy(virtual_page, wb->buf + wb->write_head, end_fragment_size);
            memcpy(virtual_page + end_fragment_size, wb->buf, wb->block_size - end_fragment_size);
            device_write(wb->device, virtual_page, wb->current_block * wb->block_size, wb->block_size);
            // printf("Wrote wraparound block at %d\n", wb->current_block * wb->block_size);
            blocks_written++;
        } else {
            device_write(wb->device, wb->buf + wb->write_head, wb->current_block * wb->block_size, wb->block_size);
            // printf("Wrote block to %d\n", wb->current_block * wb->block_size);
            blocks_written++;
        }
        wb->current_block++;
        wb->write_head = (wb->write_head + wb->block_size) % wb->buf_size;
    }
    if (flush_partial && wb->write_head < wb->append_head) {
        u8 *virtual_page = malloc(wb->block_size);
        memset(virtual_page, 0, wb->block_size);
        memcpy(virtual_page, wb->buf + wb->write_head, wb->append_head - wb->write_head);
        device_write(wb->device, virtual_page, wb->current_block * wb->block_size, wb->block_size);
        // printf("Wrote partial block at %d\n", wb->current_block * wb->block_size);
    }

    wb->is_full = false;
    pthread_mutex_unlock(&wb->access_mutex);
    pthread_cond_signal(&wb->append_waiting_for_space);
    // printf("wrote %d of %d blocks.\n", blocks_written, target_blocks);
}

void wb_shutdown(WriteBuffer *buf) {
    pthread_mutex_lock(&buf->access_mutex);
    buf->shutdown = true;
    pthread_cond_signal(&buf->write_waiting_for_data_to_flush);
    pthread_mutex_unlock(&buf->access_mutex);
    pthread_join(buf->write_thread, NULL);
}

static void *worker_loop(WriteBuffer *buf) {
    while (true) {
        if (buf->shutdown) {
            return NULL;
        }

        // printf("Beginning flush...\n");
        wb_flush(buf, false);

        pthread_mutex_lock(&buf->write_cond_mutex);
        // printf("Worker waiting for data to flush...\n");
        pthread_cond_wait(&buf->write_waiting_for_data_to_flush, &buf->write_cond_mutex);
        pthread_mutex_unlock(&buf->write_cond_mutex);
    }
}

///////////////
/// Read Cache
///////////////

typedef struct ReadCache {
    struct device *block;
    int block_size;
    u8 *read_cache;
    // the pages contained in the read_cache.
    // -1 means the page is not free.
    int pages[RCACHE_BLOCKS];
    // simple round-robin eviction policy.
    // TODO second-chance/LRU
    int eviction_index;
    pthread_mutex_t access_mutex;
} ReadCache;

static ReadCache *rc_init(struct device *block) {
    ReadCache *rc = malloc(sizeof(ReadCache));
    rc->block = block;
    rc->block_size = device_block(block);
    rc->read_cache = malloc(device_block(block) * RCACHE_BLOCKS);
    memset(rc->pages, -1, RCACHE_BLOCKS * sizeof(int));
    rc->eviction_index = 0;
    pthread_mutex_init(&rc->access_mutex, NULL);
    return rc;
}

/**
 * Return the first free page. If no free page, evict the page identified by eviction_index.
 *
 * Assumes caller holds access_mutex.
 */
static int get_free_page(ReadCache *rc) {
    for (int i = 0; i < RCACHE_BLOCKS; i++) {
        if (rc->pages[i] == -1) {
            return i;
        }
    }
    int page_to_return = rc->eviction_index;
    rc->eviction_index = (rc->eviction_index + 1) % RCACHE_BLOCKS;
    return page_to_return;
}

static void rc_invalidate(ReadCache *rc, int page_no) {
    for (int i = 0; i < RCACHE_BLOCKS; i++) {
        if (rc->pages[i] == page_no) {
            rc->pages[i] = -1;
        }
    }
}

/**
 * Get the page at the given page number.
 * If it doesn't exist in the cache, read it from the device and store in the cache first.
 *
 * Will return a pointer to the page in the cache. This memory is not thread-safe, and must be copied elsewhere before releasing the lock.
 *
 * Assumes that the caller holds access_mutex.
 */
static u8 *rc_getpage(ReadCache *rc, int page_no) {
    for (int i = 0; i < RCACHE_BLOCKS; i++) {
        if (rc->pages[i] == page_no) {
            return rc->read_cache + i * rc->block_size;
        }
    }
    // page not in cache
    int page_to_replace = get_free_page(rc);
    rc->pages[page_to_replace] = page_no;
    u8 *page_ptr = rc->read_cache + (page_to_replace * rc->block_size);
    device_read(rc->block, page_ptr, page_no * rc->block_size, rc->block_size);
    return rc->read_cache + page_to_replace * rc->block_size;
}

/**
 * Read data from the cache into the given buffer.
 *
 * Will query the cache for the page containing the given address. Can handle data spanning mulitple pages.
 * Guaranteed to succeed, assuming that the address is valid and buffer is large enough.
 *
 * Threadsafe & reentrant.
 */
void rc_read(ReadCache *rc, int address, u8 *buf, u64 size) {
    // UNUSED(rc_getpage);
    // pthread_mutex_lock(&rc->access_mutex);
    // u8 *temp_buf = calloc(1, rc->block_size);
    // device_read(rc->block, temp_buf, address - (address % rc->block_size), rc->block_size);
    // memcpy(buf, temp_buf + address % rc->block_size, size);

    printf("[RC]data[%d..%d] = \n", address, address + size);
    int current_page = address / rc->block_size;
    int page_offset = address % rc->block_size;

    u64 copied_bytes = 0;
    while (copied_bytes < size) {
        u8 *page_data = rc_getpage(rc, current_page);
        u8 *data_to_copy = page_data + page_offset;
        int length_to_copy = MIN(size - copied_bytes, rc->block_size - page_offset);

        printf(" + %d[%d..%d](%s) \n", current_page, page_offset, page_offset + length_to_copy, dump_bytes(data_to_copy, length_to_copy));
        memcpy(buf + copied_bytes, data_to_copy, length_to_copy);

        page_offset = 0;
        copied_bytes += length_to_copy;
        current_page++;
    }
    printf("---END----\n");
    assert(copied_bytes == size);
    pthread_mutex_unlock(&rc->access_mutex);
}

//////////////
//// LogFS
/////////////

typedef struct logfs {
    WriteBuffer *wb;
    ReadCache *cache;
} LogFS;

int logfs_read(struct logfs *logfs, void *buf, uint64_t off, size_t len) {
    bool was_in_wb = wb_read(logfs->wb, off, (u8 *)buf, len);
    if (!was_in_wb) {
        rc_read(logfs->cache, off, (u8 *)buf, len);
    }
    return 0;
}

int logfs_append(struct logfs *logfs, const void *buf, uint64_t len) {
    rc_invalidate(logfs->cache, logfs->wb->current_block);
    wb_append(logfs->wb, (const u8 *)buf, len);
    return 0;
}

struct logfs *logfs_open(const char *pathname) {
    struct device *block = device_open(pathname);
    LogFS *logfs = malloc(sizeof(LogFS));
    logfs->wb = wb_init(block);
    logfs->cache = rc_init(block);
    return logfs;
}

void logfs_close(struct logfs *logfs) {
    wb_shutdown(logfs->wb);
}
