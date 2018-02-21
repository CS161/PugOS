#include "kernel.hh"
#include "k-apic.hh"
#include "k-vmiter.hh"

// kernel.cc
//
//    This is the kernel.

volatile unsigned long ticks;   // # timer interrupts so far on CPU 0
int kdisplay;                   // type of display

static void kdisplay_ontick();
static void process_setup(pid_t pid, const char* program_name);


// kernel_start(command)
//    Initialize the hardware and processes and start running. The `command`
//    string is an optional string passed from the boot loader.

void kernel_start(const char* command) {
    assert(read_rbp() % 16 == 0);  // check stack alignment

    init_hardware();
    console_clear();
    kdisplay = KDISPLAY_MEMVIEWER;

    // Set up process descriptors
    for (pid_t i = 0; i < NPROC; i++) {
        ptable[i] = nullptr;
    }

    auto irqs = ptable_lock.lock();
    process_setup(1, "p-init");
    process_setup(2, "p-testwaitpid");
    ptable_lock.unlock(irqs);

    // Switch to the first process
    cpus[0].schedule(nullptr);
}


// process_setup(pid, name)
//    Load application program `name` as process number `pid`.
//    This loads the application's code and data into memory, sets its
//    %rip and %rsp, gives it a stack page, and marks it as runnable.

void process_setup(pid_t pid, const char* name) {
#ifdef CHICKADEE_FIRST_PROCESS
    name = CHICKADEE_FIRST_PROCESS;
#endif

    assert(!ptable[pid]);
    proc* p = ptable[pid] = kalloc_proc();
    x86_64_pagetable* npt = kalloc_pagetable();
    assert(p && npt);
    p->init_user(pid, npt);

    int r = p->load(name);
    assert(r >= 0);
    p->regs_->reg_rsp = MEMSIZE_VIRTUAL;
    x86_64_page* stkpg = kallocpage();
    assert(stkpg);
    r = vmiter(p, MEMSIZE_VIRTUAL - PAGESIZE).map(ka2pa(stkpg));
    p->regs_->reg_rsp -= 8;     // align stack by 16 bytes
    assert(r >= 0);

    r = vmiter(p, ktext2pa(console)).map(ktext2pa(console), PTE_P|PTE_W|PTE_U);
    assert(r >= 0);

    // manage process hierarchy
    p->children_.reset();
    p->ppid_ = 1;
    if (p->pid_ != 1) {
        ptable[1]->children_.push_back(p);
    }

    int cpu = pid % ncpu;
    cpus[cpu].runq_lock_.lock_noirq();
    cpus[cpu].enqueue(p);
    cpus[cpu].runq_lock_.unlock_noirq();
}


void process_exit(proc* p, int status = 0) {
    p->exit_status_ = status;

    // free virtual memory
    for (vmiter vmit(p); vmit.va() < MEMSIZE_VIRTUAL; vmit.next()) {
        if (vmit.user() && vmit.writable() && vmit.pa() != ktext2pa(console)) {
            debug_printf("virtual mem: freeing va %p\n", vmit.va());

            kfree(reinterpret_cast<void*>(pa2ka(vmit.pa())));
        }
    }

    // free pagetables
    debug_printf("freeing l3-1 pagetables:\n");
    for (ptiter ptit(p, 0); ptit.low(); ptit.next()) {
        // debug_printf("\t\tpa=%p\n", ptit.ptp_pa());
        // if (ptit.ptp_pa() % PAGESIZE != 0) {
        debug_printf("FREE PAGETABLE: ");
        debug_printf("pid %d ", p->pid_);
        debug_printf("pa %p ", ptit.ptp_pa());
        debug_printf("va %p\n", ptit.va());
        // }
        kfree(reinterpret_cast<void*>(pa2ka(ptit.ptp_pa())));
    }

    p->state_ = proc::broken;
}


// omae wa mou shindeiru
int process_reap(pid_t pid) {
    auto irqs = ptable_lock.lock();
    proc* p = ptable[pid];
    // manage process hierarchy
    ptable[p->ppid_]->children_.erase(p);
    while (!p->children_.empty()) {
        proc* child = p->children_.pop_front();
        child->ppid_ = 1;
        ptable[1]->children_.push_back(child);
    }

    int status = p->exit_status_;
    kfree(p->pagetable_);
    kfree(p);
    ptable[pid] = nullptr;
    ptable_lock.unlock(irqs);
    return status;
}



