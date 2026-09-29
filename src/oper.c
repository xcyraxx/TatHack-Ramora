#include "oper.h"
#include <inttypes.h>
#include <errno.h>
#include <fnmatch.h>
#include <string.h>
#include <strings.h>

#define container_of(ptr, T, member) \
    ((T *)( (char *)ptr - offsetof(T, member) ))

int set_ttl_Entry_ms(char* key, int64_t ttl){
    struct HMap* hmap = &gd.kv_db;

    uint64_t hcode_key = str_hash(key, strlen(key));
    struct HNode** from = lookup_HMap(hmap, key, hcode_key, entry_eq);

    if (from == NULL || *from == NULL){
        return 0;
    }

    struct Entry* entry = (struct Entry*)container_of(*from, struct Entry, node);
    if (ttl == -1){
        if (entry->heap_idx != (size_t)-1){
            heap_delete(&gd.ttl_heap, entry->heap_idx);
            entry->heap_idx = (size_t)-1;
        }
        return 1;
    }

    uint64_t expiration_time = get_monotonic_msec() + (uint64_t)ttl;

    if (entry->heap_idx == (size_t)-1){
        if (heap_insert(&gd.ttl_heap, expiration_time, &entry->heap_idx) == 0){
            return -1;
        }
    }
    else {
        gd.ttl_heap.arr[entry->heap_idx].expiration_time = expiration_time;
        heap_update(gd.ttl_heap.arr, entry->heap_idx, gd.ttl_heap.sz);
    }
    return 1;
}

int64_t get_ttl_Entry_ms(char* key){
    struct HMap* hmap = &gd.kv_db;

    uint64_t hcode_key = str_hash(key, strlen(key));
    struct HNode** from = lookup_HMap(hmap, key, hcode_key, entry_eq);

    if (from == NULL || *from == NULL){
        return -2; // -2 for RES_NX -> key not present
    }

    struct Entry* entry = (struct Entry*)container_of(*from, struct Entry, node);
    if (entry->heap_idx == (size_t)-1){
        return -1; // -1 for no ttl -> not set for expiration
    }

    int64_t ttl = gd.ttl_heap.arr[entry->heap_idx].expiration_time - get_monotonic_msec();
    if (ttl < 0) ttl = 0;
    return ttl;
}

int set_ttl_Entry_sec(char* key, int64_t ttl){
    if (ttl == -1){
        return set_ttl_Entry_ms(key, -1);
    }
    if (ttl < 0){
        return -1;
    }
    return set_ttl_Entry_ms(key, ttl * 1000);
}

int64_t get_ttl_Entry_sec(char* key){
    int64_t ttl = get_ttl_Entry_ms(key);
    if (ttl < 0) return ttl;
    else return ttl / 1000;
}

struct buf* get_Entry(char* key){
    struct HMap* hmap = &gd.kv_db;

    uint64_t hcode_key = str_hash(key, strlen(key));
    struct HNode** from = lookup_HMap(hmap, key, hcode_key, entry_eq);

    if (from == NULL || *from == NULL){
        gd.stat_misses++;
        return NULL;
    }

    struct Entry* entry = container_of(*from, struct Entry, node);
    if (entry->heap_idx != (size_t)-1){
        if (gd.ttl_heap.arr[entry->heap_idx].expiration_time <= get_monotonic_msec()){
            del_Entry(key);
            gd.stat_misses++;
            return NULL;
        }
    }

    gd.stat_hits++;
    entry->last_accessed = get_monotonic_msec();
    return entry->val;
}

