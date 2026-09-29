#include "cmd_proc.h"
#include "persist.h"
#include <sys/resource.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>

struct CommandSpec command_list[] = {
    {"PING", 1, handle_ping},
    {"AUTH", 2, handle_auth},
    {"GET", 2, handle_get},
    {"SET", 5, handle_set},
    {"SET", 3, handle_set},
    {"DEL", 2, handle_del},
    {"EXISTS", 2, handle_exists},
    {"INCR", 2, handle_incr},
    {"DECR", 2, handle_decr},
    {"SAVE", 1, handle_save},
    {"INFO", 1, handle_info},
    {"STATS", 1, handle_info},
    {"KEYS", 2, handle_keys},
    {"SCAN", 2, handle_scan},
    {"SCAN", 3, handle_scan},
    {"SCAN", 4, handle_scan},
    {"SCAN", 6, handle_scan},
    {"TTL", 3, handle_set_ttl_sec},
    {"TTL", 2, handle_get_ttl_sec},
    {"TTLMS", 3, handle_set_ttl_ms},
    {"TTLMS", 2, handle_get_ttl_ms}
};

size_t command_list_len = 21;

char* get_string(struct buf* buffer, size_t* offset){
    uint32_t len;
    bufcpylenoffset(buffer, *offset, &len, HEADER_SIZE);

    char* str = malloc(sizeof(char) * (len + 1));
    if (str == NULL){
        die("get_string (cmd_proc.c): malloc");
        exit(EXIT_FAILURE);
    }
    *offset += HEADER_SIZE;
    bufcpylenoffset(buffer, *offset, str, len);
    str[len] = '\0';
    *offset += len;

    return str;
}

static int64_t parse_to_int(char* str){
    int is_neg = 0;
    if (*str == '-'){
        is_neg = 1;
        str++;
    }
    int64_t val = 0;
    while (*str){
        if (*str < '0' || *str > '9'){
            return -2; // -2 for incorrect type
        }

        val *= 10;
        val += *str - '0';
        str++;
    }

    if (is_neg == 1){
        val *= -1;
    }

    if (val < 0) return -1;
    else return val;
}

int handle_ping(struct Conn* conn, size_t offset){
    (void)offset;
    int rv = response_code(conn->wbuf, RES_OK);
    rv &= response_u32(conn->wbuf, 1);
    rv &= response_str(conn->wbuf, "PONG");
    return rv;
}

static int const_time_streq(const char* s1, const char* s2) {
    size_t len1 = strlen(s1);
    size_t len2 = strlen(s2);
    size_t max_len = len1 > len2 ? len1 : len2;
    unsigned char diff = (unsigned char)(len1 ^ len2);

    for (size_t i = 0; i < max_len; i++) {
        unsigned char c1 = (i < len1) ? (unsigned char)s1[i] : 0;
        unsigned char c2 = (i < len2) ? (unsigned char)s2[i] : 0;
        diff |= (c1 ^ c2);
    }
    return diff == 0;
}

int handle_auth(struct Conn* conn, size_t offset){
    char* pass = get_string(conn->rbuf, &offset);

    if (REQUIREPASS[0] == '\0'){
        free(pass);
        int rv = response_code(conn->wbuf, RES_ERR);
        rv &= response_u32(conn->wbuf, 1);
        rv &= response_str(conn->wbuf, "ERR Client sent AUTH, but no password is set");
        return rv;
    }

    uint64_t now = get_monotonic_msec();
    if (conn->lockout_until > now){
        free(pass);
        int rv = response_code(conn->wbuf, RES_ERR);
        rv &= response_u32(conn->wbuf, 1);
        rv &= response_str(conn->wbuf, "ERR Temporary lockout due to repeated failed authentication attempts");
        return rv;
    }

    if (const_time_streq(pass, REQUIREPASS)){
        free(pass);
        conn->authenticated = 1;
        conn->auth_failures = 0;
        conn->lockout_until = 0;
        gd.stat_auth_success++;

        int rv = response_code(conn->wbuf, RES_OK);
        rv &= response_u32(conn->wbuf, 1);
        rv &= response_str(conn->wbuf, "OK");
        return rv;
    }
    else {
        free(pass);
        conn->auth_failures++;
        gd.stat_auth_failures++;

        if ((int)conn->auth_failures >= MAX_AUTH_FAILURES){
            conn->lockout_until = now + (uint64_t)AUTH_LOCKOUT_MS;
            gd.stat_auth_lockouts++;
            msg("handle_auth: temporary connection lockout triggered after repeated failed attempts");
        }

        int rv = response_code(conn->wbuf, RES_ERR);
        rv &= response_u32(conn->wbuf, 1);
        rv &= response_str(conn->wbuf, "ERR invalid password");
        return rv;
    }
}

