#ifndef PERSIST_H
#define PERSIST_H

#include <stdint.h>
#include <stddef.h>

#define SNAPSHOT_MAGIC "RMRA"
#define SNAPSHOT_FOOTER "ENDR"
#define SNAPSHOT_VERSION 1

int save_snapshot(const char* filepath);
int load_snapshot(const char* filepath);

#endif
