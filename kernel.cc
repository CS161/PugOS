#include "kernel.hh"
#include "k-apic.hh"
#include "k-devices.hh"
#include "k-vmiter.hh"

// kernel.cc
//
//    This is the kernel.

volatile unsigned long ticks;   // # timer interrupts so far on CPU 0
timing_wheel sleep_wheel;       // timer wheel to wake on ticks
int kdisplay;                   // type of display

static wait_queue waitpid_wq;   // waitqueue for sys_waitpid

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
    process_setup(1, "init");
    const char* pname;
#ifdef CHICKADEE_FIRST_PROCESS
    // make run-NAMEHERE
    pname = CHICKADEE_FIRST_PROCESS;
#else
    // manual entry
    pname = "testzombie";
#endif
    process_setup(2, pname);
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
    proc* p = ptable[pid] = kalloc_proc();
    x86_64_pagetable* npt = kalloc_pagetable();
    assert(p && npt);

    // initial fdtable setup
    p->fdtable_ = knew<fdtable>();
    assert(p->fdtable_);
    // fds 0, 1, and 2 all hooked up to keyboard/console
    file* f = p->fdtable_->fds_[0] = p->fdtable_->fds_[1] =
              p->fdtable_->fds_[2] = knew<file>();
    assert(f);
    f->type_ = file::stream;
    f->readable_ = true;
    f->writeable_ = true;
    f->vnode_ = knew<vn_keyboard_console>();
    assert(f->vnode_);
    f->refs_ = 3;

    p->init_user(pid, npt);

    int r = p->load(name);
    assert(r >= 0 && "probably a bad process name");
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

    int cpu = p->cpu_ = pid % ncpu;
    cpus[cpu].runq_lock_.lock_noirq();
    debug_printf("process_setup enqueueing pid %d\n", p->pid_);
    cpus[cpu].enqueue(p);
    cpus[cpu].runq_lock_.unlock_noirq();
}


void process_exit(proc* p, int status = 0) {
    p->exit_status_ = status;

    // free virtual memory
    for (vmiter vmit(p); vmit.va() < MEMSIZE_VIRTUAL; vmit.next()) {
        if (vmit.user() && vmit.writable() && vmit.pa() != ktext2pa(console)) {
            kfree(reinterpret_cast<void*>(pa2ka(vmit.pa())));
            int r = vmiter(p->pagetable_, vmit.va()).map(0x0);
            assert(r >= 0);
        }
    }

    // free pagetables
    for (ptiter ptit(p, 0); ptit.low(); ptit.next()) {
        kfree(reinterpret_cast<void*>(pa2ka(ptit.ptp_pa())));
    }

    // free file descriptor table
    kdelete(p->fdtable_);

    // interrupt parent
    auto irqs = ptable_lock.lock();
    debug_printf("[%d] process_exit interrupting parent %d\n",
                 p->pid_, p->ppid_);
    auto daddy = ptable[p->ppid_];
    if (daddy && daddy->state_ == proc::blocked) {
        daddy->interrupted_ = true;
        daddy->wake();
    }

    // manage process hierarchy
    while (!p->children_.empty()) {
        proc* child = p->children_.pop_front();
        child->ppid_ = 1;
        ptable[1]->children_.push_back(child);
    }

    debug_printf("[%d] process_exit waking waitpid_wq\n", p->pid_);
    ptable_lock.unlock(irqs);
    waitpid_wq.wake_all();

    p->state_ = proc::broken;
}


