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
#include <sys/time.h>
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

//////////////
// Region
/////////////

typedef struct Region {
    u64 address;
    u64 size;
} Region;

static inline Region new_region(usize address, u64 size) {
    Region r;
    r.address = address;
    r.size = size;
    return r;
}

static inline u64 region_end(Region r) {
    return r.address + r.size;
}

/////////////
// Metadata
////////////

#define RESERVED_BLOCKS 1

typedef struct Metadata {
    char tag[6];
    // write cursor
    u64 current_block;
    u64 current_offset;
    // where the persisted index is stored
    Region index;
} Metadata;

void meta_init(Metadata *metadata) {
    // the first block is reserved for metadata.
    // LOGFS always starts at block 1.
    metadata->current_block = RESERVED_BLOCKS;
    metadata->current_offset = 0;
    metadata->index.address = 0;
    metadata->index.size = 0;
    strcpy(metadata->tag, "LOGFS");
}

void meta_save(Metadata *metadata, struct device *block) {
    int blk_size = device_block(block);
    static u8 *page;
    if (page == NULL) {
        page = malloc(blk_size);
    }
    memset(page, 0, blk_size);
    memcpy(page, metadata, sizeof(Metadata));
    device_write(block, page, blk_size, blk_size);
}

Metadata meta_load(struct device *block) {
    int blk_size = device_block(block);
    static u8 *page;
    if (page == NULL) {
        page = malloc(blk_size);
    }
    memset(page, 0, blk_size);
    device_read(block, page, blk_size, blk_size);
    Metadata metadata;
    memcpy(&metadata, page, sizeof(Metadata));

    if (strcmp(metadata.tag, "LOGFS") != 0) {
        printf("Corrupt metadata or block device is not initialized with LogFS\n");
        meta_init(&metadata);
    }
    // meta_init(&metadata);
    printf("Loaded metadata: cursor=(%d,%d), index=(%d,%d)\n", metadata.current_block, metadata.current_offset, metadata.index.address, metadata.index.size);

    return metadata;
}

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

