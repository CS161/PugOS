#include "kernel.hh"
#include "k-apic.hh"
#include "k-vmiter.hh"

// kernel.cc
//
//    This is the kernel.

unsigned long ticks;            // # timer interrupts so far on CPU 0

static void memshow();
static void process_setup(pid_t pid, const char* program_name);


// kernel_start(command)
//    Initialize the hardware and processes and start running. The `command`
//    string is an optional string passed from the boot loader.

void kernel_start(const char* command) {
    hardware_init();
    console_clear();

    // Set up process descriptors
    for (pid_t i = 0; i < NPROC; i++) {
        ptable[i] = nullptr;
    }

    auto irqs = ptable_lock.lock();
    process_setup(1, "p-allocator");
    ptable_lock.unlock(irqs);

    // Switch to the first process
    cpus[0].schedule(nullptr);
}


// process_setup(pid, name)
//    Load application program `name` as process number `pid`.
//    This loads the application's code and data into memory, sets its
//    %rip and %rsp, gives it a stack page, and marks it as runnable.

void process_setup(pid_t pid, const char* name) {
    assert(!ptable[pid]);
    proc* p = ptable[pid] = reinterpret_cast<proc*>(kallocpage());
    x86_64_pagetable* npt = kalloc_pagetable();
    assert(p && npt);
    p->init_user(pid, npt);

    int r = p->load(name);
    assert(r >= 0);
    p->regs_->reg_rsp = MEMSIZE_VIRTUAL;
    x86_64_page* stkpg = kallocpage();
    assert(stkpg);
    vmiter(p, p->regs_->reg_rsp - PAGESIZE).map(ka2pa(stkpg));

    int cpu = pid % ncpu;
    cpus[cpu].runq_lock_.lock_noirq();
    cpus[cpu].enqueue(p);
    cpus[cpu].runq_lock_.unlock_noirq();
}


// 
pid_t process_fork(proc* oldp) {
    log_printf("-HIGHMEM_BASE = %p\n", -HIGHMEM_BASE);

    // 1. Allocate a new PID.
    pid_t fpid = -1;

    auto irqs = ptable_lock.lock();
    for (pid_t i = 1; i < NPROC; i++) {
        if (!ptable[i]
            | (ptable[i] && ptable[i]->state_ == proc::blank)) {
            fpid = i;
            log_printf("Forking into pid %d\n", fpid);
            break;
        }
    }
    if (fpid < 1) {
        // No free process slot found
        return -1;
    }

    // 2. Allocate a struct proc and a page table.
    // 5. Store the new process in the process table.
    proc* fproc = ptable[fpid] = reinterpret_cast<proc*>(kallocpage());
    fproc->state_ = proc::broken;
    ptable_lock.unlock(irqs);
    x86_64_pagetable* fpt = kalloc_pagetable();
    assert(fproc && fpt);

    // 3. Copy the parent process’s user-accessible memory and map the copies
    // into the new process’s page table.
    for (vmiter source(oldp); source.low(); source.next()) {
        if (source.user() && source.writable()) {
            uintptr_t npage = ka2pa(kallocpage());
            if (!npage) return -1;
            log_printf("Attempting to copy user-writable page (%d bytes) at %p "
                       "to new page at %p\n", PAGESIZE, source.pa(), npage);
            memcpy(reinterpret_cast<void*>(pa2ka(npage)),
                   reinterpret_cast<void*>(pa2ka(source.pa())), PAGESIZE);
            if (!vmiter(fpt, source.va()).map(npage, source.perm()))
                return -1;
        }
        else if (source.user()) {
            if (!vmiter(fpt, source.va()).map(source.pa(), source.perm()))
                return -1;
        }
    }

    // Initialize fproc structure (note: sets registers wrong)
    fproc->init_user(fpid, fpt);

    // 4. Initialize the new process’s registers to a copy of the old process’s
    // registers.
    fproc->regs_ = oldp->regs_;

    // 6. Enqueue the new process on some CPU’s run queue.
    int cpu = fpid % ncpu;
    cpus[cpu].runq_lock_.lock_noirq();
    cpus[cpu].enqueue(fproc);
    cpus[cpu].runq_lock_.unlock_noirq();

    // 7. Arrange for the new PID to be returned to the parent process and 0 to
    // be returned to the child process.
    fproc->regs_->reg_rax = 0;
    return fpid;
}