size_t count_keys_Entry(void){
    uint64_t now_ms = get_monotonic_msec();
    size_t count = 0;
    if (gd.kv_db.new_tab && gd.kv_db.new_tab->tab) {
        for (size_t i = 0; i < gd.kv_db.new_tab->cap; i++) {
            for (struct HNode* curr = gd.kv_db.new_tab->tab[i]; curr != NULL; curr = curr->next) {
                struct Entry* entry = container_of(curr, struct Entry, node);
                if (entry->heap_idx != (size_t)-1 && gd.ttl_heap.arr[entry->heap_idx].expiration_time <= now_ms) {
                    continue;
                }
                count++;
            }
        }
    }
    if (gd.kv_db.is_migrating && gd.kv_db.old_tab && gd.kv_db.old_tab->tab) {
        for (size_t i = gd.kv_db.migrating_pos; i < gd.kv_db.old_tab->cap; i++) {
            for (struct HNode* curr = gd.kv_db.old_tab->tab[i]; curr != NULL; curr = curr->next) {
                struct Entry* entry = container_of(curr, struct Entry, node);
                if (entry->heap_idx != (size_t)-1 && gd.ttl_heap.arr[entry->heap_idx].expiration_time <= now_ms) {
                    continue;
                }
                count++;
            }
        }
    }
    return count;
}

static inline int match_key(const char* pattern, const char* key) {
    if (pattern[0] == '*' && pattern[1] == '\0') {
        return 1;
    }
    return fnmatch(pattern, key, 0) == 0;
}

int get_keys_Entry(const char* pattern, struct buf* wbuf) {
    uint64_t now_ms = get_monotonic_msec();
    uint32_t count = 0;

    if (gd.kv_db.new_tab && gd.kv_db.new_tab->tab) {
        for (size_t i = 0; i < gd.kv_db.new_tab->cap; i++) {
            for (struct HNode* curr = gd.kv_db.new_tab->tab[i]; curr != NULL; curr = curr->next) {
                struct Entry* entry = container_of(curr, struct Entry, node);
                if (entry->heap_idx != (size_t)-1 && gd.ttl_heap.arr[entry->heap_idx].expiration_time <= now_ms) {
                    continue;
                }
                const char* k = bufstart(entry->key);
                if (match_key(pattern, k)) {
                    count++;
                }
            }
        }
    }
    if (gd.kv_db.is_migrating && gd.kv_db.old_tab && gd.kv_db.old_tab->tab) {
        for (size_t i = 0; i < gd.kv_db.old_tab->cap; i++) {
            for (struct HNode* curr = gd.kv_db.old_tab->tab[i]; curr != NULL; curr = curr->next) {
                struct Entry* entry = container_of(curr, struct Entry, node);
                if (entry->heap_idx != (size_t)-1 && gd.ttl_heap.arr[entry->heap_idx].expiration_time <= now_ms) {
                    continue;
                }
                const char* k = bufstart(entry->key);
                if (match_key(pattern, k)) {
                    count++;
                }
            }
        }
    }

    int rv = response_code(wbuf, RES_OK);
    rv &= response_u32(wbuf, count);

    if (count > 0 && rv != 0) {
        if (gd.kv_db.new_tab && gd.kv_db.new_tab->tab) {
            for (size_t i = 0; i < gd.kv_db.new_tab->cap; i++) {
                for (struct HNode* curr = gd.kv_db.new_tab->tab[i]; curr != NULL; curr = curr->next) {
                    struct Entry* entry = container_of(curr, struct Entry, node);
                    if (entry->heap_idx != (size_t)-1 && gd.ttl_heap.arr[entry->heap_idx].expiration_time <= now_ms) {
                        continue;
                    }
                    char* k = bufstart(entry->key);
                    if (match_key(pattern, k)) {
                        rv &= response_str(wbuf, k);
                        if (!rv) return 0;
                    }
                }
            }
        }
        if (gd.kv_db.is_migrating && gd.kv_db.old_tab && gd.kv_db.old_tab->tab) {
            for (size_t i = 0; i < gd.kv_db.old_tab->cap; i++) {
                for (struct HNode* curr = gd.kv_db.old_tab->tab[i]; curr != NULL; curr = curr->next) {
                    struct Entry* entry = container_of(curr, struct Entry, node);
                    if (entry->heap_idx != (size_t)-1 && gd.ttl_heap.arr[entry->heap_idx].expiration_time <= now_ms) {
                        continue;
                    }
                    char* k = bufstart(entry->key);
                    if (match_key(pattern, k)) {
                        rv &= response_str(wbuf, k);
                        if (!rv) return 0;
                    }
                }
            }
        }
    }

    return rv;
}

