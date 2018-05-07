#include "kernel.hh"
#include "k-apic.hh"
#include "k-chkfs.hh"
#include "k-chkfsiter.hh"
#include "k-devices.hh"
#include "k-gfx.hh"
#include "k-vfs.hh"
#include "k-vmiter.hh"

// kernel.cc
//
//    This is the kernel.

volatile unsigned long ticks;   // # timer interrupts so far on CPU 0
timing_wheel sleep_wheel;       // timer wheel to wake on ticks
int kdisplay;                   // type of display

wait_queue waitpid_wq;   // waitqueue for sys_waitpid

static void kdisplay_ontick();
static void process_setup(pid_t pid, const char* program_name);


// kernel_start(command)
//    Initialize the hardware and processes and start running. The `command`
//    string is an optional string passed from the boot loader.

void kernel_start(const char* command) {
    assert(read_rbp() % 16 == 0);  // check stack alignment

#ifdef GFX
    log_printf("Graphics enabled! No console output will be visible.\n");
#endif

    init_hardware();
    console_clear();
    kdisplay = KDISPLAY_MEMVIEWER;

    // Set up process descriptors
    for (pid_t i = 0; i < NPROC; i++) {
        ptable[i] = nullptr;
    }

    auto irqs = ptable_lock.lock();
    const char* proc_name;
#ifdef CHICKADEE_FIRST_PROCESS
    // make run-proc_name
    proc_name = CHICKADEE_FIRST_PROCESS;
#else
    // manual entry
    proc_name = "testzombie";
#endif

    // old tests that want to run on pid 1
    if (!strcmp(proc_name, "allocexit")
          || !strcmp(proc_name, "allocator")) {
        process_setup(1, proc_name);
    }
    // newer programs that expect an init process
    else {
        process_setup(1, "init");
        process_setup(2, proc_name);
    }
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
    assert(!true_ptable[pid]);
    proc* p = ptable[pid] = true_ptable[pid] = kalloc_proc();
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
    f->vnode_ = knew<vnode_kbc>();
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
        true_ptable[1]->children_.push_back(p);
    }

    int cpu = p->cpu_ = pid % ncpu;
    cpus[cpu].runq_lock_.lock_noirq();
    debug_printf("process_setup enqueueing pid %d\n", p->pid_);
    cpus[cpu].enqueue(p);
    cpus[cpu].runq_lock_.unlock_noirq();
}


unsigned active_threads(proc* p, bool lock = true) {
    irqstate irqs;
    if (lock) {
        irqs = ptable_lock.lock();
    }

    unsigned r = 0;
    for (auto i = 0; i < NPROC; ++i) {
        if (ptable[i] && ptable[i]->true_pid_ == p->true_pid_ &&
                (ptable[i]->state_ == proc::runnable ||
                 ptable[i]->state_ == proc::blocked)) {
            ++r;
        }
    }

    if (lock) {
        ptable_lock.unlock(irqs);
    }
    return r;
}


void display_proc(proc* p) {
    if (p) {
        debug_printf("ptable[%d] = true_pid_ %d, exiting_ %d, state %s\n",
            p->pid_, p->true_pid_, p->exiting_, state_string(p));
    }
}


void process_exit(proc* p, int status = 0) {
    p->exit_status_ = status;
    debug_printf("[%d] process_exit\n", current()->pid_);

    auto irqs = ptable_lock.lock();
    if (active_threads(p, false) > 1) {
        // mark all threads as exiting
        debug_printf("[%d] process_exit killing threads...\n", current()->pid_);
        for (auto i = 0; i < NPROC; ++i) {
            if (ptable[i]
                    && ptable[i]->true_pid_ == p->true_pid_
                    && ptable[i]->state_ != proc::broken
                    && ptable[i]->pid_ != p->pid_){
                debug_printf("[%d] process_exit killing %d\n",
                    p->pid_, ptable[i]->pid_);
                ptable[i]->exiting_ = true;
            }

            if (ptable[i]) {
                display_proc(ptable[i]);
            }
        }

        // block until all threads exit
        waiter(current()).block_until(waitpid_wq, [&] () {
                for (auto i = 0; i < NPROC; ++i) {
                    display_proc(ptable[i]);

                    if (ptable[i]
                            && ptable[i]->true_pid_ == p->true_pid_
                            && ptable[i]->pid_ != p->pid_
                            && ptable[i]->state_ != proc::broken){
                        if (!ptable[i]->exiting_) {
                            ptable[i]->exiting_ = true;
                        }
                        if (ptable[i]->state_ == proc::blocked) {
                            ptable[i]->wake();
                            waitpid_wq.wake_all();
                        }
                        return false;
                    }
                }
                return true;
            }, ptable_lock, irqs);

        debug_printf("[%d] process_exit finished waiting for threads, "
            "exiting\n", p->pid_);
    }
    ptable_lock.unlock(irqs);

    // free file descriptors
    for (unsigned i = 0; i < NFDS; i++) {
        if (p->fdtable_->fds_[i]) {
            p->fdtable_->fds_[i]->deref();
        }
    }

    // interrupt parent
    irqs = ptable_lock.lock();
    // debug_printf("[%d] process_exit interrupting parent %d\n",
    //              p->pid_, p->ppid_);
    auto daddy = ptable[p->ppid_];
    if (daddy && daddy->state_ == proc::blocked) {
        daddy->interrupted_ = true;
        daddy->wake();
    }

    // re-parent children
    while (!p->children_.empty()) {
        proc* child = p->children_.pop_front();
        child->ppid_ = 1;
        ptable[1]->children_.push_back(child);
    }

    // debug_printf("[%d] process_exit waking waitpid_wq\n", p->pid_);
    ptable_lock.unlock(irqs);
    waitpid_wq.wake_all();

    p->state_ = proc::broken;
}


