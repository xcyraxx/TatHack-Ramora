#include "persist.h"
#include "g_data.h"
#include "oper.h"
#include "config.h"
#include "logging_helper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define container_of(ptr, T, member) \
    ((T *)( (char *)ptr - offsetof(T, member) ))

static inline void update_checksum(uint64_t* crc, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        *crc ^= p[i];
        *crc *= 1099511628211ULL;
    }
}

int save_snapshot(const char* filepath) {
    char tmp_path[1100];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", filepath);

    FILE* fp = fopen(tmp_path, "wb");
    if (!fp) {
        msg("save_snapshot: failed to open tmp file for writing");
        return -1;
    }

    uint64_t now_ms = get_monotonic_msec();
    uint64_t checksum = 1469598103934665603ULL;

    uint32_t count = 0;
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

    char magic[4] = {'R', 'M', 'R', 'A'};
    uint32_t version = SNAPSHOT_VERSION;
    uint32_t count_u32 = count;

    if (fwrite(magic, 1, 4, fp) != 4 ||
        fwrite(&version, 4, 1, fp) != 1 ||
        fwrite(&count_u32, 4, 1, fp) != 1) {
        fclose(fp);
        unlink(tmp_path);
        return -1;
    }
    update_checksum(&checksum, magic, 4);
    update_checksum(&checksum, &version, 4);
    update_checksum(&checksum, &count_u32, 4);

    #define WRITE_ENTRY(entry) do { \
        if ((entry)->heap_idx != (size_t)-1 && gd.ttl_heap.arr[(entry)->heap_idx].expiration_time <= now_ms) { \
            break; \
        } \
        int64_t rem_ttl = -1; \
        if ((entry)->heap_idx != (size_t)-1) { \
            rem_ttl = (int64_t)(gd.ttl_heap.arr[(entry)->heap_idx].expiration_time - now_ms); \
            if (rem_ttl <= 0) break; \
        } \
        uint32_t klen = (uint32_t)strlen(bufstart((entry)->key)); \
        uint32_t vlen = (uint32_t)bufsize((entry)->val); \
        if (fwrite(&klen, 4, 1, fp) != 1 || \
            fwrite(bufstart((entry)->key), 1, klen, fp) != klen || \
            fwrite(&vlen, 4, 1, fp) != 1 || \
            fwrite(bufstart((entry)->val), 1, vlen, fp) != vlen || \
            fwrite(&rem_ttl, 8, 1, fp) != 1) { \
            fclose(fp); \
            unlink(tmp_path); \
            return -1; \
        } \
        update_checksum(&checksum, &klen, 4); \
        update_checksum(&checksum, bufstart((entry)->key), klen); \
        update_checksum(&checksum, &vlen, 4); \
        update_checksum(&checksum, bufstart((entry)->val), vlen); \
        update_checksum(&checksum, &rem_ttl, 8); \
    } while(0)

    if (gd.kv_db.new_tab && gd.kv_db.new_tab->tab) {
        for (size_t i = 0; i < gd.kv_db.new_tab->cap; i++) {
            for (struct HNode* curr = gd.kv_db.new_tab->tab[i]; curr != NULL; curr = curr->next) {
                struct Entry* entry = container_of(curr, struct Entry, node);
                WRITE_ENTRY(entry);
            }
        }
    }
    if (gd.kv_db.is_migrating && gd.kv_db.old_tab && gd.kv_db.old_tab->tab) {
        for (size_t i = gd.kv_db.migrating_pos; i < gd.kv_db.old_tab->cap; i++) {
            for (struct HNode* curr = gd.kv_db.old_tab->tab[i]; curr != NULL; curr = curr->next) {
                struct Entry* entry = container_of(curr, struct Entry, node);
                WRITE_ENTRY(entry);
            }
        }
    }
    #undef WRITE_ENTRY

    char footer[4] = {'E', 'N', 'D', 'R'};
    if (fwrite(footer, 1, 4, fp) != 4 ||
        fwrite(&checksum, 8, 1, fp) != 1) {
        fclose(fp);
        unlink(tmp_path);
        return -1;
    }

    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    if (rename(tmp_path, filepath) != 0) {
        msg("save_snapshot: rename failed");
        unlink(tmp_path);
        return -1;
    }

    return (int)count;
}

