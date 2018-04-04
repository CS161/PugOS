#ifndef CHICKADEE_DEBUG_HH
#define CHICKADEE_DEBUG_HH

#if DEBUG

#define debug_printf(...)                                                   \
    do { if (debug_filter(__FILE__, __FUNCTION__))                          \
            debug_printf_(__FILE__, __FUNCTION__, __LINE__, __VA_ARGS__);   \
       } while (0)

#define debug_pulse() \
    debug_printf("%s:%d pulse in %s\n", __FILE__, __LINE__, __FUNCTION__)

inline constexpr size_t debug_hash(const char* str) {
    size_t result = 0;
    for (size_t i = 0; str[i] != '\0'; i++) {
        result = str[i] + (result * 31);
    }
    return result;
}


inline constexpr bool debug_filter(const char* file, const char* func) {
    switch (debug_hash(file)) {
        // case debug_hash("k-cpu.cc"):
        //     switch (debug_hash(func)) {
        //         case debug_hash("enqueue"): return true;
        //         default: return false;
        //     }
        // case debug_hash("kernel.cc"):
        //     switch (debug_hash(func)) {
        //         // case debug_hash("exception"): return false;
        //         // case debug_hash("process_exit"): return false;
        //         case debug_hash("process_setup"): return false;
        //         // case debug_hash("process_fork"): return true;
        //         // case debug_hash("syscall"): return false;
        //         default: return true;
        //     }
        case debug_hash("k-chkfs.cc"):
            return true;
            break;
        default: return false;
    }
}


inline void debug_printf_(const char* file, const char* func, int line,
                   const char* format, ...) {
    //log_printf("debug print from %s:%s line %d\n", file, func, line);
    if (debug_filter(file, func)) {
        va_list val;
        va_start(val, format);
        log_vprintf(format, val);
        va_end(val);
    }
}


#else

#define debug_printf(...) do {} while(0)
#define debug_pulse() do {} while(0)

#endif
#endif
