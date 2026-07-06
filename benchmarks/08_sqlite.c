#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

int main() {
    remove("benchmarks/logs.db");

    sqlite3 *db;
    int rc = sqlite3_open("benchmarks/logs.db", &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "cannot open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    rc = sqlite3_exec(db, "CREATE TABLE logs (id INTEGER PRIMARY KEY, timestamp TEXT, level TEXT, service TEXT, message TEXT)", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "cannot create table\n");
        return 1;
    }

    rc = sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL);

    FILE *f = fopen("benchmarks/sample_large.log", "r");
    if (!f) {
        fprintf(stderr, "cannot open log file\n");
        return 1;
    }

    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, "INSERT INTO logs (timestamp, level, service, message) VALUES (?, ?, ?, ?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "cannot prepare: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    char buf[4096];
    int total = 0;
    while (fgets(buf, sizeof(buf), f)) {
        total++;
        // Parse line: "2024-01-15 08:32:01.220 [INFO] (auth) connection timeout"
        char timestamp[24];
        strncpy(timestamp, buf, 23);
        timestamp[23] = '\0';

        char *lb = strchr(buf, '[');
        char *rb = strchr(buf, ']');
        char level[16] = {0};
        if (lb && rb) {
            int len = rb - lb - 1;
            if (len < 16) strncpy(level, lb + 1, len);
        }

        char *lp = strchr(buf, '(');
        char *rp = strchr(buf, ')');
        char service[32] = {0};
        if (lp && rp) {
            int len = rp - lp - 1;
            if (len < 32) strncpy(service, lp + 1, len);
        }

        char *msg_start = rp + 2;
        char message[256] = {0};
        int msg_len = strlen(msg_start);
        if (msg_len > 0 && msg_start[msg_len - 1] == '\n') msg_start[msg_len - 1] = '\0';
        strncpy(message, msg_start, sizeof(message) - 1);

        sqlite3_bind_text(stmt, 1, timestamp, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, level, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, service, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, message, -1, SQLITE_STATIC);

        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    fclose(f);

    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    sqlite3_finalize(stmt);
    printf("wrote %d rows\n", total);

    // Read back
    sqlite3_stmt *read_stmt;
    rc = sqlite3_prepare_v2(db, "SELECT id, timestamp, level, service, message FROM logs ORDER BY id", -1, &read_stmt, NULL);
    int read_count = 0;
    while (sqlite3_step(read_stmt) == SQLITE_ROW) {
        read_count++;
        sqlite3_column_text(read_stmt, 1);
        sqlite3_column_text(read_stmt, 2);
        sqlite3_column_text(read_stmt, 3);
        sqlite3_column_text(read_stmt, 4);
    }
    sqlite3_finalize(read_stmt);
    printf("read back %d rows\n", read_count);

    sqlite3_close(db);
    return 0;
}