int scan_Entry(uint64_t cursor, const char* pattern, size_t count, struct buf* wbuf) {
    if (count == 0) count = 10;
    uint64_t now_ms = get_monotonic_msec();

    if (gd.kv_db.new_tab == NULL || gd.kv_db.new_tab->cap == 0 || gd.kv_db.new_tab->size == 0) {
        int rv = response_code(wbuf, RES_OK);
        rv &= response_u32(wbuf, 1);
        rv &= response_str(wbuf, "0");
        return rv;
    }

    size_t cap = gd.kv_db.new_tab->cap;
    if (cursor >= cap) {
        int rv = response_code(wbuf, RES_OK);
        rv &= response_u32(wbuf, 1);
        rv &= response_str(wbuf, "0");
        return rv;
    }

    size_t matched_cap = count > 16 ? count : 16;
    size_t matched_count = 0;
    char** matched_keys = malloc(sizeof(char*) * matched_cap);
    if (!matched_keys) {
        int rv = response_code(wbuf, RES_ERR);
        rv &= response_u32(wbuf, 0);
        return rv;
    }

    size_t b = (size_t)cursor;
    size_t buckets_scanned = 0;
    size_t min_buckets = count < 100 ? 512 : (count * 4);

    while (b < cap) {
        for (struct HNode* curr = gd.kv_db.new_tab->tab[b]; curr != NULL; curr = curr->next) {
            struct Entry* entry = container_of(curr, struct Entry, node);
            if (entry->heap_idx != (size_t)-1 && gd.ttl_heap.arr[entry->heap_idx].expiration_time <= now_ms) {
                continue;
            }
            char* k = bufstart(entry->key);
            if (match_key(pattern, k)) {
                if (matched_count == matched_cap) {
                    matched_cap *= 2;
                    char** new_arr = realloc(matched_keys, sizeof(char*) * matched_cap);
                    if (!new_arr) break;
                    matched_keys = new_arr;
                }
                matched_keys[matched_count++] = k;
            }
        }

        if (gd.kv_db.is_migrating && gd.kv_db.old_tab && b < gd.kv_db.old_tab->cap && b >= gd.kv_db.migrating_pos) {
            for (struct HNode* curr = gd.kv_db.old_tab->tab[b]; curr != NULL; curr = curr->next) {
                struct Entry* entry = container_of(curr, struct Entry, node);
                if (entry->heap_idx != (size_t)-1 && gd.ttl_heap.arr[entry->heap_idx].expiration_time <= now_ms) {
                    continue;
                }
                char* k = bufstart(entry->key);
                if (match_key(pattern, k)) {
                    if (matched_count == matched_cap) {
                        matched_cap *= 2;
                        char** new_arr = realloc(matched_keys, sizeof(char*) * matched_cap);
                        if (!new_arr) break;
                        matched_keys = new_arr;
                    }
                    matched_keys[matched_count++] = k;
                }
            }
        }

        b++;
        buckets_scanned++;

        if (matched_count >= count || buckets_scanned >= min_buckets) {
            break;
        }
    }

    uint64_t next_cursor = (b >= cap) ? 0 : (uint64_t)b;
    char cursor_str[32];
    snprintf(cursor_str, sizeof(cursor_str), "%" PRIu64, next_cursor);

    int rv = response_code(wbuf, RES_OK);
    rv &= response_u32(wbuf, (uint32_t)(1 + matched_count));
    rv &= response_str(wbuf, cursor_str);

    for (size_t i = 0; i < matched_count; i++) {
        rv &= response_str(wbuf, matched_keys[i]);
    }

    free(matched_keys);
    return rv;
}

static inline size_t entry_memory_usage(struct Entry* entry) {
    if (!entry) return 0;
    return sizeof(struct Entry) +
           (entry->key ? bufcap(entry->key) : 0) +
           (entry->val ? bufcap(entry->val) : 0);
}

