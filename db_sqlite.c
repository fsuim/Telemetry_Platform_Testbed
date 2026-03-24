#include "db_sqlite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} strbuf_t;

static int sb_reserve(strbuf_t *sb, size_t extra){
    if(sb->len + extra + 1 <= sb->cap) return 0;
    size_t ncap = sb->cap ? sb->cap : 1024;
    while(ncap < sb->len + extra + 1) ncap *= 2;
    char *nb = (char*)realloc(sb->buf, ncap);
    if(!nb) return -1;
    sb->buf = nb;
    sb->cap = ncap;
    return 0;
}

static int sb_append(strbuf_t *sb, const char *s){
    size_t n = strlen(s);
    if(sb_reserve(sb, n) != 0) return -1;
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
    return 0;
}

static int sb_appendf(strbuf_t *sb, const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    char tmp[512];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if(n < 0) return -1;

    if((size_t)n < sizeof(tmp)){
        if(sb_reserve(sb, (size_t)n) != 0) return -1;
        memcpy(sb->buf + sb->len, tmp, (size_t)n);
        sb->len += (size_t)n;
        sb->buf[sb->len] = '\0';
        return 0;
    }

    char *big = (char*)malloc((size_t)n + 1);
    if(!big) return -1;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int rc = sb_append(sb, big);
    free(big);
    return rc;
}

static void sb_free(strbuf_t *sb){
    free(sb->buf);
    sb->buf = NULL;
    sb->len = sb->cap = 0;
}

