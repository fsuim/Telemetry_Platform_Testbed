#ifndef WS_SERVER_H
#define WS_SERVER_H

#include <stddef.h>
#include "db_sqlite.h"

typedef struct ws_server ws_server_t;

int ws_server_start(ws_server_t **out, int port, db_reader_t *db_reader);
void ws_server_stop(ws_server_t *s);

int ws_server_broadcast(ws_server_t *s, const char *msg, size_t len);

#endif