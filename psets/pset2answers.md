CS 161 Problem Set 1 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset1collab.md`.

Answers to written questions
----------------------------

### D
Design of per-process metadata:
We implemented a linked-list on each process struct (proc::children_) that contained that process' child processes. That way, when a process is freed, it only needs to go through its linked-list of children instead of the whole ptable, meaning it takes O(C) time instead of O(P).


Grading notes
-------------