// nuke_pagetable(pt)
//    Wipes all memory associated with pagetable pt. MUST be called on an L4 pt
void nuke_pagetable(x86_64_pagetable* pt) {
    // free virtual memory
    for (vmiter vmit(pt); vmit.va() < MEMSIZE_VIRTUAL; vmit.next()) {
        if (vmit.user() && vmit.writable() && vmit.pa() != ktext2pa(console)) {
            kfree(reinterpret_cast<void*>(pa2ka(vmit.pa())));
            assert(vmiter(pt, vmit.va()).map(0x0) >= 0);
        }
    }

    // free L3-L1 pagetables
    for (ptiter ptit(pt, 0); ptit.low(); ptit.next()) {
        kfree(reinterpret_cast<void*>(pa2ka(ptit.ptp_pa())));
    }

    // free L4 pagetable
    kdelete(pt);
}


// omae wa mou shindeiru
int process_reap(pid_t pid) {
    auto irqs = ptable_lock.lock();
    proc* p = ptable[pid];
    // erase proc from parent's children
    true_ptable[p->ppid_]->children_.erase(p);

    unsigned nthr = 0;
    for (pid_t i = 0; i < NPROC; ++i) {
        if (ptable[i]
                && ptable[i]->true_pid_ == p->true_pid_
                && (ptable[i]->state_ == proc::runnable
                    || ptable[i]->state_ == proc::blocked
                    || ptable[i]->state_ == proc::broken)) {
            ++nthr;
        }
    }

    if (nthr == 1) {
        // wipe everything else
        kdelete(p->fdtable_);
        nuke_pagetable(p->pagetable_);
    }

    int exit_status = p->exit_status_;
    kfree(p);
    ptable[pid] = true_ptable[pid] = nullptr;
    ptable_lock.unlock(irqs);
    debug_printf("[%d] reaped pid %d, %d active threads\n",
        current()->pid_, pid, nthr);
    waitpid_wq.wake_all();
    return exit_status;
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
        debug_printf("[%d] sys_fork error no free pid\n");
        ptable_lock.unlock(irqs);
        return E_MFILE;
    }

    debug_printf("[%d] forking into pid %d\n", ogproc->pid_, fpid);

    // allocate proc, store in ptable
    proc* fproc = ptable[fpid] = true_ptable[fpid] = kalloc_proc();
    if (!fproc) {
        ptable_lock.unlock(irqs);
        return E_NOMEM;
    }
    fproc->state_ = proc::broken;
    ptable_lock.unlock(irqs);

    // allocate pagetable
    x86_64_pagetable* fpt = fproc->pagetable_ = kalloc_pagetable();
    if (!fpt) {
        irqs = ptable_lock.lock();
        ptable[fpid] = true_ptable[fpid] = nullptr;
        kdelete(fproc);
        ptable_lock.unlock(irqs);
        return E_NOMEM;
    }

    // initialize proc data
    fproc->init_user(fpid, fpt);    // note: sets registers wrong

    // allocate fdtable
    fproc->fdtable_ = knew<fdtable>();
    if (!fproc->fdtable_) {
        irqs = ptable_lock.lock();
        kdelete(fpt);
        kdelete(fproc);
        ptable[fpid] = nullptr;
        ptable_lock.unlock(irqs);
        return E_NOMEM;
    }

    // clone ogproc's fdtable
    auto fdt_irqs = ogproc->fdtable_->lock_.lock();
    for (unsigned i = 0; i < NFDS && ogproc->fdtable_->fds_[i]; i++) {
        auto f = fproc->fdtable_->fds_[i] = ogproc->fdtable_->fds_[i];
        f->lock_.lock_noirq();
        f->refs_++;
        f->vnode_->lock_.lock_noirq();
        f->lock_.unlock_noirq();
        f->vnode_->refs_++;
        f->vnode_->lock_.unlock_noirq();
    }
    ogproc->fdtable_->lock_.unlock(fdt_irqs);

    // set up process hierarchy
    fproc->ppid_ = ogproc->pid_;
    ogproc->children_.push_back(fproc);


    // 3. Copy the parent process’s user-accessible memory and map the copies
    // into the new process’s page table.
    for (vmiter source(ogproc); source.low(); source.next()) {
        if (source.user() && source.writable()
                && source.pa() != ktext2pa(console)) {
            void* npage_ka = kallocpage();
            if (npage_ka == nullptr) {
                process_reap(fpid);
                return E_NOMEM;
            }
            uintptr_t npage_pa = ka2pa(npage_ka);

            memcpy(npage_ka, reinterpret_cast<void*>(pa2ka(source.pa())),
                PAGESIZE);
            if (vmiter(fpt, source.va()).map(npage_pa, source.perm()) < 0) {
                kfree(npage_ka);
                process_reap(fpid);
                return E_NOMEM;
            }
        }
        else if (source.user()) {
            if (vmiter(fpt, source.va()).map(source.pa(), source.perm()) < 0) {
                process_reap(fpid);
                return E_NOMEM;
            }
        }
    }

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


