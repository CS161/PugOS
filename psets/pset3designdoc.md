# VFS Design Document
### Maxwell Levenson and Lucas Cassels
## VFS Layer Objects
`fdtable` - The file descriptor table. This enumerates the open `file`s for a given process.

`file` - The file structure stores offset, type and reference count information about a file to give information to the `vnode` on how to handle operations on the file.

`vnode` - Gives implementations of common functions to interface with the file data and metadata. 
Each `file` is associated with an appropriate `vnode`. 

`vnode_kbc` is a `vnode` subclass to handle the read/write operations for the keyboard and console.

`vnode_pipe` is a `vnode` subclass that handles read/write operations for created pipes.

`vnode_memfile` is a `vnode` subclass that handles read/write operations for in-memory files (programs that have been loaded into memory). 

`bbuffer` is a data structure for managing the data inside of a pipe. It is a static-size circular buffer that stores information written to a pipe that has not been read yet.

## Interface
```cpp
#define NFDS 256
// File descriptor table
struct fdtable {
    fdtable(): refs_(1), fds_{nullptr} { };

    // lock_ guards everything below it
    spinlock lock_;
    int refs_;
    file* fds_[NFDS]; // LENGTH: global constant
};
```
The size of table is defined by a global constant set in 
`k-vfs.hh`, the header file for our VFS implementation. We take inspiration from how the process table and CPU state table are defined. The `fdtable` is allocated, initialized, and stored on its appropriate `proc` struct when the `proc` is initialized either from `process_setup()` or from a fork. Right before the process pid is reclaimed in the exit process,  the file descriptor table is freed. Earlier on in the exit process, we handle decrementing and handling the files in the `fdtable`. On initialization, we set the 0-2 file descriptors to handle keyboard and console I/O.

----------

```cpp
struct file {
    file() : readable_(true), writeable_(true), refs_(1), off_(0) { };
    ~file();

    void deref();
    
    enum { normie, pipe, directory, stream } type_;
    const bool readable_;
    const bool writeable_;
    vnode* vnode_;
    
    // lock_ guards everything below it
    spinlock lock_;
    int refs_; // for threading later
    size_t off_;
};
```
When a file is opened, allocate a `file` struct. Set it's readable and writeable bools to default to true and set them otherwise in special cases like when the type is `pipe`. When it's closed, decrement the `file` struct, remove its entry from the process `fdtable`, and decrement the pointed to `vnode`. If the ref count reaches 0, free the file struct. 


----------


```cpp
struct vnode {
    vnode() : refs_(1) { };
    const char* filename_;
    
    // lock_ guards the refs
    spinlock lock_;
    int refs_;
    bbuffer* bb_;

    virtual size_t read(uintptr_t buf, size_t sz, size_t& off);
    virtual size_t write(uintptr_t buf, size_t sz, size_t& off);
};
```
`vnode`s are allocated when one does not already exist that corresponds to the `file` struct that a process creates. `vnode`s are freed once their reference counts decrement to zero. For most `vnode`s, the `bbuffer* bb_` member isn't used. We only use it for pipes.


----------
```cpp
struct vnode_kbc : vnode {
    const char* filename_ = "keyboard/console";
    size_t read(uintptr_t buf, size_t sz, size_t& off) override;
    size_t write(uintptr_t buf, size_t sz, size_t& off) override;
};
```
The keyboard and console have their own read and write calls. They need to have special code to handle interfacing with the respective devices.


----------

```cpp
struct vnode_pipe : vnode {
    const char* filename_ = "pipe";
    size_t read(uintptr_t buf, size_t sz, size_t& off) override;
    size_t write(uintptr_t buf, size_t sz, size_t& off) override;
};
```
The `vnode` for pipes is the only one that uses the `bbuffer* bb_` on the `vnode` class. We tried a few different architectural ideas for where to put the bbuffer for pipes, and this was the best tradeoff we found for clarity versus efficiency.

----------

```cpp
struct vnode_memfile : vnode {
    vnode_memfile(memfile* m) : m_(m) { filename_ = m->name_; };

    size_t read(uintptr_t buf, size_t sz, size_t& off) override;
    size_t write(uintptr_t buf, size_t sz, size_t& off) override;

    private:
        memfile* m_;
};
```
The `vnode_memfile` struct stores a private pointer to the corresponding `memfile` struct that stores the data and metadata for the program in memory.


