CS 161 Problem Set 1 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset1collab.md`.

Answers to written questions
----------------------------

### A.1
First address returned: 0xffff800000001000
This is because the kernel is allocating this first page, so it is a high canonical address, because the kernel maps its memory in high virtual addresses.

### A.2
Highest address: 0xffff8000001ff000

### A.3
High canonical (kernel) addresses. Line 17 (`p = pa2ka<x86_64_page*>(next_free_pa);`) determines this

### A.4
k-init.cc:177 from `physical_ranges.set(0, MEMSIZE_PHYSICAL, mem_available);` to `physical_ranges.set(0, 0x300000, mem_available);`

### A.5
```
    // skip over reserved and kernel memory
    while (next_free_pa != physical_ranges.limit()) {
        if (physical_ranges.type(next_free_pa) == mem_available) {
            // use this page
            p = pa2ka<x86_64_page*>(next_free_pa);
            next_free_pa += PAGESIZE;
            break;
        } else {
            // move to next range
            next_free_pa += PAGESIZE;
        }
    }
```

### A.6
The original find() loop is faster, because it can skip over entire ranges of memory that have the wrong type, while the new type() version can't

### A.7
Having no page_lock would introduce race conditions. A page could be allocated twice

### B.1
k-memviewer.cc:73 `mark(pa, f_kernel);`

### B.2
k-memviewer.cc:82 `mark(ka2pa(p), f_kernel | f_process(pid));`

### B.3
The ptiter loop loops over the physical memory holding the process' pagetables, while the vmiter loop loops over the virtual memory mapped by the process' pagetables. In both cases, level 0 pagetables are marked. If the pages marked by ptiter loop were use-accessible, the user process would be able to change its own virtual memory mappings and be able to grant themselves access to the entirety of physical memory.

### B.4
They're all mem_available. This is because we're still in the operating system bootup stage, so the processes haven't actually allocated anything yet

### B.5
QEMU displays a black screen. This is because this new version scans every single address in virtual memory NPROC times, whereas the original skips over empty segments of virtual memory (and virtual memory is normally pretty sparse so this is a bit difference)

### B.6
There is one page allocated to store v_ in memusage::refresh(), and then where ncpu > 1, ncpu pages are allocated in cpustate::idle_task() and are used to store idle tasks' process state structure (one for each core)

### B.7
See code

### C
See code


Grading notes
-------------