// validate_memory(addr, sz, perms)
//    Returns true if [addr, addr + sz) all has permissions = perms and is in
//    valid address space.

bool validate_memory(uintptr_t addr, size_t sz = 0, int perms = 0) {
    return !(!addr
             || addr + sz > VA_LOWEND
             || addr > VA_HIGHMAX - sz
             || (sz > 0 ? !vmiter(current(), addr).check_range(sz, perms)
                        : false));
}

template <typename T>
bool validate_memory(T* addr, size_t sz = 0, int perms = 0) {
    return validate_memory(reinterpret_cast<uintptr_t>(addr), sz, perms);
}


// validate_fd(fd)
//    Returns 0 if it's valid, or the error number if not
//    MUST BE CALLED WITH fdtable_.lock HELD

int validate_fd(int fd, fdtable* fdt) {
    if (fd < 0 || fd >= NFDS || fdt->fds_[fd] == nullptr) {
        return E_BADF;
    }
    else {
        return 0;
    }
}


// check_string_termination(str, max_len)
//    Checks if a string terminates within max_len characters. Returns <0 if not
//    or the length of the string if it does.

int check_string_termination(const char* str, int max_len) {
    if (!str)
        return E_FAULT;
    for (int i = 0; i < max_len; i++) {
        if (!validate_memory(&str[i], 1, PTE_P | PTE_U))
            return E_FAULT;
        else if (str[i] == '\0')
            return i;
    }
    return E_INVAL;
}


