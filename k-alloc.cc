#include "kernel.hh"
#include "k-list.hh"
#include "k-lock.hh"

static spinlock page_lock;
static uintptr_t next_free_pa;

// allocator constants
static const int MIN_ORDER = 12;
static const int MAX_ORDER = 21;

struct pagestate {
    int order;
    bool allocated;
};
static pagestate pages[MEMSIZE_PHYSICAL / PAGESIZE];

struct Block {
    const int addr_;
    list_links link_;

    Block(const int addr)
        : addr_(addr) {
    }
};
static list<Block, &Block::link_> _order_lists[MAX_ORDER - MIN_ORDER + 1];
#define free_blocks(order) _order_lists[order - MIN_ORDER]

x86_64_page* kallocpage() {
    auto irqs = page_lock.lock();

    x86_64_page* p = nullptr;

    // skip over reserved and kernel memory
    auto range = physical_ranges.find(next_free_pa);
    while (range != physical_ranges.end()) {
        if (range->type() == mem_available) {
            // use this page
            p = pa2ka<x86_64_page*>(next_free_pa);
            next_free_pa += PAGESIZE;
            break;
        } else {
            // move to next range
            next_free_pa = range->last();
            ++range;
        }
    }

    page_lock.unlock(irqs);
    return p;
}


// find_max_order(addr)
//    Determine the largest order this address is aligned by which fits in the interval. Used in
//    init_kalloc
int find_max_order(uintptr_t start, uintptr_t end) {
    for (auto order = MAX_ORDER; order >= MIN_ORDER; order--) {
        if (start % (1 << order) == 0 && start + (1 << order) <= end) {
            return order;
        }
    }
    return -1;
}


// init_kalloc
//    Initialize stuff needed by `kalloc`. Called from `init_hardware`,
//    after `physical_ranges` is initialized.
void init_kalloc() {
    memset((void*) &pages, 0, sizeof(pages));

    for (auto range = physical_ranges.begin();
     range != physical_ranges.end();
     ++range) {

        auto addr = range->first();
        while(addr < range->last()) {
            int order = find_max_order(addr, range->last());
            assert(order > 0);
            pages[addr / PAGESIZE].order = order;

            // is this range allocated?
            if (range->type() == mem_available) {
                pages[addr / PAGESIZE].allocated = false;
            }
            else {
                pages[addr / PAGESIZE].allocated = true;
            }

            addr += 1 << order;
        }
    }
}

// kalloc(sz)
//    Allocate and return a pointer to at least `sz` contiguous bytes
//    of memory. Returns `nullptr` if `sz == 0` or on failure.
void* kalloc(size_t sz) {
    assert(0 && "kalloc not implemented yet");
}

// kfree(ptr)
//    Free a pointer previously returned by `kalloc`, `kallocpage`, or
//    `kalloc_pagetable`. Does nothing if `ptr == nullptr`.
void kfree(void* ptr) {
    assert(0 && "kfree not implemented yet");
}

// test_kalloc
//    Run unit tests on the kalloc system.
void test_kalloc() {
    log_printf("Running kalloc unit tests...\n");
    // helper functions
    assert(find_max_order(7, 10) == -1);
    assert(find_max_order(0x1000, 0x10000) == 12);
    assert(find_max_order(0x10000, 0xFFFFFFFFFFF) == 16);
    assert(find_max_order(0x0, (uintptr_t) -1) == MAX_ORDER);
    assert(find_max_order(0x0, 0x1000) == 12);
}
