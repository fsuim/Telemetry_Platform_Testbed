#ifndef DB_SQLITE_H
#define DB_SQLITE_H

#include <stddef.h>
#include <sqlite3.h>

#include "telemetry_types.h"

typedef struct {
    sqlite3 *db;
    sqlite3_stmt *ins_state;
} db_writer_t;

typedef struct {
    sqlite3 *db;
} db_reader_t;

int db_writer_open(db_writer_t *w, const char *path);
void db_writer_close(db_writer_t *w);

int db_writer_begin(db_writer_t *w);
int db_writer_commit(db_writer_t *w);
int db_writer_insert_state(db_writer_t *w, const telemetry_state_t *s);

int db_reader_open(db_reader_t *r, const char *path);
void db_reader_close(db_reader_t *r);

// {"type":"history","items":[...]}
int db_reader_history_last_json(db_reader_t *r, int limit, char **out_json, size_t *out_len);

#endif