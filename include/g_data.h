#ifndef G_DATA_H
#define G_DATA_H

#include "hmap.h"
#include "dlist.h"
#include "heap.h"

struct g_data {
    struct HMap kv_db;
    struct DList idle_list;
    struct Heap ttl_heap;
    struct DList conn_pool;
    int pooled_conn_count;

    uint64_t start_time_ms;
    uint64_t stat_commands_processed;
    uint64_t stat_hits;
    uint64_t stat_misses;
    uint32_t active_clients;

    uint64_t stat_auth_success;
    uint64_t stat_auth_failures;
    uint64_t stat_auth_lockouts;

    size_t stat_used_memory;
    uint64_t stat_evicted_keys;
};

int init_g_data(struct g_data* gd);

#endif