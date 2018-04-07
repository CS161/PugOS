#include "p-lib.hh"

#define MBASE 0xA0000		// EGA/VGA buffer location
#define width 320
#define height 200
#define depth 4			// 16 colors

// how many bytes of VRAM you should skip to go one pixel right
#define pixelwidth (depth / 8)
// how many bytes of VRAM you should skip to go one pixel down
#define pitch (width * pixelwidth)

uintptr_t pixel_at(unsigned x, unsigned y) {
	auto addr = MBASE + (y * width * depth) / 8 + (x * depth) / 8;
	sys_log_printf("[GFX] %dx%d = %p\n", x, y, addr);
	return addr;
}

void process_main() {
	sys_log_printf("[GFX] %dx%dx%d; pw: %d, pitch: %d\n", width, height, depth,
		pixelwidth, pitch);


	// auto addr = pixel_at(0, 0);
	// sys_log_printf("[GFX] writing to screen at %p\n", addr);
	sys_memset(pixel_at(0,0), -1, 1);
	sys_memset(pixel_at(1,1), -1, 1);
	sys_exit(0);
}
