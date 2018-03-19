#include "kernel.hh"
#include "elf.h"
#include "k-vmiter.hh"
#include "k-devices.hh"

proc* ptable[NPROC];                    // array of process descriptor pointers
// protects `ptable`, pid_, ppid_, and children_
spinlock ptable_lock;

// proc::proc()
//    The constructor initializes the `proc` to empty.

proc::proc()
    : pid_(0), regs_(nullptr), yields_(nullptr),
      state_(blank), pagetable_(nullptr), interrupted_(false) {
}


// kalloc_proc()
//    Allocate and return a new `proc`. Calls the constructor.

proc* kalloc_proc() {
    void* ptr;
    if (sizeof(proc) <= PAGESIZE) {
        ptr = kallocpage();
    } else {
        ptr = kalloc(sizeof(proc));
    }
    if (ptr) {
        return new (ptr) proc;
    } else {
        return nullptr;
    }
}


// helper function to print a proc state
static const char* sstring_null = "NULL";
static const char* sstring_blank = "BLANK";
static const char* sstring_broken = "BROKEN";
static const char* sstring_blocked = "BLOCKED";
static const char* sstring_runnable = "RUNNABLE";
static const char* sstring_unknown = "UNKNOWN";
const char* state_string(const proc* p) {
    if (!p) return sstring_null;
    switch (p->state_) {
        case proc::blank: return sstring_blank;
        case proc::broken: return sstring_broken;
        case proc::blocked: return sstring_blocked;
        case proc::runnable: return sstring_runnable;
        default: return sstring_unknown;
    }
}


// proc::init_user(pid, pt)
//    Initialize this `proc` as a new runnable user process with PID `pid`
//    and initial page table `pt`.

void proc::init_user(pid_t pid, x86_64_pagetable* pt) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(this);
    assert(!(addr & PAGEOFFMASK));
    // ensure layout `k-exception.S` expects
    assert(reinterpret_cast<uintptr_t>(&pid_) == addr);
    assert(reinterpret_cast<uintptr_t>(&regs_) == addr + 8);
    assert(reinterpret_cast<uintptr_t>(&yields_) == addr + 16);
    // ensure initialized page table
    assert(!(reinterpret_cast<uintptr_t>(pt) & PAGEOFFMASK));
    assert(pt->entry[256] == early_pagetable->entry[256]);
    assert(pt->entry[510] == early_pagetable->entry[510]);
    assert(pt->entry[511] == early_pagetable->entry[511]);

    pid_ = pid;
    canary_ = canary_value;

    regs_ = reinterpret_cast<regstate*>(addr + KTASKSTACK_SIZE) - 1;
    memset(regs_, 0, sizeof(regstate));
    regs_->reg_cs = SEGSEL_APP_CODE | 3;
    regs_->reg_fs = SEGSEL_APP_DATA | 3;
    regs_->reg_gs = SEGSEL_APP_DATA | 3;
    regs_->reg_ss = SEGSEL_APP_DATA | 3;
    regs_->reg_rflags = EFLAGS_IF;

    yields_ = nullptr;

    state_ = proc::runnable;

    pagetable_ = pt;
}


// proc::init_kernel(pid)
//    Initialize this `proc` as a new kernel process with PID `pid`,
//    starting at function `f`.

void proc::init_kernel(pid_t pid, void (*f)(proc*)) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(this);
    assert(!(addr & PAGEOFFMASK));

    pid_ = pid;
    canary_ = canary_value;

    regs_ = reinterpret_cast<regstate*>(addr + KTASKSTACK_SIZE) - 1;
    memset(regs_, 0, sizeof(regstate));
    regs_->reg_cs = SEGSEL_KERN_CODE;
    regs_->reg_fs = SEGSEL_KERN_DATA;
    regs_->reg_gs = SEGSEL_KERN_DATA;
    regs_->reg_ss = SEGSEL_KERN_DATA;
    regs_->reg_rflags = EFLAGS_IF;
    regs_->reg_rsp = addr + KTASKSTACK_SIZE;
    regs_->reg_rip = reinterpret_cast<uintptr_t>(f);
    regs_->reg_rdi = addr;

    yields_ = nullptr;
    state_ = proc::runnable;

    pagetable_ = early_pagetable;
}


#define SECTORSIZE 512

// proc::load(binary_name)
//    Load the code corresponding to program `binary_name` into this process
//    and set `regs_->reg_rip` to its entry point. Calls `kallocpage()`.
//    Returns 0 on success and negative on failure (e.g. out-of-memory).

int proc::load(const char* binary_name) {
    // find `memfile` for `binary_name`
    auto fs = memfile::initfs_lookup(binary_name);
    if (!fs) {
        return E_NOENT;
    }

    // validate the binary
    if (fs->len_ < sizeof(elf_header)) {
        return E_NOEXEC;
    }
    const elf_header* eh = reinterpret_cast<const elf_header*>(fs->data_);
    if (eh->e_magic != ELF_MAGIC
        || eh->e_phentsize != sizeof(elf_program)
        || eh->e_shentsize != sizeof(elf_section)
        || eh->e_phoff > fs->len_
        || eh->e_phnum == 0
        || (fs->len_ - eh->e_phoff) / eh->e_phnum < eh->e_phentsize) {
        return E_NOEXEC;
    }

    // load each loadable program segment into memory
    const elf_program* ph = reinterpret_cast<const elf_program*>
        (fs->data_ + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != ELF_PTYPE_LOAD) {
            continue;
        }
        if (ph[i].p_offset > fs->len_
            || fs->len_ - ph[i].p_offset < ph[i].p_filesz
            || ph[i].p_va > VA_LOWEND
            || VA_LOWEND - ph[i].p_va < ph[i].p_memsz) {
            return E_NOEXEC;
        }
        int r = load_segment(&ph[i], fs->data_ + ph[i].p_offset);
        if (r < 0) {
            return r;
        }
    }

    // set the entry point from the ELF header
    regs_->reg_rip = eh->e_entry;
    return 0;
}


// proc::load_segment(ph, src)
//    Load an ELF segment at virtual address `ph->p_va` into this process.
//    Copies `[src, src + ph->p_filesz)` to `dst`, then clears
//    `[ph->p_va + ph->p_filesz, ph->p_va + ph->p_memsz)` to 0.
//    Calls `kallocpage` to allocate pages and uses `vmiter::map`
//    to map them in `pagetable_`. Returns 0 on success and -1 on failure.

int proc::load_segment(const elf_program* ph, const uint8_t* data) {
    uintptr_t va = (uintptr_t) ph->p_va;
    uintptr_t end_file = va + ph->p_filesz;
    uintptr_t end_mem = va + ph->p_memsz;

    // allocate memory
    for (vmiter it(this, va & ~(PAGESIZE - 1));
         it.va() < end_mem;
         it += PAGESIZE) {
        x86_64_page* pg = kallocpage();
        if (!pg || it.map(ka2pa(pg)) < 0) {
            return E_NOMEM;
        }
        assert(it.pa() == ka2pa(pg));
    }

    // ensure new memory mappings are active
    set_pagetable(pagetable_);

    // copy data from executable image into process memory
    memcpy((uint8_t*) va, data, end_file - va);
    memset((uint8_t*) end_file, 0, end_mem - end_file);

    // restore early pagetable
    set_pagetable(early_pagetable);

    return 0;
}