static int exec_sql(sqlite3 *db, const char *sql){
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if(rc != SQLITE_OK){
        fprintf(stderr, "[DB] sqlite3_exec failed: %s\nSQL: %s\n", err ? err : "(null)", sql);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static int column_exists(sqlite3 *db, const char *table, const char *col){
    if(!db || !table || !col) return 0;
    char sql[256];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table);

    sqlite3_stmt *st = NULL;
    if(sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK){
        return 0;
    }

    int exists = 0;
    while(sqlite3_step(st) == SQLITE_ROW){
        const unsigned char *name = sqlite3_column_text(st, 1); // column name
        if(name && strcmp((const char*)name, col) == 0){
            exists = 1;
            break;
        }
    }
    sqlite3_finalize(st);
    return exists;
}

static int ensure_column(sqlite3 *db, const char *table, const char *col_def){
    // col_def example: "motor1_id TEXT"
    // Adds column only if it does not exist.
    if(!db || !table || !col_def) return -1;

    char col[128];
    size_t n = 0;
    while(col_def[n] && col_def[n] != ' ' && n + 1 < sizeof(col)){
        col[n] = col_def[n];
        n++;
    }
    col[n] = '\0';

    if(column_exists(db, table, col)) return 0;

    char sql[512];
    snprintf(sql, sizeof(sql), "ALTER TABLE %s ADD COLUMN %s;", table, col_def);
    return exec_sql(db, sql);
}

static int db_apply_pragmas(sqlite3 *db){
    if(exec_sql(db, "PRAGMA journal_mode=WAL;") != 0) return -1;
    if(exec_sql(db, "PRAGMA synchronous=NORMAL;") != 0) return -1;
    if(exec_sql(db, "PRAGMA foreign_keys=ON;") != 0) return -1;
    return 0;
}

static int db_migrate_v2(sqlite3 *db){
    if(db_apply_pragmas(db) != 0) return -1;

    const char *schema =
        "CREATE TABLE IF NOT EXISTS telemetry_state (\n"
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "  received_at_ns INTEGER NOT NULL,\n"
        "  topic TEXT NOT NULL,\n"
        "  robot_id TEXT,\n"
        "  seq INTEGER,\n"
        "  msg_timestamp_ns INTEGER,\n"
        "  imu_id TEXT,\n"
        "  acc_x REAL, acc_y REAL, acc_z REAL,\n"
        "  gyro_x REAL, gyro_y REAL, gyro_z REAL,\n"
        "  tilt_id TEXT,\n"
        "  tilt_x REAL, tilt_y REAL, tilt_z REAL,\n"
        "  tilt_status INTEGER,\n"
        "  motor1_id TEXT,\n"
        "  motor1_tics INTEGER,\n"
        "  motor1_rpm INTEGER,\n"
        "  motor1_temperature_c INTEGER,\n"
        "  motor1_voltage_power_stage_mv INTEGER,\n"
        "  motor1_current_power_stage_ma INTEGER,\n"
        "  motor2_id TEXT,\n"
        "  motor2_tics INTEGER,\n"
        "  motor2_rpm INTEGER,\n"
        "  motor2_temperature_c INTEGER,\n"
        "  motor2_voltage_power_stage_mv INTEGER,\n"
        "  motor2_current_power_stage_ma INTEGER,\n"
        "  raw BLOB\n"
        ");\n"
        "CREATE INDEX IF NOT EXISTS idx_state_received ON telemetry_state(received_at_ns);\n"
        "CREATE INDEX IF NOT EXISTS idx_state_robot_received ON telemetry_state(robot_id, received_at_ns);\n";

    if(exec_sql(db, schema) != 0) return -1;

    // In case an old unversioned DB already exists with a partial schema, ensure columns.
    const char *t = "telemetry_state";
    const char *cols[] = {
        "motor1_id TEXT",
        "motor1_tics INTEGER",
        "motor1_rpm INTEGER",
        "motor1_temperature_c INTEGER",
        "motor1_voltage_power_stage_mv INTEGER",
        "motor1_current_power_stage_ma INTEGER",
        "motor2_id TEXT",
        "motor2_tics INTEGER",
        "motor2_rpm INTEGER",
        "motor2_temperature_c INTEGER",
        "motor2_voltage_power_stage_mv INTEGER",
        "motor2_current_power_stage_ma INTEGER",
    };
    for(size_t i=0; i<sizeof(cols)/sizeof(cols[0]); i++){
        if(ensure_column(db, t, cols[i]) != 0) return -1;
    }

    if(exec_sql(db, "PRAGMA user_version=2;") != 0) return -1;
    return 0;
}

static int db_upgrade_v1_to_v2(sqlite3 *db){
    if(db_apply_pragmas(db) != 0) return -1;

    const char *t = "telemetry_state";
    const char *cols[] = {
        "motor1_id TEXT",
        "motor1_tics INTEGER",
        "motor1_rpm INTEGER",
        "motor1_temperature_c INTEGER",
        "motor1_voltage_power_stage_mv INTEGER",
        "motor1_current_power_stage_ma INTEGER",
        "motor2_id TEXT",
        "motor2_tics INTEGER",
        "motor2_rpm INTEGER",
        "motor2_temperature_c INTEGER",
        "motor2_voltage_power_stage_mv INTEGER",
        "motor2_current_power_stage_ma INTEGER",
    };

    for(size_t i=0; i<sizeof(cols)/sizeof(cols[0]); i++){
        if(ensure_column(db, t, cols[i]) != 0) return -1;
    }

    if(exec_sql(db, "PRAGMA user_version=2;") != 0) return -1;
    return 0;
}


static int db_ensure_schema(sqlite3 *db){
    int user_version = 0;
    sqlite3_stmt *st = NULL;
    if(sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &st, NULL) != SQLITE_OK){
        fprintf(stderr, "[DB] prepare user_version failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    if(sqlite3_step(st) == SQLITE_ROW){
        user_version = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);

    if(user_version == 0) return db_migrate_v2(db);
    if(user_version == 1) return db_upgrade_v1_to_v2(db);
    if(user_version == 2) return 0;

    fprintf(stderr, "[DB] Unsupported user_version=%d\n", user_version);
    return -1;
}

int db_writer_open(db_writer_t *w, const char *path){
    memset(w, 0, sizeof(*w));

    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if(sqlite3_open_v2(path, &w->db, flags, NULL) != SQLITE_OK){
        fprintf(stderr, "[DB] open writer failed: %s\n", sqlite3_errmsg(w->db));
        return -1;
    }
    if(db_ensure_schema(w->db) != 0) return -2;

    const char *ins =
        "INSERT INTO telemetry_state("
        "received_at_ns, topic, robot_id, seq, msg_timestamp_ns,"
        "imu_id, acc_x, acc_y, acc_z, gyro_x, gyro_y, gyro_z,"
        "tilt_id, tilt_x, tilt_y, tilt_z, tilt_status,"
        "motor1_id, motor1_tics, motor1_rpm, motor1_temperature_c, motor1_voltage_power_stage_mv, motor1_current_power_stage_ma,"
        "motor2_id, motor2_tics, motor2_rpm, motor2_temperature_c, motor2_voltage_power_stage_mv, motor2_current_power_stage_ma,"
        "raw"
        ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";

    if(sqlite3_prepare_v2(w->db, ins, -1, &w->ins_state, NULL) != SQLITE_OK){
        fprintf(stderr, "[DB] prepare insert failed: %s\n", sqlite3_errmsg(w->db));
        return -3;
    }

    return 0;
}

void db_writer_close(db_writer_t *w){
    if(!w) return;
    if(w->ins_state) sqlite3_finalize(w->ins_state);
    w->ins_state = NULL;
    if(w->db) sqlite3_close(w->db);
    w->db = NULL;
}

int db_writer_begin(db_writer_t *w){ return exec_sql(w->db, "BEGIN IMMEDIATE;"); }
int db_writer_commit(db_writer_t *w){ return exec_sql(w->db, "COMMIT;"); }

int db_writer_insert_state(db_writer_t *w, const telemetry_state_t *s){
    if(!w || !w->db || !w->ins_state || !s) return -1;

    sqlite3_stmt *st = w->ins_state;
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);

    int i = 1;
    sqlite3_bind_int64(st, i++, (sqlite3_int64)s->received_at_ns);
    sqlite3_bind_text(st, i++, s->topic ? s->topic : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, i++, s->robot_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, i++, (int)s->seq);
    sqlite3_bind_int64(st, i++, (sqlite3_int64)s->msg_timestamp_ns);

    sqlite3_bind_text(st, i++, s->imu_id, -1, SQLITE_STATIC);
    sqlite3_bind_double(st, i++, s->acc_g[0]);
    sqlite3_bind_double(st, i++, s->acc_g[1]);
    sqlite3_bind_double(st, i++, s->acc_g[2]);
    sqlite3_bind_double(st, i++, s->gyro_dps[0]);
    sqlite3_bind_double(st, i++, s->gyro_dps[1]);
    sqlite3_bind_double(st, i++, s->gyro_dps[2]);

    sqlite3_bind_text(st, i++, s->tilt_id, -1, SQLITE_STATIC);
    sqlite3_bind_double(st, i++, s->tilt_deg[0]);
    sqlite3_bind_double(st, i++, s->tilt_deg[1]);
    sqlite3_bind_double(st, i++, s->tilt_deg[2]);
    sqlite3_bind_int(st, i++, (int)s->tilt_status);

    // Motors (persist for replay/analysis)
    sqlite3_bind_text(st, i++, s->motor_id[0], -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, i++, (sqlite3_int64)s->motor_tics[0]);
    sqlite3_bind_int(st, i++, (int)s->motor_rpm[0]);
    sqlite3_bind_int(st, i++, (int)s->motor_temperature_c[0]);
    sqlite3_bind_int(st, i++, (int)s->motor_voltage_power_stage_mv[0]);
    sqlite3_bind_int(st, i++, (int)s->motor_current_power_stage_ma[0]);

    sqlite3_bind_text(st, i++, s->motor_id[1], -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, i++, (sqlite3_int64)s->motor_tics[1]);
    sqlite3_bind_int(st, i++, (int)s->motor_rpm[1]);
    sqlite3_bind_int(st, i++, (int)s->motor_temperature_c[1]);
    sqlite3_bind_int(st, i++, (int)s->motor_voltage_power_stage_mv[1]);
    sqlite3_bind_int(st, i++, (int)s->motor_current_power_stage_ma[1]);

    if(s->raw && s->raw_len > 0){
        sqlite3_bind_blob(st, i++, s->raw, (int)s->raw_len, SQLITE_STATIC);
    }else{
        sqlite3_bind_null(st, i++);
    }

    int rc = sqlite3_step(st);
    if(rc != SQLITE_DONE){
        fprintf(stderr, "[DB] insert failed: %s\n", sqlite3_errmsg(w->db));
        return -2;
    }

    return 0;
}