static struct Entry* pick_random_entry(void) {
    if (!gd.kv_db.new_tab || gd.kv_db.new_tab->size == 0) return NULL;
    size_t cap = gd.kv_db.new_tab->cap;
    size_t start = (size_t)rand() % cap;
    for (size_t i = 0; i < cap; i++) {
        size_t idx = (start + i) % cap;
        if (gd.kv_db.new_tab->tab[idx] != NULL) {
            return container_of(gd.kv_db.new_tab->tab[idx], struct Entry, node);
        }
    }
    return NULL;
}

int evict_if_needed(size_t needed_bytes) {
    if (MAXMEMORY == 0) return 0;
    if (gd.stat_used_memory + needed_bytes <= MAXMEMORY) return 0;

    if (strcasecmp(MAXMEMORY_POLICY, "noeviction") == 0) {
        return -1;
    }

    while (gd.stat_used_memory + needed_bytes > MAXMEMORY) {
        if (strcasecmp(MAXMEMORY_POLICY, "volatile-ttl") == 0) {
            if (gd.ttl_heap.sz == 0) {
                return -1;
            }
            struct Entry* entry = container_of(gd.ttl_heap.arr[0].ref, struct Entry, heap_idx);
            char* k = strdup(bufstart(entry->key));
            if (!k) return -1;
            del_Entry(k);
            free(k);
            gd.stat_evicted_keys++;
        }
        else if (strcasecmp(MAXMEMORY_POLICY, "allkeys-lru") == 0) {
            struct Entry* oldest_entry = NULL;
            for (int s = 0; s < 5; s++) {
                struct Entry* cand = pick_random_entry();
                if (!cand) break;
                if (!oldest_entry || cand->last_accessed < oldest_entry->last_accessed) {
                    oldest_entry = cand;
                }
            }
            if (!oldest_entry) return -1;
            char* k = strdup(bufstart(oldest_entry->key));
            if (!k) return -1;
            del_Entry(k);
            free(k);
            gd.stat_evicted_keys++;
        }
        else {
            struct Entry* cand = pick_random_entry();
            if (!cand) return -1;
            char* k = strdup(bufstart(cand->key));
            if (!k) return -1;
            del_Entry(k);
            free(k);
            gd.stat_evicted_keys++;
        }
    }

    return (gd.stat_used_memory + needed_bytes <= MAXMEMORY) ? 0 : -1;
}

int exists_Entry(char* key){
    struct HMap* hmap = &gd.kv_db;

    uint64_t hcode_key = str_hash(key, strlen(key));
    struct HNode** from = lookup_HMap(hmap, key, hcode_key, entry_eq);

    if (from == NULL || *from == NULL){
        return 0;
    }

    struct Entry* entry = container_of(*from, struct Entry, node);
    if (entry->heap_idx != (size_t)-1){
        if (gd.ttl_heap.arr[entry->heap_idx].expiration_time <= get_monotonic_msec()){
            del_Entry(key);
            return 0;
        }
    }

    return 1;
}

int incr_decr_Entry(char* key, int64_t delta, int64_t* out_val){
    struct HMap* hmap = &gd.kv_db;
    uint64_t hcode_key = str_hash(key, strlen(key));
    struct HNode** from = lookup_HMap(hmap, key, hcode_key, entry_eq);

    if (from == NULL || *from == NULL){
        char numbuf[32];
        snprintf(numbuf, sizeof(numbuf), "%" PRId64, delta);
        if (set_Entry(key, numbuf) == 0){
            return -3;
        }
        *out_val = delta;
        return 0;
    }

    struct Entry* entry = container_of(*from, struct Entry, node);
    if (entry->heap_idx != (size_t)-1){
        if (gd.ttl_heap.arr[entry->heap_idx].expiration_time <= get_monotonic_msec()){
            del_Entry(key);
            char numbuf[32];
            snprintf(numbuf, sizeof(numbuf), "%" PRId64, delta);
            if (set_Entry(key, numbuf) == 0){
                return -3;
            }
            *out_val = delta;
            return 0;
        }
    }

    size_t sz = bufsize(entry->val);
    if (sz == 0){
        return -1;
    }

    char val_str[64];
    if (sz >= sizeof(val_str)){
        return -1;
    }
    memcpy(val_str, bufstart(entry->val), sz);
    val_str[sz] = '\0';

    char* endptr = NULL;
    errno = 0;
    long long parsed = strtoll(val_str, &endptr, 10);
    if (errno == ERANGE){
        return -2;
    }
    if (endptr == val_str || *endptr != '\0'){
        return -1;
    }

    if (delta > 0 && parsed > INT64_MAX - delta){
        return -2;
    }
    if (delta < 0 && parsed < INT64_MIN - delta){
        return -2;
    }

    int64_t result = (int64_t)parsed + delta;
    char numbuf[32];
    int num_len = snprintf(numbuf, sizeof(numbuf), "%" PRId64, result);

    entry->last_accessed = get_monotonic_msec();
    size_t old_mem = entry_memory_usage(entry);
    clearbuf(entry->val);
    if (bufappend(entry->val, numbuf, num_len) == 0){
        return -3;
    }
    size_t new_mem = entry_memory_usage(entry);
    if (new_mem > old_mem) {
        evict_if_needed(new_mem - old_mem);
        gd.stat_used_memory += (new_mem - old_mem);
    } else if (old_mem > new_mem) {
        if (gd.stat_used_memory >= (old_mem - new_mem)) gd.stat_used_memory -= (old_mem - new_mem);
        else gd.stat_used_memory = 0;
    }

    *out_val = result;
    return 0;
}