// omae wa mou shindeiru
int process_reap(pid_t pid) {
    auto irqs = ptable_lock.lock();
    proc* p = ptable[pid];

    // manage process hierarchy
    ptable[p->ppid_]->children_.erase(p);

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

    // allocate proc, store in ptable
    proc* fproc = ptable[fpid] = kalloc_proc();
    if (!fproc) {
        ptable_lock.unlock(irqs);
        return -1;
    }

    // allocate fdtable
    fproc->fdtable_ = knew<fdtable>();
    if (!fproc->fdtable_) {
        kdelete(fproc);
        ptable[fpid] = nullptr;
        ptable_lock.unlock(irqs);
        return -1;
    }

    // misc initialization
    fproc->pid_ = fpid;
    fproc->state_ = proc::broken;
    fproc->ppid_ = ogproc->pid_;
    fproc->children_.reset();
    ogproc->children_.push_back(fproc);
    ptable_lock.unlock(irqs);

    // clone ogproc's fdtable
    auto fdt_irqs = ogproc->fdtable_->lock_.lock();
    fproc->fdtable_ = ogproc->fdtable_;
    for (unsigned i = 0; fproc->fdtable_->fds_[i] && i < NFDS; i++) {
        fproc->fdtable_->fds_[i]->refs_++;
    }
    ogproc->fdtable_->lock_.unlock(fdt_irqs);

    x86_64_pagetable* fpt = fproc->pagetable_ = kalloc_pagetable();
    if (!fpt) {
        kdelete(fproc->fdtable_);
        kdelete(fproc);
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

    fproc->regs_->reg_rax = 0;

    // 6. Enqueue the new process on some CPU’s run queue.
    int cpu = fproc->cpu_ = fpid % ncpu;
    cpus[cpu].runq_lock_.lock_noirq();
    debug_printf("[%d] process_fork enqueueing pid %d\n",
                 ogproc->pid_, fproc->pid_);
    cpus[cpu].enqueue(fproc);
    cpus[cpu].runq_lock_.unlock_noirq();

    // 7. Arrange for the new PID to be returned to the parent process and 0 to
    // be returned to the child process.
    return fpid;
}


int canary_value = rand();
// check_corruption(p)
//    Check for data corruption in current cpustate and proc structs by looking
//    at the canary values. If the stack got too big and overwrote data, we will
//    know because the saved canary values will have changed.

void check_corruption(proc* p) {
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
    if (pid_ > 0)
        debug_printf("[%d] exception %d\n", pid_, regs->reg_intno);

    assert(read_rbp() % 16 == 0);  // check stack alignment

    // Show the current cursor location.
    console_show_cursor(cursorpos);


    // Actually handle the exception.
    switch (regs->reg_intno) {

    case INT_IRQ + IRQ_TIMER: {
        cpustate* cpu = this_cpu();
        if (cpu->index_ == 0) {
            ++ticks;
            if (!sleep_wheel.wqs_[ticks % WHEEL_SPOKES].q_.empty()) {
                sleep_wheel.wqs_[ticks % WHEEL_SPOKES].wake_all();
            }
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

    case INT_IRQ + IRQ_KEYBOARD:
        keyboardstate::get().handle_interrupt();
        break;

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
        if (addr >= VA_LOWEND || addr & 0xFFF) {
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
        interrupted_ = false;
        unsigned long end = ticks + (regs->reg_rdi + 9) / 10;
        debug_printf("[%d] sys_msleep(%d)\n", pid_, regs->reg_rdi);
        waiter w(this);
        auto wq = &sleep_wheel.wqs_[end % WHEEL_SPOKES];
        while (true) {
            w.prepare(wq);
            if ((long) (end - ticks) <= 0 || interrupted_)
                break;
            w.block();
        }
        w.clear();

        debug_printf("[-] resumes: %d\n", resumes);

        debug_printf("[%d] sys_msleep%sinterrupted\n", pid_,
                     interrupted_ ? " " : " not ");
        r = interrupted_ ? E_INTR : 0;
        break;
    }

    case SYSCALL_GETPPID:
        r = ppid_;
        break;

    case SYSCALL_WAITPID: {
        pid_t child_pid = regs->reg_rdi;
        assert(child_pid < NPROC && child_pid >= 0);
        int options = regs->reg_rsi;

        auto irqs = ptable_lock.lock();
        debug_printf("[%d] sys_waitpid on child pid %d; options %s W_NOHANG"
                     "\n", pid_, child_pid, options == W_NOHANG ? "=" : "!=");

        pid_t parent_of_child = 0;
        if (ptable[child_pid]) {
            parent_of_child = ptable[child_pid]->ppid_;
        }
        ptable_lock.unlock(irqs);

        pid_t to_reap = 0;
        if ((child_pid != 0 && pid_ != parent_of_child)
            || (child_pid == 0 && children_.empty())) {
            r = E_CHILD;
            debug_printf("[%d] sys_waitpid returning E_CHILD r=%d\n", pid_, r);
            break;
        }
        else {
            waiter w(this);
            while (true) {
                irqs = ptable_lock.lock();
                debug_printf("[%d] sys_waitpid preparing\n", pid_);
                w.prepare(&waitpid_wq);

                // wait for any child
                if (child_pid == 0) {
                    for (auto p = children_.front(); p; p = children_.next(p)) {
                        if (p->state_ == proc::broken) {
                            to_reap = p->pid_;
                            break;
                        }
                    }
                }
                // wait for a child (child_pid)
                else {
                    if (ptable[child_pid]->state_ == proc::broken) {
                        to_reap = ptable[child_pid]->pid_;
                    }
                }

                ptable_lock.unlock(irqs);
                if (to_reap || options == W_NOHANG)
                    break;
                
                w.block();
            }
            w.clear();
        }
        debug_printf("[%d] sys_waitpid: to_reap pid = %d\n", pid_, to_reap);

        if (!to_reap && options == W_NOHANG && r != (uintptr_t) E_CHILD) {
            r = E_AGAIN;
            debug_printf("[%d] sys_waitpid returning E_AGAIN r=%d\n", pid_, r);
        }
        else {
            debug_printf("[%d] sys_waitpid returning pid %d\n",
                pid_, r);
            int exit_status = process_reap(to_reap);
            asm("movl %0, %%ecx;": : "r" (exit_status) : "ecx");
            r = to_reap;
        }

        break;
    }

    case SYSCALL_COMMIT_SEPPUKU: {
        r = seppuku();
        break;
    }

    case SYSCALL_READ:
    case SYSCALL_WRITE: {
        int fd = regs->reg_rdi;
        uintptr_t addr = regs->reg_rsi;
        size_t sz = regs->reg_rdx;

        auto irqs = fdtable_->lock_.lock();
        debug_printf("[%d] sys_%s on fd %d", pid_,
            regs->reg_rax == SYSCALL_READ ? "read" : "write", fd);
        if (fd < 0 || fd >= NFDS || fdtable_->fds_[fd] == nullptr) {
            fdtable_->lock_.unlock(irqs);
            r = E_BADF;
            debug_printf("; returning E_BADF\n");
            break;
        }

        file* f = fdtable_->fds_[fd];
        debug_printf("; mode r? %s; w? %s\n",
            f->readable_ ? "yes" : "no", f->writeable_ ? "yes" : "no");
        if ((regs->reg_rax == SYSCALL_READ && !f->readable_)
            || (regs->reg_rax == SYSCALL_WRITE && !f->writeable_)) {
            fdtable_->lock_.unlock(irqs);
            r = E_BADF;
            debug_printf("returning E_PERM\n");
            break;
        }

        if (sz == 0) {
            fdtable_->lock_.unlock(irqs);
            r = 0;
            break;
        }

        // verify inputs are valid
        auto mem_flags = PTE_P | PTE_U;
        if (regs->reg_rax == SYSCALL_READ) {
            mem_flags |= PTE_W;
        }
        if (addr + sz > VA_LOWEND
            || addr > VA_HIGHMAX - sz
            || !vmiter(this, addr).check_range(sz, mem_flags)) {
            fdtable_->lock_.unlock(irqs);
            r = E_FAULT;
            debug_printf("returning E_FAULT\n");
            break;
        }

        f->lock_.lock_noirq();
        fdtable_->lock_.unlock_noirq();
        f->refs_++;
        f->lock_.unlock(irqs);

        if (regs->reg_rax == SYSCALL_READ) {
            r = f->vnode_->read(addr, sz);
        }
        else {
            r = f->vnode_->write(addr, sz);
        }

        irqs = f->lock_.lock();
        f->deref();
        f->lock_.unlock(irqs);

        debug_printf("[%d] sys_%s %d bytes\n", pid_,
            regs->reg_rax == SYSCALL_READ ? "read read" : "write wrote", r);

        break;
    }

    case SYSCALL_CLOSE: {
        int fd = regs->reg_rdi;

        debug_printf("[%d] sys_close(%d)\n", pid_, fd);
        auto irqs = fdtable_->lock_.lock();
        if (fd < 0 || fd >= NFDS || fdtable_->fds_[fd] == nullptr) {
            fdtable_->lock_.unlock(irqs);
            debug_printf("returning E_BADF\n");
            r = E_BADF;
            break;
        }

        fdtable_->fds_[fd]->deref();
        fdtable_->fds_[fd] = nullptr;
        fdtable_->lock_.unlock(irqs);

        r = 0;
        break;
    }

    case SYSCALL_DUP2: {
        int oldfd = regs->reg_rdi;
        int newfd = regs->reg_rsi;

        debug_printf("[%d] sys_dup2(%d, %d)\n", pid_, oldfd, newfd);
        auto irqs = fdtable_->lock_.lock();
        if (oldfd < 0 || newfd < 0 || oldfd >= NFDS || newfd >= NFDS
            || fdtable_->fds_[oldfd] == nullptr) {
            fdtable_->lock_.unlock(irqs);
            debug_printf("returning E_BADF\n");
            r = E_BADF;
            break;
        }

        fdtable_->fds_[newfd] && fdtable_->fds_[newfd]->deref();
        fdtable_->fds_[newfd] = fdtable_->fds_[oldfd];
        fdtable_->fds_[oldfd]->refs_++;
        fdtable_->lock_.unlock(irqs);

        r = 0;
        break;
    }

    default:
        // no such system call
        log_printf("%d: no such system call %u\n", pid_, regs->reg_rax);
        r = E_NOSYS;
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

    extern void console_memviewer(proc* vmp);
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
