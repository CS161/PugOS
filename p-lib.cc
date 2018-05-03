#include "p-lib.hh"

// dprintf
//    Construct a string from `format` and pass it to `sys_write(fd)`.
//    Returns the number of characters printed, or E_2BIG if the string
//    could not be constructed.

int dprintf(int fd, const char* format, ...) {
    char buf[1025];
    va_list val;
    va_start(val, format);
    size_t n = vsnprintf(buf, sizeof(buf), format, val);
    if (n < sizeof(buf)) {
        return sys_write(fd, buf, n);
    } else {
        return E_2BIG;
    }
}


// printf
//    Like `printf(1, ...)`.

int printf(const char* format, ...) {
    char buf[1025];
    va_list val;
    va_start(val, format);
    size_t n = vsnprintf(buf, sizeof(buf), format, val);
    if (n < sizeof(buf)) {
        return sys_write(1, buf, n);
    } else {
        return E_2BIG;
    }
}


// panic, assert_fail
//     Call the SYSCALL_PANIC system call so the kernel loops until Control-C.

void panic(const char* format, ...) {
    va_list val;
    va_start(val, format);
    char buf[160];
    memcpy(buf, "PANIC: ", 7);
    int len = vsnprintf(&buf[7], sizeof(buf) - 7, format, val) + 7;
    va_end(val);
    if (len > 0 && buf[len - 1] != '\n') {
        strcpy(buf + len - (len == (int) sizeof(buf) - 1), "\n");
    }
    (void) console_printf(CPOS(23, 0), 0xC000, "%s", buf);
    sys_panic(NULL);
}

int error_vprintf(int cpos, int color, const char* format, va_list val) {
    return console_vprintf(cpos, color, format, val);
}

void assert_fail(const char* file, int line, const char* msg) {
    error_printf(CPOS(23, 0), COLOR_ERROR,
                 "%s:%d: user assertion '%s' failed\n",
                 file, line, msg);
    sys_panic(NULL);
}



// sys_clone
//    Create a new thread.

pid_t sys_clone(int (*function)(void*), void* arg, char* stack_top) {
    pid_t r = syscall0(SYSCALL_CLONE);
    if (r == 0) {
        register uintptr_t rax asm("rax") =
            reinterpret_cast<uintptr_t>(function);
        asm volatile ("call *%%rax"
                      : "+a" (rax), "+D" (arg)
                      :
                      : "cc", "rcx", "rdx", "rsi",
                        "r8", "r9", "r10", "r11");
        sys_log_printf("sys_clone returned, exit status %d\n", rax);
        sys_texit(rax);
    }
    return r;
}


// atoi
//    Return integer from string
//    (Implementation from https://www.geeksforgeeks.org/write-your-own-atoi/)

bool isNumericChar(char x)
{
    return x >= '0' && x <= '9';
}
  
// A simple atoi() function. If the given string contains
// any invalid character, then this function returns 0
int atoi(const char *str)
{
    if (!str)
       return 0;
  
    int res = 0;
    int sign = 1;
    int i = 0;

    if (str[0] == '-')
    {
        sign = -1;
        i++;
    }
  
    for (; str[i] != '\0'; ++i)
    {
        if (!isNumericChar(str[i])) {
            sys_log_printf("WARNING: atoi(%s) errored due to invalid char %c\n",
                str, str[i]);
            return 0;
        }
        res = res*10 + str[i] - '0';
    }
  
    // Return result with sign
    return sign*res;
}
