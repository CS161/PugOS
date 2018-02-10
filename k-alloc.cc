#include "kernel.hh"
#include "k-list.hh"
#include "k-lock.hh"

static spinlock page_lock;
static uintptr_t next_free_pa;

// allocator constants
static const int NPAGES = MEMSIZE_PHYSICAL / PAGESIZE;
static const int MIN_ORDER = 12;
static const int MAX_ORDER = 21;
static const int NORDERS = MAX_ORDER - MIN_ORDER + 1;

// physical page array
struct pagestate {
    list_links link_;
    int order;
    int pindex;
    bool allocated;
};
static pagestate pages[NPAGES];

// memory block linked lists
static list<pagestate, &pagestate::link_> _free_block_lists[NORDERS];


// free_blocks(order)
//    Returns a pointer to the linked list containing free blocks of size order
list<pagestate, &pagestate::link_>* free_blocks(int order) {
    assert(order >= MIN_ORDER);
    assert(order <= MAX_ORDER);
    return &_free_block_lists[order - MIN_ORDER];
}


// order_of(size)
//    Returns the smallest order of size
int order_of(int n) {
    if (n == 0) {
        return 0;
    }
    else {
        return msb(n - 1);
    }
}


x86_64_page* kallocpage() {
    return reinterpret_cast<x86_64_page*>(kalloc(PAGESIZE));
    // auto irqs = page_lock.lock();

    // x86_64_page* p = nullptr;

    // // skip over reserved and kernel memory
    // auto range = physical_ranges.find(next_free_pa);
    // while (range != physical_ranges.end()) {
    //     if (range->type() == mem_available) {
    //         // use this page
    //         p = pa2ka<x86_64_page*>(next_free_pa);
    //         next_free_pa += PAGESIZE;
    //         break;
    //     } else {
    //         // move to next range
    //         next_free_pa = range->last();
    //         ++range;
    //     }
    // }

    // page_lock.unlock(irqs);
    // return p;
}


// find_max_order(addr)
//    Determine the largest order buddy-allocator block creatable at this
//    address, considering available free space and alignment. Used in
//    init_kalloc.
int find_max_order(uintptr_t start, uintptr_t end) {
    for (auto order = MAX_ORDER; order >= MIN_ORDER; order--) {
        if (start % (1 << order) == 0 && start + (1 << order) <= end) {
            return order;
        }
    }
    return -1;
}


// debug function
void print_block_list(list<pagestate, &pagestate::link_>* lst) {
    for (pagestate* b = lst->front(); b; b = lst->next(b)) {
        log_printf("%d ", b->pindex);
    }
    log_printf("\n");
}


// init_kalloc
//    Initialize stuff needed by `kalloc`. Called from `init_hardware`,
//    after `physical_ranges` is initialized.
void init_kalloc() {
    memset(pages, 0, sizeof(pages));

    for (auto range = physical_ranges.begin();
     range->first() < MEMSIZE_PHYSICAL;
     ++range) {

        auto addr = range->first();
        while(addr < range->last()) {
            // find the largest buddy-allocation chunk we can make here
            int order = find_max_order(addr, range->last());
            assert(order > 0);
            int pindex = addr / PAGESIZE;
            pages[pindex].order = order;
            pages[pindex].pindex = pindex;

            // is the chunk allocated?
            if (range->type() == mem_available) {
                pages[pindex].allocated = false;
                // add the available block to the free_blocks lists
                free_blocks(order)->push_back(&pages[pindex]);
            }
            else {
                pages[pindex].allocated = true;
            }

            // move past the chunk we just made
            addr += 1 << order;
        }
    }
}

// min_larger_order
//    Finds the smallest order block that can be broken up to eventually
//    get blocks of goal order size
int min_larger_order(int order) {
    int test_order = order;
    while (free_blocks(test_order)->empty()) {
        ++test_order;
        if (test_order > MAX_ORDER) return -1;
    }
    assert(test_order <= MAX_ORDER);
    return test_order;
}

// kalloc(sz)
//    Allocate and return a pointer to at least `sz` contiguous bytes
//    of memory. Returns `nullptr` if `sz == 0` or on failure.
void* kalloc(size_t sz) {
    log_printf("kalloc(%d)\n", sz);
    if (!sz) return nullptr;

    int order = order_of(sz) < MIN_ORDER ? MIN_ORDER : order_of(sz);
    if (order > MAX_ORDER) return nullptr;

    // order of largest block to be broken up
    int largest_min_order = min_larger_order(order);

    // break up blocks if necessary
    while (free_blocks(order)->empty()) {
        int larger_order = min_larger_order(order);
        if (larger_order < 0) return nullptr;

        // block to be broken in half
        pagestate* target_block = free_blocks(larger_order)->pop_front();
        target_block->order--;
        free_blocks(larger_order - 1)->push_back(target_block);

        // second half of split larger block
        int new_pindex = target_block->pindex +
            (1 << (target_block->order - order_of(PAGESIZE)));
        pagestate* new_block = &pages[new_pindex];

        // put new block info into pages array
        new_block->order = target_block->order;
        new_block->allocated = false;
        new_block->pindex = new_pindex;
        // put new block into free blocks lists
        free_blocks(new_block->order)->push_back(new_block);
    }

    pagestate* free_block = free_blocks(order)->pop_front();
    free_block->allocated = true;
    return pa2ka<void*>(free_block->pindex * PAGESIZE);
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
    // helper functions
    assert(find_max_order(7, 10) == -1);
    assert(find_max_order(0x1000, 0x10000) == 12);
    assert(find_max_order(0x10000, 0xFFFFFFFFFFF) == 16);
    assert(find_max_order(0x0, (uintptr_t) -1) == MAX_ORDER);
    assert(find_max_order(0x0, 0x1000) == 12);

    log_printf("msb(0) = 0x%x, order_of(0) = 0x%x\n", msb(0), order_of(0));
    log_printf("msb(1) = 0x%x, order_of(1) = 0x%x\n", msb(1), order_of(1));
    assert(order_of(4095) == 12);
    assert(order_of(4096) == 12);
    assert(order_of(4097) == 13);

    // pages array invariants
    uintptr_t addr = 0;
    while (addr < MEMSIZE_PHYSICAL) {
        auto page = &pages[addr / PAGESIZE];
        assert(page->order >= MIN_ORDER);
        assert(page->order <= MAX_ORDER);
        assert(addr % (1 << page->order) == 0); // buddy allocator alignment
        addr += (1 << page->order);
    }
}
