CS 161 Problem Set 3 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset3collab.md`.

Answers to written questions
----------------------------

## Changes from initial vfs design

#### VFS OBJS
- added bbuffer - bounded buffer for pipes

#### VNODES
- vnode_keyboard and vnode_console are now merged into vnode_kbc
- added vnode_memfile, vnode_pipe
- removed unused virtual functions (i.e. everything but read and write)
- removed sz_
- linked list of open vnodes moved to future directions (wasn't relevant for this pset)
- read and write take a reference to the offset now so they can update it on the file struct
- read and write take uintptr_t instead of void*
- added generic filename to pipe and keyboard/console vnodes

#### FDTABLE
- global length of fdtable stored in k-vfs.hh not kernel.hh
- on fdtable initialization in process_setup, fds 0, 1, and 2 point to vnode_kbc

#### FILE
- added stream type (used for kbc)
- added deref function

#### FUNCTIONALITY
- pipe is now specifically for pipes, with a new type stream for generic non-seekable files (like the keyboard/console)

#### FILE MODES
- readable_, writeable_ no longer const

#### SYNCH
- off_ controlled excelusively by vnode implementation, synchronized managed by vnode virtual functions
- vnode lock_ only guards refs_
- block on reads when bbuffer is empty and writes when bbuffer is full
- added a memfs lock memfile::lock_ that protects the whole initfs array
- the added bounded buffer implementation has a lock that guards its data

#### FUTURE DIRECTIONS
- global list for vnodes for a file system
- removed section on not freeing vnodes when refcount hits 0

#### CONCERNS
- added note about how it seems a bit jank to store the bounded buffer on the base vnode class


Grading notes
-------------