int load_snapshot(const char* filepath) {
    FILE* fp = fopen(filepath, "rb");
    if (!fp) {
        return 0;
    }

    uint64_t checksum = 1469598103934665603ULL;
    char magic[4];
    uint32_t version = 0;
    uint32_t count = 0;

    if (fread(magic, 1, 4, fp) != 4 ||
        fread(&version, 4, 1, fp) != 1 ||
        fread(&count, 4, 1, fp) != 1) {
        msg("load_snapshot: truncated header");
        fclose(fp);
        return -1;
    }

    if (memcmp(magic, SNAPSHOT_MAGIC, 4) != 0) {
        msg("load_snapshot: invalid magic header");
        fclose(fp);
        return -1;
    }

    if (version != SNAPSHOT_VERSION) {
        msg("load_snapshot: unsupported snapshot version");
        fclose(fp);
        return -1;
    }

    update_checksum(&checksum, magic, 4);
    update_checksum(&checksum, &version, 4);
    update_checksum(&checksum, &count, 4);

    uint32_t loaded = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t klen = 0;
        if (fread(&klen, 4, 1, fp) != 1 || klen == 0 || klen > (uint32_t)MAX_PAYLOAD_SIZE) {
            msg("load_snapshot: invalid key length");
            fclose(fp);
            return -1;
        }

        char* key = malloc(klen + 1);
        if (!key) {
            fclose(fp);
            return -1;
        }
        if (fread(key, 1, klen, fp) != klen) {
            free(key);
            msg("load_snapshot: truncated key data");
            fclose(fp);
            return -1;
        }
        key[klen] = '\0';

        uint32_t vlen = 0;
        if (fread(&vlen, 4, 1, fp) != 1 || vlen > (uint32_t)MAX_PAYLOAD_SIZE) {
            free(key);
            msg("load_snapshot: invalid value length");
            fclose(fp);
            return -1;
        }

        char* val = malloc(vlen + 1);
        if (!val) {
            free(key);
            fclose(fp);
            return -1;
        }
        if (fread(val, 1, vlen, fp) != vlen) {
            free(key);
            free(val);
            msg("load_snapshot: truncated value data");
            fclose(fp);
            return -1;
        }
        val[vlen] = '\0';

        int64_t rem_ttl = -1;
        if (fread(&rem_ttl, 8, 1, fp) != 1) {
            free(key);
            free(val);
            msg("load_snapshot: truncated ttl data");
            fclose(fp);
            return -1;
        }

        update_checksum(&checksum, &klen, 4);
        update_checksum(&checksum, key, klen);
        update_checksum(&checksum, &vlen, 4);
        update_checksum(&checksum, val, vlen);
        update_checksum(&checksum, &rem_ttl, 8);

        set_Entry(key, val);
        if (rem_ttl > 0) {
            set_ttl_Entry_ms(key, rem_ttl);
        }

        free(key);
        free(val);
        loaded++;
    }

    char footer[4];
    uint64_t expected_checksum = 0;
    if (fread(footer, 1, 4, fp) != 4 ||
        fread(&expected_checksum, 8, 1, fp) != 1) {
        msg("load_snapshot: truncated footer/checksum");
        fclose(fp);
        return -1;
    }

    if (memcmp(footer, SNAPSHOT_FOOTER, 4) != 0) {
        msg("load_snapshot: invalid footer magic");
        fclose(fp);
        return -1;
    }

    if (checksum != expected_checksum) {
        msg("load_snapshot: checksum mismatch! Snapshot file is corrupted");
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return (int)loaded;
}
