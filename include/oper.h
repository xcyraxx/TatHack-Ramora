#ifndef OPER_H
#define OPER_H

#include <stddef.h>

#include "response.h"
#include "g_data.h"
#include "timer.h"

#include <stdint.h>

extern struct g_data gd;

struct buf* get_Entry(char* key);
int exists_Entry(char* key);
int set_Entry(char* key, char* val);
int del_Entry(char* key);
int incr_decr_Entry(char* key, int64_t delta, int64_t* out_val);
int set_ttl_Entry_ms(char* key, int64_t ttl);
int set_ttl_Entry_sec(char* key, int64_t ttl);
int64_t get_ttl_Entry_ms(char* key);
int64_t get_ttl_Entry_sec(char* key);
size_t count_keys_Entry(void);
int get_keys_Entry(const char* pattern, struct buf* wbuf);
int scan_Entry(uint64_t cursor, const char* pattern, size_t count, struct buf* wbuf);
int evict_if_needed(size_t needed_bytes);

#endif