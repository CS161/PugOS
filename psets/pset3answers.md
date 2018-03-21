CS 161 Problem Set 3 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset3collab.md`.

Answers to written questions
----------------------------

## Changes

deref in file struct

void* -> uintptr_t in read/write params

bb * on vnode

keyboard and console vnodes are actually 1 vnode

added a filename on keyboard/console and pipe vnodes (why? who knows)

sys read and write throw ebadf not eperm on read/write bad perms

offset ref on file, passed into vnode functions

Grading notes
-------------
