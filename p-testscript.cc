#include "p-lib.hh"

void process_main() {
    const char* tests[9] = {
        "testrwaddr",
        "testmemfs",
        "testmsleep",
        "testpipe",
        "testvfs",
        "testwaitpid",
        "testwritefs",
        "testzombie",
        "end"
    };

    console_printf("Starting tests.\n");

    for (size_t i = 0; strcmp(tests[i], "end") != 0; i++) {
        const char* args[] = { tests[i], nullptr };
        int r = sys_fork();
        if (r == 0) {
            console_printf("\nRunning %s.\n", tests[i]);
            sys_execv(tests[i], args);
        }
        else {
            sys_waitpid(r);
        }
    }

    console_printf("\n"
        "*******************\n"
        "Tests complete.\n"
        "*******************\n");

    sys_exit(0);
}