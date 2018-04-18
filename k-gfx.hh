#ifndef CHICKADEE_K_GFX_HH
#define CHICKADEE_K_GFX_HH


#define SCREEN_MEMBASE 0xA0000		// VGA buffer location (physical)

#define s_width 320
#define s_height 200
#define s_depth 8			// 256 colors
#define SCREEN_MEMSIZE (s_width * s_height * s_depth / 8)

// how many bytes of VRAM you skip to go one pixel right
#define s_pixelwidth (s_depth / 8)
// how many bytes of VRAM you skip to go one pixel down
#define s_pitch (s_width * s_pixelwidth)

#define PPOS(x, y) (x * s_pixelwidth + y * s_pitch)

#endif
