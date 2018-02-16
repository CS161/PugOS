#include "kernel.hh"

// hash function
static constexpr size_t h(const char* str) {
    size_t result = 0;
    for (size_t i = 0; str[i] != '\0'; i++) {
        result = str[i] + (result * 31);
    }
    return result;
}


static constexpr bool filter(const char* file, const char* func) {
	switch (h(file)) {
		case h("kernel.cc"): return true;
		case h("k-cpu.cc"): {
			switch (h(func)) {
				case h("annihilate"): return true;
				default: return false;
			}
		}
		default: return false;
	}
}


void debug_printf_(const char* file, const char* func, int line,
				   const char* format, ...) {
	//log_printf("debug print from %s:%s line %d\n", file, func, line);
	if (filter(file, func)) {
		va_list val;
	    va_start(val, format);
	    log_vprintf(format, val);
	    va_end(val);
	}
}