// process_fork(ogproc, ogregs)
//    Fork the process ogproc into the first available pid.
static pid_t process_fork(proc* ogproc, regstate* ogregs) {
    // 1. Allocate a new PID.
    pid_t fpid = -1;

    auto irqs = ptable_lock.lock();
    for (pid_t i = 1; i < NPROC; i++) {
        if (!ptable[i] || ptable[i]->state_ == proc::blank) {
            fpid = i;
            break;
        }
    }
    // No free process slot found
    if (fpid < 1) {
        ptable_lock.unlock(irqs);
        return -1;
    }

    // 2. Allocate a struct proc and a page table.
    // 5. Store the new process in the process table.
    proc* fproc = ptable[fpid] = reinterpret_cast<proc*>(kallocpage());
    if (!fproc) {
        ptable_lock.unlock(irqs);
        return -1;
    }
    fproc->pid_ = fpid;
    fproc->state_ = proc::broken;
    fproc->ppid_ = ogproc->pid_;
    fproc->children_.reset();
    ogproc->children_.push_back(fproc);
    ptable_lock.unlock(irqs);


    x86_64_pagetable* fpt = fproc->pagetable_ = kalloc_pagetable();
    if (!fpt) {
        kfree(fproc);
        irqs = ptable_lock.lock();
        ptable[fpid] = nullptr;
        ptable_lock.unlock(irqs);
        return -1;
    }

    // 3. Copy the parent process’s user-accessible memory and map the copies
    // into the new process’s page table.
    for (vmiter source(ogproc); source.low(); source.next()) {
        if (source.user() && source.writable()
                && source.pa() != ktext2pa(console)) {
            void* npage_ka = kallocpage();
            if (npage_ka == nullptr) {
                process_exit(fproc);
                return -1;
            }
            uintptr_t npage_pa = ka2pa(npage_ka);

            memcpy(npage_ka, reinterpret_cast<void*>(pa2ka(source.pa())),
                PAGESIZE);
            if (vmiter(fpt, source.va()).map(npage_pa, source.perm()) < 0) {
                kfree(npage_ka);
                process_exit(fproc);
                return -1;
            }
        }
        else if (source.user()) {
            if (vmiter(fpt, source.va()).map(source.pa(), source.perm()) < 0) {
                process_exit(fproc);
                return -1;
            }
        }
    }

    // Initialize fproc structure (note: sets registers wrong)
    fproc->init_user(fpid, fpt);

    // 4. Initialize the new process’s registers to a copy of the old process’s
    // registers.
    *fproc->regs_ = *ogregs;

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


int canary_value = rand();
// check_corruption(p)
//    Check for data corruption in current cpustate and proc structs by looking
//    at the canary values. If the stack got too big and overwrote data, we will
//    know because the saved canary values will have changed.

static void check_corruption(proc* p) {
    cpustate* c = &cpus[p->pid_ % ncpu];
    assert(p->canary_ == canary_value);
    assert(c->canary_ == canary_value);
}


// seppuku()
//    Die an honorable death

int seppuku() {
    int big[1000];
    for (int i = 0; i < 1000; i++) {
        big[i] = i;
    }
    return big[934];
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

    assert(read_rbp() % 16 == 0);  // check stack alignment

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
            kdisplay_ontick();
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
        error_printf(CPOS(24, 0), 0x0C00,
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
    assert(read_rbp() % 16 == 0);  // check stack alignment

    uintptr_t r = -1;
    switch (regs->reg_rax) {

    case SYSCALL_KDISPLAY:
        if (kdisplay != (int) regs->reg_rdi) {
            console_clear();
        }
        kdisplay = regs->reg_rdi;
        return 0;

    case SYSCALL_PANIC:
        panic(NULL);
        break;                  // will not be reached

    case SYSCALL_GETPID:
        r = pid_;
        break;

    case SYSCALL_YIELD:
        this->yield();
        r = 0;
        break;

    case SYSCALL_PAGE_ALLOC: {
        uintptr_t addr = regs->reg_rdi;
        if (addr >= 0x800000000000 || addr & 0xFFF) {
            break;
        }
        x86_64_page* pg = kallocpage();
        if (!pg || vmiter(this, addr).map(ka2pa(pg)) < 0) {
            break;
        }
        r = 0;
        break;
    }

    case SYSCALL_PAUSE: {
        sti();
        for (uintptr_t delay = 0; delay < 1000000; ++delay) {
            pause();
        }
        cli();
        r = 0;
        break;
    }

    case SYSCALL_FORK: {
        r = process_fork(this, regs);
        break;
    }

    case SYSCALL_EXIT: {
        int status = regs->reg_rdi;
        process_exit(this, status);
        this->yield_noreturn();
    }

    case SYSCALL_MAP_CONSOLE: {
        uintptr_t addr = regs->reg_rdi;
        if (addr > VA_LOWMAX || addr & 0xFFF) {
            break;
        }
        if (vmiter(this, addr).map(ktext2pa(console), PTE_P|PTE_W|PTE_U) < 0) {
            break;
        }
        r = 0;
        break;
    }

    case SYSCALL_MSLEEP: {
        unsigned long end = ticks + (regs->reg_rdi + 9) / 10;
        while ((long) (end - ticks) > 0) {
            this->yield();
        }
        r = 0;
        break;
    }

    case SYSCALL_GETPPID:
        r = ppid_;
        break;

    case SYSCALL_WAITPID: {
        pid_t child_pid = regs->reg_rdi;
        assert(child_pid < NPROC && child_pid >= 0);
        int options = regs->reg_rsi;

        /* Instead of iterating through our process' children list to see if
        the child_pid is one of its own, we check if the child's parent is
        this process to be O(1) instead of O(C) where C is the number of
        children. */
        auto irqs = ptable_lock.lock();
        pid_t parent_of_child = 0;
        if (ptable[child_pid]) {
            parent_of_child = ptable[child_pid]->ppid_;
        }
        ptable_lock.unlock(irqs);

        pid_t to_reap = 0;

        if ((child_pid != 0 && pid_ != parent_of_child) ||
            (child_pid == 0 && children_.empty())) {
            debug_printf("Returning pid %d\n", E_CHILD);
            r = E_CHILD;
            break;
        }
        else {
            do {
                irqs = ptable_lock.lock();

                if (child_pid == 0) {
                    // Finds any broken child
                    for (auto c = children_.front(); c; c = children_.next(c)) {
                        if (c->state_ == proc::broken) {
                            to_reap = c->pid_;
                            debug_printf("To reap: %d\n", to_reap);
                            break;
                        }
                    }
                }

                else {
                    if (ptable[child_pid]->state_ == proc::broken) {
                        to_reap = child_pid;
                    }
                }
                ptable_lock.unlock(irqs);
                if (to_reap || options == W_NOHANG) break;
                this->yield();
            } while (1);

            if (!to_reap && options == W_NOHANG) {
                r = E_AGAIN;
            }
            else {
                r = to_reap;
                debug_printf("Returning pid %d\n", r);
                int exit_status = process_reap(to_reap);
                asm("movl %0, %%ecx;": : "r"(exit_status) : "ecx");
            }
        }
        break;
    }

    case SYSCALL_COMMIT_SEPPUKU: {
        r = seppuku();
        break;
    }

    default:
        // no such system call
        log_printf("%d: no such system call %u\n", pid_, regs->reg_rax);
        break;
    }

    check_corruption(this);
    return r;
}


// memshow()
//    Draw a picture of memory (physical and virtual) on the CGA console.
//    Switches to a new process's virtual memory map every 0.25 sec.
//    Uses `console_memviewer()`, a function defined in `k-memviewer.cc`.

static void memshow() {
    static unsigned last_ticks = 0;
    static int showing = 1;

    // switch to a new process every 0.25 sec
    if (last_ticks == 0 || ticks - last_ticks >= HZ / 2) {
        last_ticks = ticks;
        showing = (showing + 1) % NPROC;
    }

    auto irqs = ptable_lock.lock();

    int search = 0;
    while ((!ptable[showing]
            || !ptable[showing]->pagetable_
            || ptable[showing]->pagetable_ == early_pagetable)
           && search < NPROC) {
        showing = (showing + 1) % NPROC;
        ++search;
    }

    extern void console_memviewer(const proc* vmp);
    console_memviewer(ptable[showing]);

    ptable_lock.unlock(irqs);
}


// kdisplay_ontick()
//    Shows the currently-configured kdisplay. Called once every tick
//    (every 0.01 sec) by CPU 0.

void kdisplay_ontick() {
    if (kdisplay == KDISPLAY_MEMVIEWER) {
        memshow();
    }
}