int handle_get(struct Conn* conn, size_t offset){
    char* key = get_string(conn->rbuf, &offset);

    struct buf* val = get_Entry(key);
    free(key);
    
    if (val == NULL){
        int rv = response_code(conn->wbuf, RES_NX);
        rv &= response_u32(conn->wbuf, 0);
        return rv;
    }

    int rv = response_code(conn->wbuf, RES_OK);
    rv &= response_u32(conn->wbuf, 1);
    rv &= response_str_len(conn->wbuf, bufstart(val), bufsize(val));
    return rv;
}

int handle_set(struct Conn* conn, size_t offset){
    uint32_t nstr = 0;
    bufcpylen(conn->rbuf, &nstr, HEADER_SIZE);

    char* key = get_string(conn->rbuf, &offset);
    char* val = get_string(conn->rbuf, &offset);
    int res = set_Entry(key, val);

    if (res == -2) {
        free(key);
        free(val);
        int rv = response_code(conn->wbuf, RES_ERR);
        rv &= response_u32(conn->wbuf, 1);
        rv &= response_str(conn->wbuf, "OOM command not allowed when used memory > 'maxmemory'");
        return rv;
    }

    if (res == 0) {
        free(key);
        free(val);
        int rv = response_code(conn->wbuf, RES_ERR);
        rv &= response_u32(conn->wbuf, 0);
        return rv;
    }

    if (nstr == 5) {
        char* opt = get_string(conn->rbuf, &offset);
        char* ttl_str = get_string(conn->rbuf, &offset);
        long long ttl = atoll(ttl_str);
        if (strcasecmp(opt, "EX") == 0) {
            if (ttl > 0) set_ttl_Entry_sec(key, ttl);
        } else if (strcasecmp(opt, "PX") == 0) {
            if (ttl > 0) set_ttl_Entry_ms(key, ttl);
        }
        free(opt);
        free(ttl_str);
    }

    free(key);
    free(val);

    int rv = response_code(conn->wbuf, RES_OK);
    rv &= response_u32(conn->wbuf, 0);
    return rv;
}

int handle_del(struct Conn* conn, size_t offset){
    char* key = get_string(conn->rbuf, &offset);
    int res = del_Entry(key);
    free(key);

    int rv = 1;
    if (res == 0){
        rv = response_code(conn->wbuf, RES_NX);
    }
    else {
        rv = response_code(conn->wbuf, RES_OK);
    }
    rv &= response_u32(conn->wbuf, 0);
    return rv;
}

int handle_exists(struct Conn* conn, size_t offset){
    char* key = get_string(conn->rbuf, &offset);
    int exists = exists_Entry(key);
    free(key);

    int rv = response_code(conn->wbuf, RES_OK);
    rv &= response_u32(conn->wbuf, 1);
    rv &= response_int(conn->wbuf, exists);
    return rv;
}

int handle_incr(struct Conn* conn, size_t offset){
    char* key = get_string(conn->rbuf, &offset);
    int64_t val = 0;
    int res = incr_decr_Entry(key, 1, &val);
    free(key);

    int rv = 1;
    if (res == 0){
        rv = response_code(conn->wbuf, RES_OK);
        rv &= response_u32(conn->wbuf, 1);
        rv &= response_int64(conn->wbuf, val);
    }
    else if (res == -1){
        rv = response_code(conn->wbuf, RES_TY);
        rv &= response_u32(conn->wbuf, 0);
    }
    else {
        rv = response_code(conn->wbuf, RES_ERR);
        rv &= response_u32(conn->wbuf, 0);
    }
    return rv;
}

int handle_decr(struct Conn* conn, size_t offset){
    char* key = get_string(conn->rbuf, &offset);
    int64_t val = 0;
    int res = incr_decr_Entry(key, -1, &val);
    free(key);

    int rv = 1;
    if (res == 0){
        rv = response_code(conn->wbuf, RES_OK);
        rv &= response_u32(conn->wbuf, 1);
        rv &= response_int64(conn->wbuf, val);
    }
    else if (res == -1){
        rv = response_code(conn->wbuf, RES_TY);
        rv &= response_u32(conn->wbuf, 0);
    }
    else {
        rv = response_code(conn->wbuf, RES_ERR);
        rv &= response_u32(conn->wbuf, 0);
    }
    return rv;
}

