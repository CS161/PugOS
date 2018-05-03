#include "p-lib.hh"

void process_main() {
    sys_kdisplay(KDISPLAY_NONE);

    // test sprintf
    const char* tstr = "test string %d %p";
    char tout[50];
    console_printf("strlen '%s': %d, should be 17 without \\0\n", tstr, strlen(tstr));
    sprintf(tout, tstr, 12345, nullptr);
    console_printf("sprintf output: ");
    for (auto i=0; i < 50 && tout[i]; ++i) {
        console_printf("%c", tout[i]);
    }
    console_printf("\n");

    console_printf("tstr '%s' -> '%s', %%d=%d %%p=%p\n",
        tstr, tout, 12345, nullptr);


    // test atoi
    console_printf("testing atoi...\n");
    assert_eq(-473, atoi("-473"));
    assert_eq(0, atoi("0"));
    assert_eq(2342827, atoi("2342827"));
    console_printf("atoi tests finished\n");



    // test toupper
    console_printf("testing toupper...\n");
    assert_eq('A', toupper('A'));
    assert_eq('Z', toupper('Z'));
    assert_eq('A', toupper('a'));
    assert_eq('F', toupper('f'));
    assert_eq('Z', toupper('Z'));
    assert_eq(',', toupper(','));
    assert_eq('7', toupper('7'));
    assert_eq('a' - 1, toupper('a' - 1));
    assert_eq('z' + 1, toupper('z' + 1));
    assert_eq(' ', toupper(' '));
    console_printf("toupper tests finished\n");



    // test malloc
    size_t sz = PAGESIZE * 10;
    int nb = 10;
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
