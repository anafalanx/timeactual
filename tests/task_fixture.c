/* Exercises the real task runner without starting the GUI or touching user data. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *mode = getenv("TIMEACTUAL_TASK_FIXTURE");
    if (!mode || argc != 3 || strcmp(argv[1], "--selftest")) return 97;
    if (!strcmp(mode, "timeout")) { Sleep(30000); return 0; }
    if (!strcmp(mode, "missing")) return 0;
    FILE *report = fopen(argv[2], "wb");
    if (!report) return 98;
    fputs(!strcmp(mode, "failure") ? "status=FAIL\n" : "status=ok\n", report);
    fclose(report);
    return !strcmp(mode, "exit") ? 23 : 0;
}