int handle_save(struct Conn* conn, size_t offset){
    (void)offset;
    int res = save_snapshot(SNAPSHOT_PATH);
    if (res >= 0){
        int rv = response_code(conn->wbuf, RES_OK);
        rv &= response_u32(conn->wbuf, 1);
        rv &= response_str(conn->wbuf, "OK");
        return rv;
    }
    else {
        int rv = response_code(conn->wbuf, RES_ERR);
        rv &= response_u32(conn->wbuf, 1);
        rv &= response_str(conn->wbuf, "ERR snapshot save failed");
        return rv;
    }
}

int handle_info(struct Conn* conn, size_t offset){
    (void)offset;

    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    long rss_kb = usage.ru_maxrss;
    double rss_mb = (double)rss_kb / 1024.0;

    uint64_t uptime_sec = (get_monotonic_msec() - gd.start_time_ms) / 1000;
    uint64_t uptime_days = uptime_sec / 86400;

    size_t keys_count = count_keys_Entry();
    size_t expiring_keys = gd.ttl_heap.sz;

    uint64_t total_lookups = gd.stat_hits + gd.stat_misses;
    double hit_rate = (total_lookups > 0) ? ((double)gd.stat_hits * 100.0 / (double)total_lookups) : 0.0;

    char info_buf[2048];
    int len = snprintf(info_buf, sizeof(info_buf),
        "# Server\r\n"
        "ramora_version:0.2.0\r\n"
        "process_id:%d\r\n"
        "uptime_in_seconds:%" PRIu64 "\r\n"
        "uptime_in_days:%" PRIu64 "\r\n"
        "\r\n"
        "# Clients\r\n"
        "connected_clients:%u\r\n"
        "pooled_connections:%d\r\n"
        "max_pooled_connections:%d\r\n"
        "\r\n"
        "# Memory\r\n"
        "used_memory:%zu\r\n"
        "used_memory_human:%.2fM\r\n"
        "used_memory_rss_bytes:%ld\r\n"
        "used_memory_rss_human:%.2fM\r\n"
        "maxmemory:%zu\r\n"
        "maxmemory_human:%.2fM\r\n"
        "maxmemory_policy:%s\r\n"
        "evicted_keys:%" PRIu64 "\r\n"
        "\r\n"
        "# Stats\r\n"
        "total_commands_processed:%" PRIu64 "\r\n"
        "keyspace_keys:%zu\r\n"
        "keyspace_expiring_keys:%zu\r\n"
        "keyspace_hits:%" PRIu64 "\r\n"
        "keyspace_misses:%" PRIu64 "\r\n"
        "keyspace_hit_rate:%.2f%%\r\n"
        "\r\n"
        "# Security\r\n"
        "auth_enabled:%d\r\n"
        "auth_successes:%" PRIu64 "\r\n"
        "auth_failures:%" PRIu64 "\r\n"
        "auth_lockouts:%" PRIu64 "\r\n"
        "max_auth_failures_limit:%d\r\n"
        "auth_lockout_duration_ms:%d\r\n",
        getpid(),
        uptime_sec,
        uptime_days,
        gd.active_clients,
        gd.pooled_conn_count,
        MAX_POOLED_CONN_COUNT,
        gd.stat_used_memory,
        (double)gd.stat_used_memory / 1048576.0,
        rss_kb * 1024L,
        rss_mb,
        MAXMEMORY,
        (double)MAXMEMORY / 1048576.0,
        MAXMEMORY_POLICY,
        gd.stat_evicted_keys,
        gd.stat_commands_processed,
        keys_count,
        expiring_keys,
        gd.stat_hits,
        gd.stat_misses,
        hit_rate,
        (REQUIREPASS[0] != '\0' ? 1 : 0),
        gd.stat_auth_success,
        gd.stat_auth_failures,
        gd.stat_auth_lockouts,
        MAX_AUTH_FAILURES,
        AUTH_LOCKOUT_MS
    );

    int rv = response_code(conn->wbuf, RES_OK);
    rv &= response_u32(conn->wbuf, 1);
    rv &= response_str_len(conn->wbuf, info_buf, (size_t)len);
    return rv;
}

int handle_keys(struct Conn* conn, size_t offset){
    char* pattern = get_string(conn->rbuf, &offset);
    int rv = get_keys_Entry(pattern, conn->wbuf);
    free(pattern);
    return rv;
}

