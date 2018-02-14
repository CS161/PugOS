#include "kernel.hh"
#include "k-vmiter.hh"

cpustate cpus[NCPU];
int ncpu;


// cpustate::init()
//    Initialize a `cpustate`. Should be called once per active CPU,
//    by the relevant CPU.

void cpustate::init() {
    {
        // check that this CPU is one of the expected CPUs
        uintptr_t addr = reinterpret_cast<uintptr_t>(this);
        assert((addr & PAGEOFFMASK) == 0);
        assert(this >= cpus && this < cpus + NCPU);
        assert(read_rsp() > addr && read_rsp() <= addr + CPUSTACK_SIZE);

        // ensure layout `k-exception.S` expects
        assert(reinterpret_cast<uintptr_t>(&self_) == addr);
        assert(reinterpret_cast<uintptr_t>(&current_) == addr + 8);
        assert(reinterpret_cast<uintptr_t>(&syscall_scratch_) == addr + 16);
    }

    self_ = this;
    current_ = nullptr;
    index_ = this - cpus;
    runq_lock_.clear();
    idle_task_ = nullptr;
    spinlock_depth_ = 0;

    canary_ = canary_value;

    // now initialize the CPU hardware
    init_cpu_hardware();
}


// cpustate::annihilate(p)
//    Destroy p and scatter its ashes to the corners of the world
static int ka2p(void* addr) {
    return (int) (ka2pa(reinterpret_cast<uintptr_t>(addr)) / PAGESIZE);
}

#define debug_print(p, ptr)                                \
    if (ptr != nullptr) {                               \
        log_printf("Freeing pid %d's '"#ptr"' va %p ", p->pid_, ptr);       \
        log_printf("pindex %d pa %p\n", ka2p(ptr), ka2pa(ptr));           \
    } else {                                            \
        log_printf("Not freeing "#ptr", is null\n");    \
    }

void annihilate(proc* p) {
    // log_printf("cpustate::annihilate pid %d\n", p->pid_);
    // free stack page
    // vmiter it(p, MEMSIZE_VIRTUAL - PAGESIZE);
    // log_printf("\tstack page: pa=%p ka=%p\n", it.pa(), it.ka());
    // if (it.pa() != 0xffff'ffff'ffff'ffff) {
    //     kfree(reinterpret_cast<void*>(pa2ka(it.pa())));
    // }

    for (vmiter vmit(p); vmit.va() < MEMSIZE_VIRTUAL; vmit.next()) {
        if (vmit.user() && vmit.writable() && vmit.pa() != ktext2pa(console)) {
            // log_printf("%d virtual mem: freeing va %p\n", p->pid_, it.va());
            kfree(reinterpret_cast<void*>(pa2ka(vmit.pa())));
        }
    }

    // free misc proc struct stuff
    // log_printf("\tfreeing regs_ and yields_\n");
    kfree(p->regs_);
    kfree(p->yields_);

    // free pagetables
    // log_printf("\tfreeing l3-1 pagetables:\n");
    for (ptiter ptit(p->pagetable_, 0); ptit.low(); ptit.next()) {
        // log_printf("\t\tpa=%p\n", ptit.ptp_pa());
        kfree(reinterpret_cast<void*>(pa2ka(ptit.ptp_pa())));
    }
    // log_printf("\tfreeing l4 pagetable pa=%p ka=%p\n",
        // ka2pa(p->pagetable_), p->pagetable_);
    kfree(p->pagetable_);

    auto pid = p->pid_;
    // log_printf("\tfreeing process struct pa=%p ka=%p\n", ka2pa(p), p);
    kfree(p);

    // wipe process from ptable array
    auto irqs = ptable_lock.lock();
    ptable[pid] = nullptr;
    ptable_lock.unlock(irqs);

    // log_printf("\tcompleted\n", pid);
}


// cpustate::enqueue(p)
//    Enqueue `p` on this CPU's run queue. `p` must not be on any
//    run queue, it must be resumable, and `this->runq_lock_` must
//    be held.

void cpustate::enqueue(proc* p) {
    assert(p->resumable());
    runq_.push_back(p);
}


static void print_runq(cpustate* c) {
    debug_printf("CPU %d runq pids: ", c->lapic_id_);
    if (c->runq_.empty()) {
        debug_printf("nothing in queue");
    }
    else {
        for (proc* p = c->runq_.front(); p; p = c->runq_.next(p)) {
            debug_printf("%d ", p->pid_);
        }
    }
    debug_printf("\n");
}


// cpustate::schedule(yielding_from)
//    Run a process, or the current CPU's idle task if no runnable
//    process exists. If `yielding_from != nullptr`, then do not
//    run `yielding_from` unless no other runnable process exists.

void cpustate::schedule(proc* yielding_from) {
    assert(contains(read_rsp()));  // running on CPU stack
    assert(is_cli());              // interrupts are currently disabled
    assert(spinlock_depth_ == 0);  // no spinlocks are held
    assert(read_rbp() % 16 == 0);  // check stack alignment

    // initialize idle task; don't re-run it
    if (!idle_task_) {
        init_idle_task();
    } else if (current_ == idle_task_) {
        yielding_from = idle_task_;
    }

    while (1) {
        // if (current_) {
        //     debug_printf(1, "cpu::schedule checking process pid %d\n",
        //         current_->pid_);
        // }
        // else {
        //     debug_printf(1, "cpu::schedule checking null process\n");
        // }
        // print_runq(this);

        // try to run `current`
        if (current_
            && current_->state_ == proc::runnable
            && current_ != yielding_from) {
            set_pagetable(current_->pagetable_);
            current_->resume();
        }

        // otherwise load the next process from the run queue
        runq_lock_.lock_noirq();
        if (current_) {
            // re-enqueue `current_` at end of run queue if runnable
            if (current_->state_ == proc::runnable) {
                enqueue(current_);
            }

            // switch to a safe page table
            lcr3(ktext2pa(early_pagetable));
            
            // if `current` is broken, clean it up
            if (current_->state_ == proc::broken) {
                annihilate(current_);
            }
        
            current_ = yielding_from = nullptr;
        }

        if (!runq_.empty()) {
            // pop head of run queue into `current_`
            current_ = runq_.pop_front();
        }
        runq_lock_.unlock_noirq();

        // if run queue was empty, run the idle task
        if (!current_) {
            current_ = idle_task_;
        }
    }
}


// cpustate::idle_task()
//    Every CPU has an *idle task*, which is a kernel task (i.e., a
//    `proc` that runs in kernel mode) that just stops the processor
//    until an interrupt is received. The idle task runs when a CPU
//    has nothing better to do.

void idle(proc*) {
    while (1) {
        asm volatile("hlt");
    }
}

void cpustate::init_idle_task() {
    assert(!idle_task_);
    idle_task_ = reinterpret_cast<proc*>(kallocpage());
    idle_task_->init_kernel(-1, idle);
    assert(idle_task_->regs_->reg_rbp % 16 == 0);
}
