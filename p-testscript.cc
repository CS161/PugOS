#include "p-lib.hh"

void process_main() {
    char* tests[11] = {
        "testmemfs",
        "testmsleep",
        "testpipe",
        "testppid",
        "testrwaddr",
        "testvfs",
        "testwaitpid",
        "testwritefs",
        "testzombie",
        "testeintr",
        "end"
    };

    sys_log_printf("Starting tests.\n");

    for (size_t i = 0; strcmp(tests[i], "end") != 0; i++) {
        char* args[] = { tests[i], nullptr };
        int r = sys_fork();
        if (r == 0) {
            console_printf("Running %s.\n", tests[i]);
            sys_log_printf("Running %s.\n", tests[i]);
            sys_execv(tests[i], args);
        }
        else {
            sys_waitpid(r);
        }
    }

    sys_log_printf("Tests complete.\n");

    sys_exit(0);
}