int handle_scan(struct Conn* conn, size_t offset){
    uint32_t nstr = 0;
    bufcpylen(conn->rbuf, &nstr, HEADER_SIZE);

    char* cursor_str = get_string(conn->rbuf, &offset);
    char* endptr = NULL;
    errno = 0;
    unsigned long long cursor = strtoull(cursor_str, &endptr, 10);
    if (endptr == cursor_str || *endptr != '\0') {
        free(cursor_str);
        int rv = response_code(conn->wbuf, RES_TY);
        rv &= response_u32(conn->wbuf, 0);
        return rv;
    }
    free(cursor_str);

    const char* pattern = "*";
    char* pattern_alloc = NULL;
    size_t count = 10;

    for (uint32_t i = 2; i < nstr; i++) {
        char* arg = get_string(conn->rbuf, &offset);
        if (strcasecmp(arg, "MATCH") == 0 && i + 1 < nstr) {
            free(arg);
            i++;
            pattern_alloc = get_string(conn->rbuf, &offset);
            pattern = pattern_alloc;
        } else if (strcasecmp(arg, "COUNT") == 0 && i + 1 < nstr) {
            free(arg);
            i++;
            char* cnt_str = get_string(conn->rbuf, &offset);
            long long c = atoll(cnt_str);
            if (c > 0) count = (size_t)c;
            free(cnt_str);
        } else if (i == 2 && pattern_alloc == NULL && strcasecmp(arg, "MATCH") != 0 && strcasecmp(arg, "COUNT") != 0) {
            pattern_alloc = arg;
            pattern = pattern_alloc;
        } else if (i == 3 && strcasecmp(arg, "COUNT") != 0) {
            long long c = atoll(arg);
            if (c > 0) count = (size_t)c;
            free(arg);
        } else {
            free(arg);
        }
    }

    int rv = scan_Entry((uint64_t)cursor, pattern, count, conn->wbuf);
    if (pattern_alloc) {
        free(pattern_alloc);
    }
    return rv;
}

int handle_get_ttl_sec(struct Conn* conn, size_t offset){
    char* key = get_string(conn->rbuf, &offset);
    int64_t ttl = get_ttl_Entry_sec(key);
    free(key);

    int rv = 1;
    if (ttl == -2){
        rv = response_code(conn->wbuf, RES_NX);
        rv &= response_u32(conn->wbuf, 0);
    }
    else {
        rv = response_code(conn->wbuf, RES_OK);
        rv &= response_u32(conn->wbuf, 1);
        rv &= response_int64(conn->wbuf, ttl);
    }
    return rv;
}

int handle_get_ttl_ms(struct Conn* conn, size_t offset){
    char* key = get_string(conn->rbuf, &offset);
    int64_t ttl = get_ttl_Entry_ms(key);
    free(key);

    int rv = 1;
    if (ttl == -2){
        rv = response_code(conn->wbuf, RES_NX);
        rv &= response_u32(conn->wbuf, 0);
    }
    else {
        rv = response_code(conn->wbuf, RES_OK);
        rv &= response_u32(conn->wbuf, 1);
        rv &= response_int64(conn->wbuf, ttl);
    }
    return rv;
}

int handle_set_ttl_sec(struct Conn* conn, size_t offset){
    char* key = get_string(conn->rbuf, &offset);
    char* ttl_string = get_string(conn->rbuf, &offset);
    int64_t ttl = parse_to_int(ttl_string);

    if (ttl == -2){
        int rv = 1;
        rv = response_code(conn->wbuf, RES_TY);
        rv &= response_u32(conn->wbuf, 0);
        free(key);
        free(ttl_string);
        return rv;
    }

    int res = set_ttl_Entry_sec(key, ttl);
    free(key);
    free(ttl_string);

    int rv = 1;
    if (res == -1){
        rv = response_code(conn->wbuf, RES_ERR);
        rv &= response_u32(conn->wbuf, 0);
    }
    else if (res == 0){
        rv = response_code(conn->wbuf, RES_NX);
        rv &= response_u32(conn->wbuf, 0);
    }
    else {
        rv = response_code(conn->wbuf, RES_OK);
        rv &= response_u32(conn->wbuf, 0);
    }
    return rv;
}

int handle_set_ttl_ms(struct Conn* conn, size_t offset){
    char* key = get_string(conn->rbuf, &offset);
    char* ttl_string = get_string(conn->rbuf, &offset);
    int64_t ttl = parse_to_int(ttl_string);

    if (ttl == -2){
        int rv = 1;
        rv = response_code(conn->wbuf, RES_TY);
        rv &= response_u32(conn->wbuf, 0);
        free(key);
        free(ttl_string);
        return rv;
    }

    int res = set_ttl_Entry_ms(key, ttl);
    free(key);
    free(ttl_string);

    int rv = 1;
    if (res == -1){
        rv = response_code(conn->wbuf, RES_ERR);
        rv &= response_u32(conn->wbuf, 0);
    }
    else if (res == 0){
        rv = response_code(conn->wbuf, RES_NX);
        rv &= response_u32(conn->wbuf, 0);
    }
    else {
        rv = response_code(conn->wbuf, RES_OK);
        rv &= response_u32(conn->wbuf, 0);
    }
    return rv;
}