int get_proc_slot(proc** ptab) {
    for (int i = 1; i < NPROC; ++i) {
        if (!ptab[i] || ptab[i]->state_ == proc::blank) {
            return i;
        }
    }
    return -1;
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
        console_printf("\n");
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
        if (sata_disk && regs->reg_intno == INT_IRQ + sata_disk->irq_) {
            sata_disk->handle_interrupt();
        } else {
            // doom debugging
            log_printf("FATAL ERROR %%rip = %p\n", regs->reg_rip);
            panic("Unexpected exception %d!\n", regs->reg_intno);
        }
        break;                  /* will not be reached */

    }


    // Return to the current process.
    // If exception arrived in user mode, the process must be runnable.
    assert((regs->reg_cs & 3) == 0 || this->state_ == proc::runnable);
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
        r = true_pid_;
        break;

    case SYSCALL_YIELD:
        this->yield();
        r = 0;
        break;

    case SYSCALL_PAGE_ALLOC: {
        uintptr_t addr = regs->reg_rdi;
        if (addr >= VA_LOWEND || addr & 0xFFF) {
            r = E_INVAL;
            break;
        }
        x86_64_page* pg = kallocpage();
        if (!pg || vmiter(this, addr).map(ka2pa(pg)) < 0) {
            r = E_NOMEM;
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

    case SYSCALL_MAP_SCREEN: {
        uintptr_t addr = regs->reg_rdi;

        int vm_r;
        size_t off;
        for (off = 0; off < SCREEN_MEMSIZE; off += PAGESIZE) {
            vm_r = vmiter(this, addr + off).map(SCREEN_MEMBASE + off,
                                                PTE_P|PTE_W|PTE_U);
            assert(vm_r >= 0);
        }
        debug_printf("[%d] sys_map_screen -> %p to ~%p\n",
            pid_, addr, addr + off);
        r = 0;
        break;
    }

    case SYSCALL_MSLEEP: {
        interrupted_ = false;
        unsigned long end = ticks + (regs->reg_rdi + 9) / 10;
        // debug_printf("[%d] sys_msleep(%d)\n", pid_, regs->reg_rdi);
        waiter w(this);
        auto wq = &sleep_wheel.wqs_[end % WHEEL_SPOKES];
        while (true) {
            w.prepare(wq);
            // debug_printf("[%d] sys_msleep woken\n", pid_);
            if ((long) (end - ticks) <= 0 || interrupted_)
                break;
            // debug_printf("[%d] sys_msleep blocking\n", pid_);
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
        if (true_pid_ > 1) {
            debug_printf("[%d] sys_waitpid on child pid %d; options %s W_NOHANG"
                       "\n", pid_, child_pid, options == W_NOHANG ? "=" : "!=");
        }

        pid_t parent_of_child = 0;
        if (ptable[child_pid]) {
            parent_of_child = ptable[child_pid]->ppid_;
        }
        ptable_lock.unlock(irqs);

        pid_t to_reap = 0;
        int exit_status = 0;
        if ((child_pid != 0 && pid_ != parent_of_child)
            || (child_pid == 0 && children_.empty())) {
            r = E_CHILD;
            if (true_pid_ > 1) {
                debug_printf("[%d] sys_waitpid returning E_CHILD r=%d\n",
                    pid_, r);
            }
            break;
        }
        else {
            waiter w(this);
            while (true) {
                irqs = ptable_lock.lock();
                if (true_pid_ > 1) {
                    debug_printf("[%d] sys_waitpid preparing\n", pid_);
                }
                w.prepare(&waitpid_wq);

                // wait for any child
                if (child_pid == 0) {
                    for (auto p = children_.front(); p; p = children_.next(p)) {
                        if (p->state_ == proc::broken) {
                            to_reap = p->true_pid_;
                            ptable_lock.unlock(irqs);
                            auto s = process_reap(p->pid_);
                            irqs = ptable_lock.lock();
                            if (s != 0) {
                                exit_status = s;
                            }
                            if (true_pid_ > 1) {
                                debug_printf("[%d] sys_waitpid reaped tid %d, "
                                    "exit_status %d\n", pid_, p->pid_, s);
                            }
                            break;
                        }
                    }
                }
                // wait for a child (child_pid)
                else {
                    for (pid_t i = 0; i < NPROC; ++i) {

                        if (ptable[i]
                                && ptable[i]->true_pid_ == child_pid
                                && ptable[i]->state_ == proc::broken) {
                            to_reap = ptable[i]->true_pid_;
                            ptable_lock.unlock(irqs);
                            auto s = process_reap(i);
                            irqs = ptable_lock.lock();
                            if (s != 0) {
                                exit_status = s;
                            }
                            if (true_pid_ > 1) {
                                debug_printf("[%d] sys_waitpid reaped tid %d, "
                                    "exit_status %d\n", pid_, i, s);
                            }
                        }
                    }
                }

                bool no_threads_left = true;
                for (pid_t i = 0; i < NPROC; ++i) {
                    if (ptable[i] && ptable[i]->true_pid_ == child_pid) {
                        no_threads_left = false;
                    }
                }

                if (true_pid_ > 1) {
                    debug_printf("[%d] sys_waitpid to_reap=%d, %sthreads left"
                        "\n", pid_, to_reap, no_threads_left ? "no " : "");
                }

                ptable_lock.unlock(irqs);
                if ((to_reap && no_threads_left) || options == W_NOHANG)
                    break;

                if (true_pid_ > 1) {
                    debug_printf("[%d] sys_waitpid blocking\n", pid_);
                }
                w.block();
            }
            w.clear();
        }

        if (true_pid_ > 1) {
            debug_printf("[%d] sys_waitpid pid to_reap=%d\n", pid_, to_reap);
        }

        if (!to_reap && options == W_NOHANG && r != (uintptr_t) E_CHILD) {
            r = E_AGAIN;
            if (true_pid_ > 1) {
                debug_printf("[%d] sys_waitpid returning E_AGAIN r=%d\n", pid_,
                    r);
            }
        }
        else {
            if (true_pid_ > 1) {
                debug_printf("[%d] sys_waitpid exit_status=%d\n",
                    pid_, exit_status);
            }
            asm("movl %0, %%ecx;": : "r" (exit_status) : "ecx");
            r = to_reap;
        }

        break;
    }

    // case SYSCALL_COMMIT_SEPPUKU: {
    //     r = seppuku();
    //     break;
    // }

    // DEBUG ONLY - could probably be used to crash the kernel
    case SYSCALL_LOG_PRINTF: {
        const char* format = reinterpret_cast<const char*>(regs->reg_rdi);
        va_list* args = reinterpret_cast<va_list*>(regs->reg_rsi);
        log_vprintf(format, *args);
        r = 0;
        break;
    }

    case SYSCALL_GETTICKS: {
        r = ticks;
        break;
    }

    case SYSCALL_READ:
    case SYSCALL_WRITE: {
        int fd = regs->reg_rdi;
        uintptr_t addr = regs->reg_rsi;
        size_t sz = regs->reg_rdx;

        auto irqs = fdtable_->lock_.lock();
        // debug_printf("[%d] sys_%s on fd %d", pid_,
        //     regs->reg_rax == SYSCALL_READ ? "read" : "write", fd);
        if (fd < 0 || fd >= NFDS || fdtable_->fds_[fd] == nullptr) {
            fdtable_->lock_.unlock(irqs);
            r = E_BADF;
            // debug_printf("; returning E_BADF\n");
            break;
        }

        file* f = fdtable_->fds_[fd];
        // debug_printf("; mode r? %s; w? %s\n",
        //     f->readable_ ? "yes" : "no", f->writeable_ ? "yes" : "no");
        if ((regs->reg_rax == SYSCALL_READ && !f->readable_)
              || (regs->reg_rax == SYSCALL_WRITE && !f->writeable_)) {
            fdtable_->lock_.unlock(irqs);
            r = E_BADF;
            // debug_printf("\t...returning E_BADF=%d\n", r);
            break;
        }

        // If size is 0 do nothing
        if (sz == 0) {
            fdtable_->lock_.unlock(irqs);
            r = 0;
            break;
        }

        // Check for valid memory ranges and permissions
        auto perms = PTE_P | PTE_U;
        if (regs->reg_rax == SYSCALL_READ) {
            perms |= PTE_W;
        }
        if (addr + sz > VA_LOWEND ||
            addr > VA_HIGHMAX - sz ||
            !vmiter(pagetable_, addr).check_range(sz, perms))
        {
            fdtable_->lock_.unlock(irqs);
            r = E_FAULT;
            break;
        }

        f->lock_.lock_noirq();
        fdtable_->lock_.unlock_noirq();
        f->refs_++;
        f->lock_.unlock(irqs);

        vnode* v = f->vnode_;
        if (regs->reg_rax == SYSCALL_READ) {
            r = v->read(addr, sz, f->off_);
        }
        else {
            r = v->write(addr, sz, f->off_);
        }

        f->deref();

        // debug_printf("[%d] sys_%s %d bytes\n", pid_,
        //     regs->reg_rax == SYSCALL_READ ? "read read" : "write wrote", r);

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

        if (fdtable_->fds_[newfd])
            fdtable_->fds_[newfd]->deref();
        fdtable_->fds_[newfd] = fdtable_->fds_[oldfd];
        fdtable_->fds_[oldfd]->refs_++;
        fdtable_->lock_.unlock(irqs);

        r = 0;
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

        debug_printf("[%d] sys_close completed\n", pid_);
        r = 0;
        break;
    }

    case SYSCALL_OPEN: {
        const char* path = reinterpret_cast<const char*>(regs->reg_rdi);
        int flags = regs->reg_rsi;
        bool created = false;

        auto path_sz = check_string_termination(path, memfile::namesize);
        if (path_sz < 0) {
            r = path_sz;
            break;
        }

        debug_printf("[%d] sys_open('%s')\n", pid_, path);

        auto& fs = chkfsstate::get();

        // read directory to find file inode number
        auto dirino = fs.get_inode(1);
        assert(dirino);
        dirino->lock_read();

        auto ino = fs.lookup_inode(dirino, path);

        dirino->unlock_read();

        if (!ino) {
            if (flags & OF_CREATE && flags & OF_WRITE) {
                // create new empty file with name
                // allocate an inode
                size_t ino_num = fs.find_empty_inode();
                ino = fs.get_inode(ino_num);
                assert(ino);
                ino->lock_write();
                // setup inode metadata
                ino->type = chickadeefs::type_regular;
                ino->size = 0;
                ino->nlink = 1;

                // allocate and map data block
                chkfs_fileiter it(ino);
                size_t data_bn = fs.allocate_block();
                it.map(data_bn);
                ino->unlock_write();

                // setup direntry for inode
                dirino->lock_write();
                chickadeefs::dirent* dirent = fs.find_empty_direntry(dirino);
                memcpy(dirent->name, path, path_sz);
                dirent->inum = ino_num;
                dirino->unlock_write();

                fs.put_inode(ino);

                created = true;
            }
            else {
                debug_printf("[%d] sys_open couldn't find file %s\n",
                    pid_, path);
                fs.put_inode(dirino);
                r = E_NOENT;
                break;
            }
        }
        fs.put_inode(dirino);

        int fd = -1;
        auto irqs = fdtable_->lock_.lock();
        for (int i = 0; i < NFDS; i++) {
            if (fdtable_->fds_[i] == nullptr) {
                fd = i;
                break;
            }
        }

        file* f = knew<file>();
        vnode* v = knew<vnode_inode>(ino);
        if (!f || !v || fd == -1) {
            fdtable_->lock_.unlock(irqs);
            fs.put_inode(ino);
            kdelete(f);
            kdelete(v);
            if (created) {
                // TODO
            }
            if (fd == -1)
                r = E_NFILE;
            else
                r = E_NOMEM;
            break;
        }
        fdtable_->fds_[fd] = f;
        fdtable_->lock_.unlock(irqs);

        if (flags & OF_TRUNC && flags & OF_WRITE) {
            irqs = f->lock_.lock();
            f->off_ = 0;
            f->lock_.unlock(irqs);
            ino->lock_write();
            ino->size = 0;
            ino->unlock_write();
        }

        f->readable_ = flags & OF_READ;
        f->writeable_ = flags & OF_WRITE;
        f->type_ = file::normie;
        f->vnode_ = v;

        debug_printf("[%d] sys_open succeeded -> fd %d\n", pid_, fd);

        r = fd;

        break;
    }

    case SYSCALL_PIPE: {
        uintptr_t rfd = -1;
        uintptr_t wfd = -1;

        auto irqs = fdtable_->lock_.lock();
        for(unsigned i = 0; i < NFDS; i++) {
            if (!fdtable_->fds_[i] && rfd == (uintptr_t) -1) {
                rfd = i;
            }
            else if (!fdtable_->fds_[i] && wfd == (uintptr_t) -1) {
                wfd = i;
                break;
            }
        }

        // not enough open fds
        if (rfd == (uintptr_t) -1 || wfd == (uintptr_t) -1) {
            fdtable_->lock_.unlock(irqs);
            r = E_NFILE;
            break;
        }

        auto rfile = fdtable_->fds_[rfd] = knew<file>();
        auto wfile = fdtable_->fds_[wfd] = knew<file>();
        auto pipe_vnode = knew<vnode_pipe>();
        auto bb = knew<bbuffer>();
        if (!rfile || !wfile || !pipe_vnode || !bb) {
            fdtable_->fds_[rfd] = fdtable_->fds_[wfd] = nullptr;
            kdelete(rfile);
            kdelete(wfile);
            kdelete(pipe_vnode);
            kdelete(bb);
            fdtable_->lock_.unlock(irqs);
            r = E_NOMEM;
            break;
        }

        rfile->type_ = wfile->type_ = file::pipe;
        rfile->readable_ = wfile->writeable_ = true;
        rfile->writeable_ = wfile->readable_ = false;
        rfile->vnode_ = wfile ->vnode_ = pipe_vnode;
        pipe_vnode->refs_ = 2;
        pipe_vnode->bb_ = bb;

        fdtable_->lock_.unlock(irqs);

        debug_printf("[%d] sys_pipe successful, r=%d w=%d\n", pid_, rfd, wfd);
        r = rfd | (wfd << 32);
        break;
    }

    case SYSCALL_EXECV: {
        auto program_name = reinterpret_cast<const char*>(regs->reg_rdi);
        auto argv = reinterpret_cast<const char* const*>(regs->reg_rsi);
        size_t argc = regs->reg_rdx;

        auto name_sz =
            check_string_termination(program_name, memfile::namesize);
        if (name_sz < 0) {
            r = name_sz;
            break;
        }

        debug_printf("[%d] sys_execv '%s' argc = %d\n",
            pid_, program_name, argc);

        // validate args
        if (argc < 1) {
            r = E_INVAL;
            break;
        } else if (!validate_memory(argv, (argc + 1) * 8, PTE_P | PTE_U)) {
            r = E_FAULT;
            break;
        } else if (!argv[0]) {  // need a real string in pos 0
            r = E_INVAL;
            break;
        }
        for(unsigned i = 0; i < argc + 1; i++) {
            // make sure argv is null-terminated
            if (i == argc) {
                if (argv[i] != nullptr) {
                    r = E_INVAL;
                    break;
                }
            } else {
                int r_cst =
                    check_string_termination(argv[i], memfile::namesize);
                if (r_cst < 0) {
                    r = r_cst;
                    break;
                }
            }
        }


        // allocate all the memory
        auto npt = kalloc_pagetable();
        auto stkpg = kallocpage();
        if (!npt || !stkpg) {
            kdelete(npt);
            kdelete(stkpg);
            r = E_NOMEM;
            break;
        }


        // load program
        disk_loader dl;
        dl.pagetable_ = npt;
        memcpy(dl.name_, program_name, name_sz);
        auto load_r = load(dl);
        if (load_r < 0) {
            r = load_r;
            debug_printf("[%d] exec load failed, r = %d\n", pid_, load_r);
            break;
        }


        // get the total length of the argv array
        size_t argv_len = 0;
        for (size_t i = 0; i < argc; i++) {
            argv_len += strlen(argv[i]) + 1;
        }

        // get the amount of memory for the ptrs to strings
        size_t sz_argv_ptrs = 8 * (argc + 1);

        // 8-byte align argv_len
        size_t mem_diff = argv_len;
        if (argv_len % 8 != 0) {
            mem_diff = ((argv_len / 8) + 1) * 8;
        }
        mem_diff += sz_argv_ptrs;

        // copy each string to the top of the stack page
        auto stkpg_top = reinterpret_cast<uintptr_t>(stkpg) + PAGESIZE;
        size_t arg_len = 0;
        for (size_t i = 0; i < argc; i++) {
            // copy string to stack page
            memcpy(reinterpret_cast<void*>(
                    stkpg_top - mem_diff + sz_argv_ptrs + arg_len),
                argv[i], strlen(argv[i]) + 1);

            // copy pointer to string to stack page
            uintptr_t argv_ptr =
                MEMSIZE_VIRTUAL - mem_diff + sz_argv_ptrs + arg_len;
            memcpy(reinterpret_cast<void*>(stkpg_top - mem_diff + 8 * i),
                &argv_ptr, 8);

            // update program_name to point into the new pagetable
            if (i == 0) {
                program_name = reinterpret_cast<const char*>(argv_ptr);
            }

            arg_len += strlen(argv[i]) + 1;
        }
        // null-terminate argv pointer array
        *reinterpret_cast<void**>(stkpg_top - mem_diff + sz_argv_ptrs - 8) =
            nullptr;

        // save things clobbered by init_user
        auto old_pt = pagetable_;
        auto old_regs = *regs;
        auto old_yields = yields_;

        // set up regs_ and pagetable_
        init_user(pid_, npt);
        yields_ = old_yields;

        regs_ = regs;

        // align stack by 16 bytes
        regs_->reg_rsp = MEMSIZE_VIRTUAL - 8;

        // map stackpage and console into vm
        assert(vmiter(this, MEMSIZE_VIRTUAL - PAGESIZE).map(ka2pa(stkpg),
                                                PTE_P | PTE_W | PTE_U) >= 0);
        assert(vmiter(this, ktext2pa(console)).map(ktext2pa(console),
                                                PTE_P | PTE_W | PTE_U) >= 0);

        set_pagetable(pagetable_);

        // set program entry point
        regs_->reg_rip = dl.entry_rip_;

        // set 1st argument to argc
        regs_->reg_rdi = argc;

        // set 2nd argument to argv pointer
        regs_->reg_rsi =
            reinterpret_cast<uintptr_t>(MEMSIZE_VIRTUAL - mem_diff);

        // set rsp to bottom of argv data
        uintptr_t below_argv = MEMSIZE_VIRTUAL - mem_diff - 8;
        if (below_argv % 16 != 0) below_argv = ((below_argv / 16) - 1) * 16;
        regs_->reg_rsp = regs_->reg_rbp = below_argv;

        nuke_pagetable(old_pt);

        yield_noreturn();
    }

    case SYSCALL_READDISKFILE: {
        const char* filename = reinterpret_cast<const char*>(regs->reg_rdi);
        unsigned char* buf = reinterpret_cast<unsigned char*>(regs->reg_rsi);
        uintptr_t sz = regs->reg_rdx;
        uintptr_t off = regs->reg_r10;

        if (!sata_disk) {
            return E_IO;
        }

        return chickadeefs_read_file_data(filename, buf, sz, off);
    }

    case SYSCALL_SYNC: {
        r = bufcache::get().sync(regs->reg_rdi != 0);
        break;
    }

    case SYSCALL_LSEEK: {
        int fd = regs->reg_rdi;
        ssize_t off = regs->reg_rsi;
        int origin = regs->reg_rdx;

        auto irqs = fdtable_->lock_.lock();
        int fd_r = validate_fd(fd, fdtable_);
        if (fd_r < 0) {
            fdtable_->lock_.unlock(irqs);
            r = fd_r;
            break;
        }

        auto f = fdtable_->fds_[fd];
        f->lock_.lock_noirq();
        fdtable_->lock_.unlock_noirq();

        // only normie types are seekable
        if (f->type_ != file::normie) {
            f->lock_.unlock(irqs);
            r = E_SPIPE;
            break;
        }

        ssize_t new_off;
        // sets the file position to off
        if (origin == LSEEK_SET) {
            new_off = off;
        }
        // adjusts the file position by off
        else if (origin == LSEEK_CUR) {
            new_off = (ssize_t) f->off_ + off;
        }
        // sets the file position to the end of the file plus off
        else if (origin == LSEEK_END) {
            new_off = (ssize_t) f->vnode_->size() + off;
        }
        // returns the file’s size without changing the position (off ignored)
        else if (origin == LSEEK_SIZE) {
            r = f->vnode_->size();
            f->lock_.unlock(irqs);
            break;
        }
        else {
            f->lock_.unlock(irqs);
            r = E_INVAL;
            break;
        }

        // negative file offsets make no sense
        f->off_ = (size_t) max(new_off, (ssize_t) 0);

        r = f->off_;
        f->lock_.unlock(irqs);
        break;
    }

    case SYSCALL_CLONE: {
        proc* new_p = kalloc_proc();

        auto irqs = ptable_lock.lock();
        new_p->ppid_ = ppid_;
        new_p->yields_ = nullptr;
        new_p->true_pid_ = true_pid_;
        new_p->fdtable_ = fdtable_;
        new_p->state_ = proc::runnable;
        new_p->pagetable_ = pagetable_;
        new_p->canary_ = canary_value;

        new_p->pid_ = get_proc_slot(ptable);
        if (new_p->pid_ < 0) {
            ptable_lock.unlock(irqs);
            kfree(new_p);
            r = -1; // TODO
            break;
        }
        ptable[new_p->pid_] = new_p;

        true_ptable[new_p->ppid_]->children_.push_back(new_p);

        new_p->regs_ = reinterpret_cast<regstate*>(
            reinterpret_cast<uintptr_t>(new_p) + KTASKSTACK_SIZE) - 1;
        *new_p->regs_ = *regs;
        new_p->regs_->reg_rax = 0;

        ptable_lock.unlock(irqs);

        int cpu = new_p->cpu_ = new_p->pid_ % ncpu;
        cpus[cpu].runq_lock_.lock_noirq();
        debug_printf("[%d] sys_clone enqueueing pid %d\n",
            pid_, new_p->pid_);
        cpus[cpu].enqueue(new_p);
        cpus[cpu].runq_lock_.unlock_noirq();

        r = new_p->pid_;
        break;
    }

    case SYSCALL_GETTID:
        r = pid_;
        break;

    case SYSCALL_TEXIT: {
        int status = regs->reg_rdi;
        debug_printf("[%d] sys_texit %d active threads\n",
            pid_, active_threads(this));

        if (active_threads(this) == 1) {
            process_exit(this, status);
        }
        else {
            exit_status_ = status;
            state_ = proc::broken;
        }

        waitpid_wq.wake_all();
        this->yield_noreturn();
    }

    case SYSCALL_MALLOC: {
        size_t size = regs->reg_rdi;
        if (size > (1U << 24)) { // 21 = MAX_ORDER from k-alloc.cc
            log_printf("WARNING: sys_malloc call exceeds maximum size\n");
        }
        if (size) {
            log_printf("[%d] sys_malloc %zu -> proc mem @ 0x%x\n",
                pid_, size, this->malloc_top_);
        } else {
            log_printf("WARNING: sys_malloc of size 0 -> nullptr\n");
            r = reinterpret_cast<uintptr_t>(nullptr);
            break;
        }

        auto kaddr = kalloc(size);
        if (!kaddr) {
            log_printf("WARNING: sys_malloc failed, probably out of memory\n");
            r = reinterpret_cast<uintptr_t>(nullptr);
            break;
        }

        int vm_r;
        for (unsigned off = 0; off < size; off += PAGESIZE) {
            vm_r = vmiter(this, this->malloc_top_ + off).map(
                            this->malloc_top_ + off, PTE_P|PTE_W|PTE_U);
            assert(vm_r >= 0);
        }

        r = this->malloc_top_;
        this->malloc_top_ = ROUNDUP(this->malloc_top_ + size + 64, PAGESIZE);
        break;
    }

    case SYSCALL_FREE: {
        uintptr_t ptr = regs->reg_rdi;
        if (!ptr) {
            r = 0;
            break;
        }

        auto ka = vmiter(this, ptr).ka();
        log_printf("[%d] sys_free %p -> ka %d\n", pid_, ptr, ka);
        kfree(ka);
        r = 0;
        break;
    }

    case SYSCALL_SWAPCOLOR: {
        uint8_t index = regs->reg_rdi;
        uint8_t red = regs->reg_rsi;
        uint8_t green = regs->reg_rdx;
        uint8_t blue = regs->reg_r10;

        outb(0x3C8, index);
        outb(0x3C9, red);
        outb(0x3C9, green);
        outb(0x3C9, blue);
        r = 0;
        break;
    }

    default:
        // no such system call
        log_printf("[%d] no such system call %u\n", pid_, regs->reg_rax);
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
