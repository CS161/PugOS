#include "p-lib.hh"

void process_main(int argc, char** argv) {
	sys_log_printf("entered p-echo, argc = %d, argv = %p\n", argc, argv);
    sys_kdisplay(KDISPLAY_NONE);

    sys_log_printf("passed sys_kdisplay\n");

    for (int i = 1; i < argc; ++i) {
    	sys_log_printf("&argv[%d] = %p\n", i, argv[i]);
    	char* c = argv[i];
    	sys_log_printf("argv[%d] = %c\n", i, c);
        if (i > 1) {
            sys_write(1, " ", 1);
        }
        sys_write(1, argv[i], strlen(argv[i]));
    }
    sys_write(1, "\n", 1);

    sys_exit(0);
}
