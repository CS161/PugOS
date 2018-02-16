#ifndef CHICKADEE_DEBUG_H
#define CHICKADEE_DEBUG_H

#if DEBUG

#define debug_printf(...) \
    debug_printf_(__FILE__, __FUNCTION__, __LINE__, __VA_ARGS__)

void debug_printf_(const char* file, const char* func, int line,
                   const char* format, ...);

#else

#define debug_printf(...) do {} while(0)

#endif
#endif
