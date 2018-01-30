#include "p-lib.hh"
#define ALLOC_SLOWDOWN 100

extern uint8_t end[];

uint8_t* heap_top;
uint8_t* stack_bottom;

void process_main(void) {
    // Fork three new copies. (But ignore failures.)
    (void) sys_fork();
    (void) sys_fork();

    pid_t p = sys_getpid();
    srand(p);

    // Console testing
    bool test_console = false;
    if (test_console) {
        sys_map_console(console);
        for (int i = 0; i < CONSOLE_ROWS * CONSOLE_COLUMNS; ++i) {
          console[i] = '*' | 0x5000;
        }
    }

    // The heap starts on the page right after the 'end' symbol,
    // whose address is the first address not allocated to process code
    // or data.
    heap_top = ROUNDUP((uint8_t*) end, PAGESIZE);

    // The bottom of the stack is the first address on the current
    // stack page (this process never needs more than one stack page).
    stack_bottom = ROUNDDOWN((uint8_t*) read_rsp() - 1, PAGESIZE);

    while (1) {
        if ((rand() % ALLOC_SLOWDOWN) < p) {
            if (heap_top == stack_bottom || sys_page_alloc(heap_top) < 0) {
                break;
            }
            *heap_top = p;      /* check we have write access to new page */
            heap_top += PAGESIZE;
        }
        sys_yield();
        if (rand() < RAND_MAX / 32) {
            sys_pause();
        }
    }

    // After running out of memory, do nothing forever
    while (1) {
        sys_yield();
    }
}