int set_Entry(char* key, char* val){
    struct HMap* hmap = &gd.kv_db;

    size_t key_len = strlen(key);
    size_t val_len = strlen(val);
    uint64_t hcode_key = str_hash(key, key_len);
    struct HNode** from = lookup_HMap(hmap, key, hcode_key, entry_eq);

    int rv = 1;
    if (from != NULL && *from != NULL){
        struct Entry* found_entry = container_of(*from, struct Entry, node);
        size_t old_mem = entry_memory_usage(found_entry);
        clearbuf(found_entry->val);
        rv &= bufappend(found_entry->val, val, val_len);
        size_t new_mem = entry_memory_usage(found_entry);
        if (new_mem > old_mem) {
            evict_if_needed(new_mem - old_mem);
            gd.stat_used_memory += (new_mem - old_mem);
        } else if (old_mem > new_mem) {
            if (gd.stat_used_memory >= (old_mem - new_mem)) gd.stat_used_memory -= (old_mem - new_mem);
            else gd.stat_used_memory = 0;
        }

        found_entry->last_accessed = get_monotonic_msec();
        if (found_entry->heap_idx != (size_t)-1){
            heap_delete(&gd.ttl_heap, found_entry->heap_idx);
            found_entry->heap_idx = (size_t)-1;
        }
        return rv;
    }

    size_t needed_mem = sizeof(struct Entry) + key_len + 1 + val_len + 64;
    if (evict_if_needed(needed_mem) != 0) {
        return -2;
    }

    struct Entry* entry = malloc(sizeof(struct Entry));
    if (entry == NULL){
        msg("set_Entry (oper.c): malloc");
        return 0;
    }
    if (init_Entry(entry) == 0){
        free(entry);
        return 0;
    }

    rv &= bufappend(entry->key, key, key_len + 1);
    rv &= bufappend(entry->val, val, val_len);
    entry->node.hcode = hcode_key;
    entry->last_accessed = get_monotonic_msec();
    gd.stat_used_memory += entry_memory_usage(entry);
    rv &= insert_HMap(hmap, &entry->node);
    return rv;
}

int del_Entry(char* key){
    struct HMap* hmap = &gd.kv_db;

    uint64_t hcode_key = str_hash(key, strlen(key));
    struct HNode* node = delete_HMap(hmap, key, hcode_key, entry_eq);

    if (node == NULL){
        return 0;
    }
    else {
        struct Entry* entry = container_of(node, struct Entry, node);
        size_t mem = entry_memory_usage(entry);
        if (gd.stat_used_memory >= mem) gd.stat_used_memory -= mem;
        else gd.stat_used_memory = 0;

        if (entry->heap_idx != (size_t)-1) heap_delete(&gd.ttl_heap, entry->heap_idx);
        free_Entry(entry);
        free(entry);
        return 1;
    }
}