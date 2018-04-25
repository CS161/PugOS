#ifndef CHICKADEE_P_LIB_H
#define CHICKADEE_P_LIB_H
#include "lib.hh"
#include "x86-64.h"
#if CHICKADEE_KERNEL
#error "p-lib.hh should not be used by kernel code."
#endif

// p-lib.hh
//
//    Support code for Chickadee processes.


// SYSTEM CALLS

inline uintptr_t syscall0(int syscallno) {
    register uintptr_t rax asm("rax") = syscallno;
    asm volatile ("syscall"
                  : "+a" (rax)
                  :
                  : "cc", "rcx", "rdx", "rsi", "rdi",
                    "r8", "r9", "r10", "r11");
    return rax;
}

inline uintptr_t syscall0(int syscallno, uintptr_t arg0) {
    register uintptr_t rax asm("rax") = syscallno;
    asm volatile ("syscall"
                  : "+a" (rax), "+D" (arg0)
                  :
                  : "cc", "rcx", "rdx", "rsi",
                    "r8", "r9", "r10", "r11");
    return rax;
}

inline uintptr_t syscall0(int syscallno, uintptr_t arg0,
                          uintptr_t arg1) {
    register uintptr_t rax asm("rax") = syscallno;
    asm volatile ("syscall"
                  : "+a" (rax), "+D" (arg0), "+S" (arg1)
                  :
                  : "cc", "rcx", "rdx", "r8", "r9", "r10", "r11");
    return rax;
}

inline uintptr_t syscall0(int syscallno, uintptr_t arg0,
                          uintptr_t arg1, uintptr_t arg2) {
    register uintptr_t rax asm("rax") = syscallno;
    asm volatile ("syscall"
                  : "+a" (rax), "+D" (arg0), "+S" (arg1), "+d" (arg2)
                  :
                  : "cc", "rcx", "r8", "r9", "r10", "r11");
    return rax;
}

inline uintptr_t syscall0(int syscallno, uintptr_t arg0,
                          uintptr_t arg1, uintptr_t arg2,
                          uintptr_t arg3) {
    register uintptr_t rax asm("rax") = syscallno;
    register uintptr_t r10 asm("r10") = arg3;
    asm volatile ("syscall"
                  : "+a" (rax), "+D" (arg0), "+S" (arg1), "+d" (arg2),
                    "+r" (r10)
                  :
                  : "cc", "rcx", "r8", "r9", "r11");
    return rax;
}

inline void clobber_memory(void* ptr) {
    asm volatile ("" : "+m" (*(char (*)[]) ptr));
}

inline void access_memory(const void* ptr) {
    asm volatile ("" : : "m" (*(const char (*)[]) ptr));
}


// sys_getpid
//    Return current process ID.
static inline pid_t sys_getpid(void) {
    return syscall0(SYSCALL_GETPID);
}

// sys_yield
//    Yield control of the CPU to the kernel. The kernel will pick another
//    process to run, if possible.
static inline void sys_yield(void) {
    syscall0(SYSCALL_YIELD);
}

// sys_page_alloc(addr)
//    Allocate a page of memory at address `addr`. `Addr` must be page-aligned
//    (i.e., a multiple of PAGESIZE == 4096). Returns 0 on success and -1
//    on failure.
static inline int sys_page_alloc(void* addr) {
    return syscall0(SYSCALL_PAGE_ALLOC, reinterpret_cast<uintptr_t>(addr));
}

// sys_fork()
//    Fork the current process. On success, return the child's process ID to
//    the parent, and return 0 to the child. On failure, return -1.
static inline pid_t sys_fork(void) {
    return syscall0(SYSCALL_FORK);
}

// sys_exit(status)
//    Exit this process. Does not return.
static inline void __attribute__((noreturn)) sys_exit(int status) {
    syscall0(SYSCALL_EXIT, status);
    assert(false);
}

// sys_pause()
//    A version of `sys_yield` that spins in the kernel long enough
//    for kernel timer interrupts to occur.
static inline void sys_pause() {
    syscall0(SYSCALL_PAUSE);
}

// sys_kdisplay(display_type)
//    Set the display type (one of the KDISPLAY constants).
static inline int sys_kdisplay(int display_type) {
    return syscall0(SYSCALL_KDISPLAY, display_type);
}

// sys_msleep(msec)
//    Block for approximately `msec` milliseconds.
static inline int sys_msleep(unsigned msec) {
    return syscall0(SYSCALL_MSLEEP, (uintptr_t) msec);
}

