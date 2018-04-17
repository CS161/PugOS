#ifndef CHICKADEE_K_GFX_HH
#define CHICKADEE_K_GFX_HH


#define SCREEN_MEMBASE 0xA0000		// EGA/VGA buffer location (physical)
// #define SCREEN_KADDR (SCREEN_MBASE + 0xFFFF'FFFF'8000'0000)
// #define SCREEN_PADDR 0xFF44'0000'0000'0000
// #define screen reinterpret_cast<void*>(SCREEN_PADDR)

#define s_width 320
#define s_height 200
#define s_depth 8			// 16 colors
#define SCREEN_MEMSIZE (s_width * s_height * s_depth / 8)

// how many bytes of VRAM you should skip to go one pixel right
#define s_pixelwidth (s_depth / 8)
// how many bytes of VRAM you should skip to go one pixel down
#define s_pitch (s_width * s_pixelwidth)

// uintptr_t constexpr ppos(unsigned x, unsigned y) {
// 	auto addr = SCREEN_PADDR + (y * s_width * s_depth) / 8 + (x * s_depth) / 8;
// 	// sys_log_printf("[GFX] %dx%d = %p\n", x, y, addr);
// 	return addr;
// }


#endif