WriteBuffer *wb_init(struct device *block, Metadata meta) {
    // TODO add metadata loading
    WriteBuffer *wb = malloc(sizeof(WriteBuffer));
    wb->device = block;
    wb->block_size = device_block(block);

    wb->buf = calloc(device_block(block), WCACHE_BLOCKS);
    wb->buf_size = device_block(block) * WCACHE_BLOCKS;

    wb->shutdown = false;
    wb->is_full = false;

    pthread_mutex_init(&wb->access_mutex, NULL);

    pthread_mutex_init(&wb->append_cond_mutex, NULL);
    pthread_cond_init(&wb->append_waiting_for_space, NULL);

    pthread_mutex_init(&wb->write_cond_mutex, NULL);
    pthread_cond_init(&wb->write_waiting_for_data_to_flush, NULL);

    wb->current_block = meta.current_block;
    // If the write cursor was in the middle of a block, we can simulate
    // that by loading the incomplete block into the write buffer.
    if (meta.current_offset > 0) {
        device_read(block, wb->buf, wb->current_block * wb->block_size, wb->block_size);
        wb->append_head = meta.current_offset;
    } else {
        wb->append_head = 0;
    }
    wb->write_head = 0;

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
 * Returns the offset within the write buffer for a certain address.
 * i.e. data for location will be present in wb->write_buffer[wb_locate(wb, location)]
 *
 * Assumes that the caller holds the access_mutex.
 */
static inline u64 wb_locate(WriteBuffer *wb, u64 address) {
    u64 wb_start = wb->current_block * wb->block_size;
    u64 wb_offset = address - wb_start;
    u64 location = (wb->write_head + wb_offset) % wb->buf_size;
    assert(location >= 0 && location < wb->buf_size);
    return location;
}

typedef enum Strategy {
    WRITE_BUFFER = 42,
    CACHE,
    BOTH
} Strategy;

typedef struct FetchPlan {
    Region disk_region;
    Region wb_region;
    Strategy strategy;
} FetchPlan;

/**
 * Splits a region into two FetchPlan, one to be served by the cache
 * and one contained within the write buffer
 *
 * We're doing it this way to avoid having to copy too much data.
 * RegionPair is pure metadata, so stack allocation is probably faster.
 *
 * Note: a region will have size 0 if it not part of the plan
 *
 * This function expects the caller to hold the access_mutex.
 */
FetchPlan wb_analyze(WriteBuffer *wb, Region region) {
    FetchPlan plan;
    plan.disk_region.size = 0;
    plan.wb_region.size = 0;

    // the region currently contained in the write buffer
    usize wb_start = wb->current_block * wb->block_size;
    usize wb_end = wb_start + wb_usedspace(wb);

    if (region_end(region) < wb_start || region.address > wb_end) {
        // the requested region is completely in cache
        plan.disk_region = region;
        plan.strategy = CACHE;
        return plan;
    } else if (region.address >= wb_start && region_end(region) <= wb_end) {
        // the requested region is completely in write-buffer
        plan.wb_region = region;
        plan.strategy = WRITE_BUFFER;
        return plan;
    } else {
        // the data is split across the write buffer and cache.
        // In this case, the write buffer gets priority.
        // We assume that the required data is at the start of the write buffer.
        // This makes sense for a split read.

        plan.wb_region = new_region(wb_start, region_end(region) - wb_start);

        plan.disk_region = region;
        plan.disk_region.size = region.size - plan.wb_region.size;
        plan.strategy = BOTH;
    }
    assert(plan.disk_region.size + plan.wb_region.size == region.size);
    return plan;
}

/**
 * Read a specific block of data from the write buffer.
 * Returns True if the block was read successfully, False otherwise.
 *
 * Expects caller to hold the access_mutex.
 */
bool wb_read(WriteBuffer *wb, u8 *buffer, Region region) {
    usize wb_start = wb->current_block * wb->block_size;
    usize wb_end = wb_start + wb_usedspace(wb);

    if (region.address < wb_start || region.address > wb_end) {
        return false;
    }
    u64 location = wb_locate(wb, region.address);
    if (location + region.size > wb->buf_size) {
        // wraparound read necessary
        int first_part_len = wb->buf_size - location;
        log("[wb] [%ld..%ld](%ld) + [wb][%ld..%ld](%ld)\n", location, location + first_part_len, first_part_len, 0l, region.size - first_part_len, region.size - first_part_len);
        memcpy(buffer, wb->buf + location, first_part_len);
        memcpy(buffer + first_part_len, wb->buf, region.size - first_part_len);
    } else {
        log("[wb] [%ld..%ld](%ld)\n", location, location + region.size, region.size);
        memcpy(buffer, wb->buf + location, region.size);
    }

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

    if (wb->append_head + size > wb->buf_size) {
        u64 first_part_size = wb->buf_size - wb->append_head;
        memcpy(wb->buf + wb->append_head, data, first_part_size);
        memcpy(wb->buf, data + first_part_size, size - first_part_size);
    } else {
        memcpy(wb->buf + wb->append_head, data, size);
    }
    wb->append_head = (wb->append_head + size) % wb->buf_size;

    if (wb_usedspace(wb) >= (u64)wb->block_size) {
        pthread_cond_signal(&wb->write_waiting_for_data_to_flush);
    }
    pthread_mutex_unlock(&wb->access_mutex);
}

/**
 * This buffer belongs to wb_flush.
 * It _should_ just be a static stack variable, but then we wouldn't be able to free it.
 */
static u8 *virtual_page = NULL;

void wb_flush(WriteBuffer *wb, bool flush_partial) {
    if (virtual_page == NULL) {
        virtual_page = malloc(wb->block_size);
    }
    memset(virtual_page, 0, wb->block_size);

    pthread_mutex_lock(&wb->access_mutex);

    int blocks_written = 0;
    int target_blocks = wb_usedspace(wb) / wb->block_size;
    UNUSED(target_blocks);

    while (wb_usedspace(wb) >= (u64)wb->block_size) {
        if ((wb->write_head + wb->block_size) >= wb->buf_size) {
            // the page we want to write is wrapped around the circular buffer.
            // The device needs a continuous region of memory, so we need to recombine them.
            int end_fragment_size = wb->buf_size - wb->write_head;
            memcpy(virtual_page, wb->buf + wb->write_head, end_fragment_size);
            memcpy(virtual_page + end_fragment_size, wb->buf, wb->block_size - end_fragment_size);
            device_write(wb->device, virtual_page, wb->current_block * wb->block_size, wb->block_size);
            blocks_written++;
        } else {
            device_write(wb->device, wb->buf + wb->write_head, wb->current_block * wb->block_size, wb->block_size);
            blocks_written++;
        }
        wb->current_block++;
        wb->write_head = (wb->write_head + wb->block_size) % wb->buf_size;
    }
    if (flush_partial && wb->write_head < wb->append_head) {
        memcpy(virtual_page, wb->buf + wb->write_head, wb->append_head - wb->write_head);
        device_write(wb->device, virtual_page, wb->current_block * wb->block_size, wb->block_size);
    }

    wb->is_full = false;
    pthread_mutex_unlock(&wb->access_mutex);
    pthread_cond_signal(&wb->append_waiting_for_space);
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

        wb_flush(buf, false);

        struct timespec timeToWait;
        struct timeval now;
        int rt;
        gettimeofday(&now, NULL);
        timeToWait.tv_sec = now.tv_sec + 1;
        timeToWait.tv_nsec = (now.tv_usec) * 1000UL;

        pthread_mutex_lock(&buf->write_cond_mutex);
        pthread_cond_timedwait(&buf->write_waiting_for_data_to_flush, &buf->write_cond_mutex, &timeToWait);
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
void rc_read(ReadCache *rc, u8 *buf, Region region) {
    pthread_mutex_lock(&rc->access_mutex);
    int current_page = region.address / rc->block_size;
    int page_offset = region.address % rc->block_size;

    u64 copied_bytes = 0;
    while (copied_bytes < region.size) {
        u8 *page_data = rc_getpage(rc, current_page);
        u8 *data_to_copy = page_data + page_offset;
        int length_to_copy = MIN(region.size - copied_bytes, rc->block_size - page_offset);

        log("[rc] %d[%d..%d]<%d>\n", current_page, page_offset, page_offset + length_to_copy, length_to_copy);
        memcpy(buf + copied_bytes, data_to_copy, length_to_copy);

        page_offset = 0;
        copied_bytes += length_to_copy;
        current_page++;
    }
    assert(copied_bytes == region.size);
    pthread_mutex_unlock(&rc->access_mutex);
}

//////////////
//// LogFS
/////////////

typedef struct logfs {
    WriteBuffer *wb;
    ReadCache *cache;
    Metadata meta;
} LogFS;

int logfs_read(struct logfs *logfs, void *buf, uint64_t off, size_t len) {
    // offset to account for the "hidden" first page
    Region region = new_region(off + logfs->wb->block_size, len);

    pthread_mutex_lock(&logfs->wb->access_mutex);
    FetchPlan plan = wb_analyze(logfs->wb, region);

    if (plan.strategy == CACHE) {
        // we no longer need this lock
        pthread_mutex_unlock(&logfs->wb->access_mutex);
    }

#ifdef DEBUG
    if (plan.strategy == BOTH) {
        log("=============Using split read\n");
    }
#endif

    if (plan.strategy == CACHE || plan.strategy == BOTH) {
        rc_read(logfs->cache, (u8 *)buf, plan.disk_region);
    }
    if (plan.strategy == WRITE_BUFFER || plan.strategy == BOTH) {
        wb_read(logfs->wb, (u8 *)buf + plan.disk_region.size, plan.wb_region);
        pthread_mutex_unlock(&logfs->wb->access_mutex);
    }

    return 0;
}

int logfs_append(struct logfs *logfs, const void *buf, uint64_t len) {
    rc_invalidate(logfs->cache, logfs->wb->current_block);
    wb_append(logfs->wb, (const u8 *)buf, len);
    return 0;
}

struct logfs *logfs_open(const char *pathname, bool enable_persistence) {
    struct device *block = device_open(pathname);
    LogFS *logfs = malloc(sizeof(LogFS));
    // if (enable_persistence) {
    //     logfs->meta = meta_load(block);
    // } else {
    // }
    meta_init(&logfs->meta);

    logfs->wb = wb_init(block, logfs->meta);
    logfs->cache = rc_init(block);
    return logfs;
}

void logfs_close(struct logfs *logfs) {
    wb_shutdown(logfs->wb);
    //logfs->meta.current_block = logfs->wb->current_block;
    //logfs->meta.current_offset = logfs->wb->append_head % logfs->wb->block_size;
    //meta_save(&logfs->meta, logfs->wb->device);

    // free write buffer
    free(logfs->wb->device);
    free(logfs->wb->buf);
    free(logfs->wb);
    // free read cache
    free(logfs->cache->read_cache);
    free(logfs->cache);

    free(logfs);

    FREE(virtual_page);
}

u64 logfs_getsize(struct logfs *logfs) {
    return (logfs->meta.current_block - RESERVED_BLOCKS) * logfs->wb->block_size + logfs->meta.current_offset;
}

void logfs_setmeta(struct logfs *logfs, u64 index_offset, u64 index_len) {
    printf("Setting meta range to %d, %d\n", index_offset, index_len);
    logfs->meta.index.address = index_offset;
    logfs->meta.index.size = index_len;
}

u8 *logfs_readindex(struct logfs *logfs, /*out*/ u64 *len) {
    u8 *buf = malloc(logfs->meta.index.size);
    printf("Reading index %ld..%d from logfs into buf (size %ld)\n", logfs->meta.index.address, logfs->meta.index.address + logfs->meta.index.size, logfs->meta.index.size);
    logfs_read(logfs, buf, logfs->meta.index.address, logfs->meta.index.size);
    *len = logfs->meta.index.size;
    return buf;
}