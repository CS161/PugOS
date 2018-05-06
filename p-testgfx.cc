#include "p-lib.hh"
#include "k-gfx.hh"


void process_main() {
	sys_kdisplay(KDISPLAY_NONE);

	sys_log_printf("[GFX] %dx%dx%d; pw: %d, pitch: %d\n", s_width, s_height,
		s_depth, s_pixelwidth, s_pitch);

	unsigned char* screen = reinterpret_cast<unsigned char*>(0x600000);
	sys_map_screen(screen);

	for (unsigned x = 0; x < 256; ++x) {
		for (unsigned y = 0; y < s_height; y++) {
			screen[PPOS(x, y)] = x;
		}
	}

	for (unsigned i = 0; i < 256; ++i) {
		sys_msleep(100);
		sys_swapcolor(i, 0, 0, 0);
	}
	
	sys_exit(0);
}