// proc::exception(reg)
//    Exception handler (for interrupts, traps, and faults).
//
//    The register values from exception time are stored in `reg`.
//    The processor responds to an exception by saving application state on
//    the current CPU stack, then jumping to kernel assembly code (in
//    k-exception.S). That code transfers the state to the current kernel
//    task's stack, then calls proc::exception().

void proc::exception(regstate* regs) {
    // It can be useful to log events using `log_printf`.
    // Events logged this way are stored in the host's `log.txt` file.
    /*log_printf("proc %d: exception %d\n", this->pid_, regs->reg_intno);*/

    // Show the current cursor location.
    console_show_cursor(cursorpos);

    // If Control-C was typed, exit the virtual machine.
    check_keyboard();


    // Actually handle the exception.
    switch (regs->reg_intno) {

    case INT_IRQ + IRQ_TIMER: {
        cpustate* cpu = this_cpu();
        if (cpu->index_ == 0) {
            ++ticks;
            memshow();
        }
        lapicstate::get().ack();
        this->regs_ = regs;
        this->yield_noreturn();
        break;                  /* will not be reached */
    }

    case INT_PAGEFAULT: {
        // Analyze faulting address and access type.
        uintptr_t addr = rcr2();
        const char* operation = regs->reg_err & PFERR_WRITE
                ? "write" : "read";
        const char* problem = regs->reg_err & PFERR_PRESENT
                ? "protection problem" : "missing page";

        if (!(regs->reg_err & PFERR_USER)) {
            panic("Kernel page fault for %p (%s %s, rip=%p)!\n",
                  addr, operation, problem, regs->reg_rip);
        }
        console_printf(CPOS(24, 0), 0x0C00,
                       "Process %d page fault for %p (%s %s, rip=%p)!\n",
                       pid_, addr, operation, problem, regs->reg_rip);
        this->state_ = proc::broken;
        this->yield();
        break;
    }

    default:
        panic("Unexpected exception %d!\n", regs->reg_intno);
        break;                  /* will not be reached */

    }


    // Return to the current process.
    assert(this->state_ == proc::runnable);
}


// proc::syscall(regs)
//    System call handler.
//
//    The register values from system call time are stored in `regs`.
//    The return value from `proc::syscall()` is returned to the user
//    process in `%rax`.

uintptr_t proc::syscall(regstate* regs) {
    switch (regs->reg_rax) {

    case SYSCALL_PANIC:
        panic(NULL);
        break;                  // will not be reached

    case SYSCALL_GETPID:
        return pid_;

    case SYSCALL_YIELD:
        this->yield();
        return 0;

    case SYSCALL_PAGE_ALLOC: {
        uintptr_t addr = regs->reg_rdi;
        if (addr >= 0x800000000000 || addr & 0xFFF) {
            return -1;
        }
        x86_64_page* pg = kallocpage();
        if (!pg || vmiter(this, addr).map(ka2pa(pg)) < 0) {
            return -1;
        }
        return 0;
    }

    case SYSCALL_PAUSE: {
        sti();
        for (uintptr_t delay = 0; delay < 1000000; ++delay) {
            pause();
        }
        cli();
        return 0;
    }

    case SYSCALL_FORK: {
        return process_fork(this);
    }

    case SYSCALL_MAP_CONSOLE: {
        uintptr_t addr = regs->reg_rdi;
        if (addr > VA_LOWMAX || addr & 0xFFF) {
            return -1;
        }
        int r = vmiter(this, addr).map(ktext2pa(console), PTE_P|PTE_W|PTE_U);
        return 0;
    }

    default:
        // no such system call
        log_printf("%d: no such system call %u\n", pid_, regs->reg_rax);
        return -1;

    }
}


// memshow()
//    Draw a picture of memory (physical and virtual) on the CGA console.
//    Switches to a new process's virtual memory map every 0.25 sec.
//    Uses `console_memviewer()`, a function defined in `k-memviewer.cc`.

void memshow() {
    static unsigned last_ticks = 0;
    static int showing = 1;

    // switch to a new process every 0.25 sec
    if (last_ticks == 0 || ticks - last_ticks >= HZ / 2) {
        last_ticks = ticks;
        ++showing;
    }

    auto irqs = ptable_lock.lock();

    while (showing <= 2*NPROC && !ptable[showing % NPROC]) {
        ++showing;
    }
    showing = showing % NPROC;

    extern void console_memviewer(const proc* vmp);
    console_memviewer(ptable[showing]);

    ptable_lock.unlock(irqs);
}
