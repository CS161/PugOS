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






VFS OBJS
added bbuffer - bounded buffer for pipes

VNODES
vnode_keyboard and vnode_console are now merged into vnode_kbc
added vnode_memfile, vnode_pipe
removed unused virtual functions (i.e. everything but read and write)
removed sz_
linked list of open vnodes moved to future directions

FDTABLE
global length of fdtable stored in k-vfs.hh not kernel.hh
on fdtable initialization in process_setup, fds 0, 1, and 2 point to vnode_kbc

FILE
added stream type (used for kbc)
added deref function

FUNCTIONALITY
pipe is now specifically for pipes, with a new type stream for generic non-seekable files (like the keyboard/console)

FILE MODES
readable_, writeable_ no longer const

SYNCH
off_ controlled excelusively by vnode implementation
vnode lock_ only guards refs_


Grading notes
-------------