// sys_getppid()
//    Return parent process ID.
static inline pid_t sys_getppid(void) {
    return syscall0(SYSCALL_GETPPID);
}

// sys_waitpid(pid, status, options)
//    Wait until process `pid` exits and report its status. The status
//    is stored in `*status`, if `status != nullptr`. If `pid == 0`,
//    waits for any child. If `options == W_NOHANG`, returns immediately.
static inline pid_t sys_waitpid(pid_t pid,
                                int* status = nullptr,
                                int options = 0) {
    register uintptr_t rax asm("rax") = SYSCALL_WAITPID;
    register uintptr_t rdi asm("rdi") = pid;
    register uintptr_t rsi asm("rsi") = options;
    register uintptr_t rcx asm("rcx");
    asm volatile ("syscall"
                  : "+a" (rax), "+D" (rdi), "+S" (rsi), "=c" (rcx)
                  :
                  : "cc", "rdx",
                    "r8", "r9", "r10", "r11");
    if (status != nullptr) {
        *status = rcx;
    }
    return rax;
}

// sys_read(fd, buf, sz)
//    Read bytes from `fd` into `buf`. Read at most `sz` bytes. Return
//    the number of bytes read, which is 0 at EOF.
inline ssize_t sys_read(int fd, char* buf, size_t sz) {
    clobber_memory(buf);
    return syscall0(SYSCALL_READ, fd, reinterpret_cast<uintptr_t>(buf), sz);
}

// sys_write(fd, buf, sz)
//    Write bytes to `fd` from `buf`. Write at most `sz` bytes. Return
//    the number of bytes written.
inline ssize_t sys_write(int fd, const char* buf, size_t sz) {
    access_memory(buf);
    return syscall0(SYSCALL_WRITE, fd, reinterpret_cast<uintptr_t>(buf), sz);
}

// sys_dup2(oldfd, newfd)
//    Make `newfd` a reference to the same file structure as `oldfd`.
inline int sys_dup2(int oldfd, int newfd) {
    return syscall0(SYSCALL_DUP2, oldfd, newfd);
}

// sys_close(fd)
//    Close `fd`.
inline int sys_close(int fd) {
    return syscall0(SYSCALL_CLOSE, fd);
}

// sys_open(path, flags)
//    Open a new file descriptor for pathname `path`. `flags` should
//    contain at least one of `OF_READ` and `OF_WRITE`.
inline int sys_open(const char* path, int flags) {
    access_memory(path);
    return syscall0(SYSCALL_OPEN, reinterpret_cast<uintptr_t>(path),
                    flags);
}

// sys_pipe(pfd)
//    Create a pipe.
inline int sys_pipe(int pfd[2]) {
    uintptr_t r = syscall0(SYSCALL_PIPE);
    if (!is_error(r)) {
        pfd[0] = r;
        pfd[1] = r >> 32;
        r = 0;
    }
    return r;
}

// sys_log_printf(format, args)
inline void sys_log_printf(const char* format, ...) {
    va_list val;
    va_start(val, format);
    syscall0(SYSCALL_LOG_PRINTF,
             reinterpret_cast<uintptr_t>(format),
             reinterpret_cast<uintptr_t>(&val));
    va_end(val);
}

inline unsigned long sys_getticks() {
    return syscall0(SYSCALL_GETTICKS);
}

// sys_execv(program_name, argv, argc)
//    Replace this process image with a new image running `program_name`
//    with `argc` arguments, stored in argument array `argv`. Returns
//    only on failure.
inline int sys_execv(const char* program_name, const char* const* argv,
                     size_t argc) {
    access_memory(program_name);
    access_memory(argv);
    return syscall0(SYSCALL_EXECV,
                    reinterpret_cast<uintptr_t>(program_name),
                    reinterpret_cast<uintptr_t>(argv), argc);
}

// sys_execv(program_name, argv)
//    Replace this process image with a new image running `program_name`
//    with arguments `argv`. `argv` is a null-terminated array. Returns
//    only on failure.
inline int sys_execv(const char* program_name, const char* const* argv) {
    size_t argc = 0;
    while (argv && argv[argc] != nullptr) {
        ++argc;
    }
    return sys_execv(program_name, argv, argc);
}

// sys_unlink(pathname)
//    Remove the file named `pathname`.
inline int sys_unlink(const char* pathname) {
    access_memory(pathname);
    return syscall0(SYSCALL_UNLINK, reinterpret_cast<uintptr_t>(pathname));
}