int db_reader_open(db_reader_t *r, const char *path){
    memset(r, 0, sizeof(*r));

    int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX;
    if(sqlite3_open_v2(path, &r->db, flags, NULL) != SQLITE_OK){
        fprintf(stderr, "[DB] open reader failed: %s\n", sqlite3_errmsg(r->db));
        return -1;
    }

    if(exec_sql(r->db, "PRAGMA journal_mode=WAL;") != 0) return -2;
    return 0;
}

void db_reader_close(db_reader_t *r){
    if(!r) return;
    if(r->db) sqlite3_close(r->db);
    r->db = NULL;
}

static int append_row_as_sample_json(strbuf_t *sb, sqlite3_stmt *st){
    int64_t received_at_ns = sqlite3_column_int64(st, 0);
    const unsigned char *robot_id = sqlite3_column_text(st, 1);
    int seq = sqlite3_column_int(st, 2);
    int64_t msg_ts = sqlite3_column_int64(st, 3);

    const unsigned char *imu_id = sqlite3_column_text(st, 4);
    double acc_x = sqlite3_column_double(st, 5);
    double acc_y = sqlite3_column_double(st, 6);
    double acc_z = sqlite3_column_double(st, 7);
    double gyr_x = sqlite3_column_double(st, 8);
    double gyr_y = sqlite3_column_double(st, 9);
    double gyr_z = sqlite3_column_double(st, 10);

    const unsigned char *tilt_id = sqlite3_column_text(st, 11);
    double tilt_x = sqlite3_column_double(st, 12);
    double tilt_y = sqlite3_column_double(st, 13);
    double tilt_z = sqlite3_column_double(st, 14);
    int tilt_status = sqlite3_column_int(st, 15);

    // Optional motors (may be NULL for older rows)
    const unsigned char *m1_id = sqlite3_column_text(st, 16);
    int64_t m1_tics = sqlite3_column_int64(st, 17);
    int m1_rpm = sqlite3_column_int(st, 18);
    int m1_temp = sqlite3_column_int(st, 19);
    int m1_vps = sqlite3_column_int(st, 20);
    int m1_cps = sqlite3_column_int(st, 21);

    const unsigned char *m2_id = sqlite3_column_text(st, 22);
    int64_t m2_tics = sqlite3_column_int64(st, 23);
    int m2_rpm = sqlite3_column_int(st, 24);
    int m2_temp = sqlite3_column_int(st, 25);
    int m2_vps = sqlite3_column_int(st, 26);
    int m2_cps = sqlite3_column_int(st, 27);

    int has_motors = 0;
    if(sqlite3_column_type(st, 16) != SQLITE_NULL) has_motors = 1;
    if(sqlite3_column_type(st, 22) != SQLITE_NULL) has_motors = 1;

    double t_sec = (double)received_at_ns / 1e9;

    if(sb_appendf(sb,
        "{\"type\":\"telemetry\",\"t\":%.6f,\"t_ns\":%lld,\"seq\":%d,\"msg_ts_ns\":%lld,"
        "\"robot_id\":\"%s\",\"imu_id\":\"%s\",\"tilt_id\":\"%s\",\"tilt_status\":%d,"
        "\"accel\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f},"
        "\"gyro\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f},"
        "\"tilt\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f}",
        t_sec, (long long)received_at_ns, seq, (long long)msg_ts,
        robot_id ? (const char*)robot_id : "",
        imu_id ? (const char*)imu_id : "",
        tilt_id ? (const char*)tilt_id : "",
        tilt_status,
        acc_x, acc_y, acc_z,
        gyr_x, gyr_y, gyr_z,
        tilt_x, tilt_y, tilt_z) != 0){
        return -1;
    }

    if(has_motors){
        if(sb_appendf(sb,
            ",\"motors\":{"
              "\"motor1\":{\"id\":\"%s\",\"tics\":%lld,\"rpm\":%d,\"temperature_c\":%d,\"voltage_power_stage_mv\":%d,\"current_power_stage_ma\":%d},"
              "\"motor2\":{\"id\":\"%s\",\"tics\":%lld,\"rpm\":%d,\"temperature_c\":%d,\"voltage_power_stage_mv\":%d,\"current_power_stage_ma\":%d}"
            "}",
            m1_id ? (const char*)m1_id : "",
            (long long)m1_tics, m1_rpm, m1_temp, m1_vps, m1_cps,
            m2_id ? (const char*)m2_id : "",
            (long long)m2_tics, m2_rpm, m2_temp, m2_vps, m2_cps) != 0){
            return -2;
        }
    }

    if(sb_append(sb, "}") != 0) return -3;
    return 0;
}