----------
```cpp
#define BBUFFER_SIZE 16
struct bbuffer {
    bbuffer() : pos_(0), len_(0), write_closed_(0) { };

    // lock_ guards everything below it
    spinlock lock_;
    char buf_[BBUFFER_SIZE];
    size_t pos_;
    size_t len_;
    bool read_closed_;
    bool write_closed_;
    wait_queue read_wq_;
    wait_queue write_wq_;
};
```
The bounded buffer abstraction has two wait queues for managing read/write blocking, and two bools for keeping track of cases where the read or write end of a pipe has closed. 

## Functionality

#### file types

##### `normie`

Specifies files that support all standard operations (so far, they are seekable and thus `off_` is meaningful).

##### `stream`

Specifies files that are not seekable and whose data is consumed when read. For this type of file, `off_` is meaningless, due to their unseekable nature.

##### `pipe`

Like streams, but also means that vnode->bb_ exists, and other pipe-specific logic needs to be done (e.g. to close pipe ends).

##### `directory`

Specifies special files which are actually file directories.

----------

#### file modes

On creation of a `file` struct, `readable_` and `writeable_` should be set according the open mode of the file, and should not change.

The `sys_read` syscall should return `E_PERM` (operation not permitted) if it is called on a filedescriptor whose `file` struct has `readable_ = false`.

Similarly, the `sys_write` syscall should  return `E_PERM` (operation not permitted) if it is called on a filedescriptor whose `file` struct has `writeable = false`.

----------

#### vnode functions

##### `virtual size_t read(uintptr_t buf, size_t sz, size_t& off)`

The `read` syscall must guarantee that the calling process has write privileges to the memory range `[buf, buf + sz)`.
The function will read up to `sz` bytes from the `vnode` starting at position `off` into `buf` and return the number of bytes read.

##### `virtual size_t write(uintptr_t buf, size_t sz, size_t& off)`

The `write` syscall must guarantee that the calling process has read privileges to the memory range `[buf, buf + sz)`.
The function will write up to `sz` bytes from `buf` into the `vnode` starting at position `off` and return the number of bytes written.


## Synchronization and Blocking

#### Synchronization
- `fdtable`s need a lock because they are shared between threads and threads shouldn't clobber each other's changes to entries.
- `file`s need a lock to manage the offset because multiple threads can access the same file and might want to share the offset, but they shouldn't clobber each other's operations. The same lock is used to manage the refcount for threads and copies of the file in forked processes.
- `vnode`s need a lock to manage their refcounts and to free the `vnode` when its refcount is zero. This prevents a race condition with the `vnode`'s refcount being incremented and decremented at the same time by two processes. 
- `bbuffer`s have a lock to prevent the multiple processes or threads from writing to or reading from a pipe at the same time.
- `memfile`s have a lock that manages access to both the `memfile` structs themselves and the `memfile_initfs[]` array of `memfiles`. This prevents any race conditions between processes with editing or running in-memory programs.

#### Locking steps for file operations
1. Lock file descriptor table
2. Find file
3. Lock file noirq
4. Unlock file descriptor table noirq
5. Increment file ref count
6. Unlock file
7. { file blocks }
8. Lock file
9. Decrement ref count
10. Unlock file

----------

#### Blocking

`vnode`'s `read` and `write` functions will block, at least for the duration of time they take to perform their file operations. They may also block for longer if the `vnode` they need to access is currently locked (i.e. in use by another process or thread), in which case they will block until the `vnode` is available and then continue to block until they have performed their file operations.

In the special case of `pipe` vnodes, the `read` operation will block for a pipe read when the bounded buffer is empty and the `write` operation will block when the bounded buffer is full. 

## Future Directions

In the future, we might want a nice `seek(position, whence)` syscall specifying the position to seek from (inspired by `lseek`). We would probably have the options be the current position, the beginning of the file, and the end of it. This feature seems like it would be real nice.

It will be necessary once we have a disk file system to have some sort of global structure to contain our `vnode`s such that a file opened by two different processes separately point to the same `vnode` and blocking reads and writes work properly. The simplest implementation could be a linked-list. However instead of using a linked-list for storing the `vnode`s, we could use a hash-table hashed on the file names for quicker lookup. For infinite scalability, it could be a hash-table of linked-lists.

## Concerns

There is no way to seek into a file right now. A fix for this is mentioned in the future directions section.

Traversing a linked list of all open `vnode`s every time a process tries to open a file could be really slow. An improvement for this is mentioned in the future directions section.

Storing the bounded buffer on the super-struct `vnode` seems a bit clunky, but it was the best option we could find in terms of the usability, clarity, and efficiency. It's possible we may modify this design choice in the future.
