#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main() {
    FILE *f = fopen("benchmarks/sample_large.log", "r");
    if (!f) {
        fprintf(stderr, "cannot open log file\n");
        return 1;
    }

    int info = 0, warn = 0, error = 0, debug = 0;
    int auth = 0, api = 0, db = 0, cache = 0, scheduler = 0;
    int total = 0;

    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) {
        total++;
        if (strstr(buf, "[INFO]")) info++;
        if (strstr(buf, "[WARN]")) warn++;
        if (strstr(buf, "[ERROR]")) error++;
        if (strstr(buf, "[DEBUG]")) debug++;
        if (strstr(buf, "(auth)")) auth++;
        if (strstr(buf, "(api)")) api++;
        if (strstr(buf, "(db)")) db++;
        if (strstr(buf, "(cache)")) cache++;
        if (strstr(buf, "(scheduler)")) scheduler++;
    }
    fclose(f);

    printf("total lines: %d\n", total);
    printf("INFO: %d, WARN: %d, ERROR: %d, DEBUG: %d\n", info, warn, error, debug);
    printf("auth: %d, api: %d, db: %d, cache: %d, scheduler: %d\n", auth, api, db, cache, scheduler);
    return 0;
}
