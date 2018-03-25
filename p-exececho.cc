#include "p-lib.hh"

void process_main() {
    sys_kdisplay(KDISPLAY_NONE);

    // sys_write(1, "Running bad arg tests...\n", 50);

    // const char* args1 = {
    // 	(char*) nullptr
    // };
    // int r = sys_execv(nullptr, args1);
    // assert_eq(r, E_INVAL);

    // char bad_string = {
    // 	'a', 'b', 'c'
    // };
    // const char* args2 = {
    // 	bad_string
    // };
    // r = sys_execv(bad_string, args2);
    // assert_eq(r, E_INVAL);

    // char* bad_pointer = reinterpret_cast<char*>(0xFFFFFFFFFFFFFFFFUL);
    // const char* args3 = {
    // 	bad_pointer;
    // }
    // r = sys_execv(bad_pointer, args3);
    // assert_eq(r, E_FAULT);

    sys_write(1, "About to greet you...\n", 22);

    const char* args[] = {
        "echo", "hello,", "world", nullptr
    };
    int r = sys_execv("echo", args);
    assert_eq(r, 0);

    sys_exit(0);
}
