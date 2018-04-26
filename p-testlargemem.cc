#include "p-lib.hh"

void process_main() {
    sys_kdisplay(KDISPLAY_NONE);

    // trying for 512 MB
    size_t goal = 100 * 1024 * 1024;
    size_t pages;

    for (pages = 0; pages < goal / PAGESIZE; ++pages) {
    	auto r = sys_page_alloc(
    		reinterpret_cast<void*>(0x600000 + pages * PAGESIZE));
    	if (r < 0) {
    		break;
    	}
    }

    console_printf("Alloc'd: \n"
    	"    %d / %d pages (%d MB / %d MB) \n"
    	"...before running out of memory\n",
    	pages, goal/PAGESIZE, pages*PAGESIZE / 1024 / 1024,
    	goal / 1024 / 1024);

    sys_exit(0);
}
