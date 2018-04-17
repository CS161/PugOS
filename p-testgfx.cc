#include "p-lib.hh"
#include "k-gfx.hh"


void process_main() {
	sys_kdisplay(KDISPLAY_NONE);

	sys_log_printf("[GFX] %dx%dx%d; pw: %d, pitch: %d\n", s_width, s_height,
		s_depth, s_pixelwidth, s_pitch);

	unsigned char* screen = reinterpret_cast<unsigned char*>(0x600000);
	sys_map_screen(screen);

	screen[0] = 0b1010'1010;
	// screen[s_width * s_height] = 0b1010'1010;
	// for (unsigned i = 0; i < SCREEN_MEMSIZE; ++i) {
	// 	screen[i] = -1;
	// }

	// auto addr = pixel_at(0, 0);
	// sys_log_printf("[GFX] writing to screen at %p\n", addr);
	// sys_memset(pixel_at(0,0), -1, 1);
	// sys_memset(pixel_at(1,1), -1, 1);
	sys_exit(0);
}
