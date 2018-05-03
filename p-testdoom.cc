#include "p-lib.hh"

void process_main() {
    sys_kdisplay(KDISPLAY_NONE);

    // test malloc
    size_t sz = 1024;
    int nb = 50;
    char* blocks[nb];
    console_printf("testing malloc...\n");

    for (auto i = 0; i < nb; ++i) {
        blocks[i] = malloc(sz);
        if (!blocks[i]) {
            console_printf("malloc failed after allocating %d bytes "
                "(%d/%d blocks)\n", i * sz, i, nb);
            sys_exit(1);
        }
        memset(blocks[i], i, sz);
    }
    // validate data
    for (auto i = 0; i < nb; ++i) {
        for (size_t off = 0; off < sz; ++off) {
            if (blocks[i][off] != i) {
                console_printf("malloc'd blocks corrupted: block %d off %u != "
                    "%d\n", i, off, i);
                break;
            }
        }
    }
    console_printf("malloc tests finished\n");

    sys_exit(0);
}