// sys_readdiskfile(filename, buf, sz, off)
//    Read bytes from disk file `filename` into `buf`. Read at most `sz`
//    bytes starting at file offset `off`. Return the number of bytes
//    read, which is 0 at EOF.
inline ssize_t sys_readdiskfile(const char* filename,
                                char* buf, size_t sz, size_t off) {
    clobber_memory(buf);
    return syscall0(SYSCALL_READDISKFILE,
                    reinterpret_cast<uintptr_t>(filename),
                    reinterpret_cast<uintptr_t>(buf), sz, off);
}

// sys_memset
//    The least safe syscall in history
inline void sys_memset(uintptr_t v, int c, size_t n) {
    syscall0(SYSCALL_MEMSET, v, c, n);
}

// sys_sync(drop)
//    Synchronize all modified buffer cache contents to disk. If
//    `drop` is true, then additionally drop all buffer cache contents,
//    so that future reads start from an empty cache.
inline int sys_sync(int drop = 0) {
    return syscall0(SYSCALL_SYNC, drop);
}

// sys_lseek(fd, offset, origin)
//    Set the current file position for `fd` to `off`, relative to
//    `origin` (one of the `LSEEK_` constants). Returns the new file
//    position (or, for `LSEEK_SIZE`, the file size).
inline ssize_t sys_lseek(int fd, ssize_t off, int origin) {
    return syscall0(SYSCALL_LSEEK, fd, off, origin);
}

// sys_ftruncate(fd, len)
//    Set the size of file `fd` to `sz`. If the file was previously
//    larger, the extra data is lost; if it was shorter, it is extended
//    with zero bytes.
inline int sys_ftruncate(int fd, size_t sz) {
    return syscall0(SYSCALL_FTRUNCATE, fd, sz);
}

// sys_rename(oldpath, newpath)
//    Rename the file with name `oldpath` to `newpath`.
inline int sys_rename(const char* oldpath, const char* newpath) {
    access_memory(oldpath);
    access_memory(newpath);
    return syscall0(SYSCALL_RENAME, reinterpret_cast<uintptr_t>(oldpath),
                    reinterpret_cast<uintptr_t>(newpath));
}

// sys_gettid()
//    Return the current thread ID.
inline pid_t sys_gettid() {
    return syscall0(SYSCALL_GETTID);
}

// sys_clone(function, arg, stack_top)
//    Create a new thread running `function` with `arg`, starting at
//    stack address `stack_top`. Returns the new thread's thread ID.
//
//    In the context of the new thread, when the `function` returns,
//    the thread should call `sys_texit` with the function's return value
//    as argument.
//
//    Unlike most other system calls, we recommend you implement `sys_clone`
//    in `p-lib.cc`.
pid_t sys_clone(int (*function)(void*), void* arg, char* stack_top);

// sys_texit(status)
//    Exit the current thread with exit status `status`. If this is
//    the last thread in the process, this will have the same effect
//    as `sys_exit(status)`.
inline void __attribute__((noreturn)) sys_texit(int status) {
    syscall0(SYSCALL_TEXIT, status);
    assert(false);
}

// sys_panic(msg)
//    Panic.
static inline pid_t __attribute__((noreturn)) sys_panic(const char* msg) {
    syscall0(SYSCALL_PANIC, reinterpret_cast<uintptr_t>(msg));
    while (1) {
    }
}

// sys_map_console(addr)
//    Map console memory to addr in process' virtual memory. Returns -1 on
//    failure.
static inline int sys_map_console(void* addr) {
    return syscall0(SYSCALL_MAP_CONSOLE, reinterpret_cast<uintptr_t>(addr));
}

// sys_map_screen(addr)
//    Map screen memory to addr in process' virtual memory. Returns -1 on
//    failure.
static inline int sys_map_screen(void* addr) {
    return syscall0(SYSCALL_MAP_SCREEN, reinterpret_cast<uintptr_t>(addr));
}

// sys_commit_seppuku()
//    Die an honorable death
static inline int sys_commit_seppuku() {
    return syscall0(SYSCALL_COMMIT_SEPPUKU);
}

// dprintf(fd, format, ...)
//    Construct a string from `format` and pass it to `sys_write(fd)`.
//    Returns the number of characters printed, or E_2BIG if the string
//    could not be constructed.
int dprintf(int fd, const char* format, ...);

// printf(format, ...)
//    Like `dprintf(1, format, ...)`.
int printf(const char* format, ...);

#endif
