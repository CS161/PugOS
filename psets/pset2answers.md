CS 161 Problem Set 2 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset2collab.md`.

Answers to written questions
----------------------------

### D
Design of per-process metadata:
We implemented a linked-list on each process struct (proc::children_) that contained that process' child processes. That way, when a process is freed, it only needs to go through its linked-list of children instead of the whole ptable, meaning it takes O(C) time instead of O(P).

We added a few synchronization invariants:
- proc::ppid_ is guarded by ptable_lock
- proc::children_ is guarded by ptable_lock
These are guarded by ptable_lock and not a new process hierarchy lock because we found that when doing things involving the process hierarchy, we often needed lock both ptable_lock and the new process hierarchy lock, and we wanted to avoid requiring multiple locks being held at the same time.

### E
sys_waitpid usually accesses ptable and proc::pid_, so we continued using our old ptable_lock invariants and locking ptable_lock whenever we accessed ptable or proc::pid_. We also use the waitqueue lock to make sure there are no race conditions involving the waitqueue.

### F
With a naive waitqueue, testmsleep generated around 52,000 resumes. With a timing wheel implemented, this dropped to around 300 resumes (not bad!).


Grading notes
-------------