int db_reader_history_last_json(db_reader_t *r, int limit, char **out_json, size_t *out_len){
    if(!r || !r->db || !out_json || !out_len) return -1;
    if(limit <= 0) limit = 300;
    if(limit > 5000) limit = 5000;

    const char *sql =
        "SELECT received_at_ns, robot_id, seq, msg_timestamp_ns, imu_id, "
        "acc_x, acc_y, acc_z, gyro_x, gyro_y, gyro_z, tilt_id, "
        "tilt_x, tilt_y, tilt_z, tilt_status, "
        "motor1_id, motor1_tics, motor1_rpm, motor1_temperature_c, motor1_voltage_power_stage_mv, motor1_current_power_stage_ma, "
        "motor2_id, motor2_tics, motor2_rpm, motor2_temperature_c, motor2_voltage_power_stage_mv, motor2_current_power_stage_ma "
        "FROM telemetry_state "
        "ORDER BY received_at_ns DESC "
        "LIMIT ?;";

    sqlite3_stmt *st = NULL;
    if(sqlite3_prepare_v2(r->db, sql, -1, &st, NULL) != SQLITE_OK){
        fprintf(stderr, "[DB] prepare history failed: %s\n", sqlite3_errmsg(r->db));
        return -2;
    }
    sqlite3_bind_int(st, 1, limit);

    strbuf_t sb = {0};
    if(sb_append(&sb, "{\"type\":\"history\",\"items\":[") != 0){
        sqlite3_finalize(st);
        return -3;
    }

    typedef struct { char *json; } row_t;
    row_t *rows = (row_t*)calloc((size_t)limit, sizeof(row_t));
    if(!rows){
        sqlite3_finalize(st);
        sb_free(&sb);
        return -4;
    }

    int n = 0;
    while(sqlite3_step(st) == SQLITE_ROW && n < limit){
        strbuf_t one = {0};
        if(append_row_as_sample_json(&one, st) != 0){
            sb_free(&one);
            break;
        }
        rows[n].json = one.buf;
        n++;
    }

    sqlite3_finalize(st);

    for(int i = n - 1; i >= 0; i--){
        if(rows[i].json){
            sb_append(&sb, rows[i].json);
            if(i != 0) sb_append(&sb, ",");
            free(rows[i].json);
        }
    }

    free(rows);

    if(sb_append(&sb, "]}") != 0){
        sb_free(&sb);
        return -5;
    }

    *out_json = sb.buf;
    *out_len = sb.len;
    return 0;
}