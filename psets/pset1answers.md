CS 161 Problem Set 1 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset1collab.md`.

Answers to written questions
----------------------------

### A.1
First address returned: 0xffff'8000'0000'1000
This is because the kernel is allocating this first page, so it is a high canonical address, because the kernel maps its memory in high virtual addresses.

### A.2
Highest address: 0xffff'8000'001f'f000

### A.3
High canonical (kernel) addresses. Line 17 (`p = pa2ka<x86_64_page*>(next_free_pa);`) determines this

### A.4
k-init.cc:177 from `physical_ranges.set(0, MEMSIZE_PHYSICAL, mem_available);` to `physical_ranges.set(0, 0x300000, mem_available);`

### A.5
```
    // skip over reserved and kernel memory
    while (!p && next_free_pa != physical_ranges.limit()) {
        if (physical_ranges.type(next_free_pa) == mem_available) {
            // use this page
            p = pa2ka<x86_64_page*>(next_free_pa);
        }
        next_free_pa += PAGESIZE;
    }
```

### A.6
The original find() loop is faster, because it can skip over entire ranges of memory that have the wrong type, while the new type() version can't

### A.7
Having no page_lock would introduce race conditions. A page could be allocated twice (to multiple cores)

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

### D
See code

### E.1
Kernel entry points:
1. cpustate::schedule (called from k-exception.S:217 proc::yield() and proc::yield_noreturn(), to yield to other processes)
2. kernel_start() (called from k-exception.S:19 kernel_entry, to set up the kernel)
3. proc::exception() (called from k-exception.S:74 exception_entry, to handle exceptions in kernel mode)
4. proc::syscall() (called from k-exception.S:174 syscall_entry, to handle system calls in kernel mode)
5. cpustate::init_ap() (called from k-exception.S:324 ap_rest, initializing an application processor)
6. cpustate::idle_task() (called to create or return an idle task, and allocates a new stack when creating an idle task)

Process entry points:
1. process_main() (called on running a process)

### E.2
Kernel entry points:
1. cpustate::schedule() not correctly aligned
2. kernel_start() correctly aligned
3. proc::exception() correctly aligned
4. proc::syscall() correctly aligned
5. cpustate::init_ap() not correctly aligned
6. cpustate::idle_task() correctly aligned

Process entry points:
1. process_main() not correctly aligned

### E.3
Kernel entry points:
1. cpustate::schedule() fixed in proc::yield and proc::yield_noreturn
5. cpustate::init_ap() fixed in ap_rest

Process entry points:
1. process_main() fixed in kernel.cc:process_setup()

### F.1
See sys_commit_seppuku()

### F.2
See kernel.cc:check_corruption()

### F.3
-Wstack-usage=4000 detects the problem. There is likely a slightly more accurate number to test, which would probably be something closer to 4096, but 4000 is large enough to not get false negatives and catches the problem

Grading notes
-------------
