#ifndef CHICKADEE_K_FS_HH
#define CHICKADEE_K_FS_HH
#include "k-devices.hh"
#include "k-wait.hh"
#include "k-lock.hh"

#define NFDS 256


// for pipes

#define BBUFFER_SIZE 128

struct bbuffer {
   char buf_[BBUFFER_SIZE];
   size_t pos_;
   size_t len_;
   bool read_closed_;
   bool write_closed_;
   spinlock lock_;

   wait_queue nonfull_wq_;
   wait_queue nonempty_wq_;

   bbuffer() : buf_{0}, pos_(0), len_(0), read_closed_(false),
               write_closed_(false) { };
};


struct vnode {
    const char* filename_;

    // bounded buffer for pipes
    bbuffer* bb_;
    
    // lock_ guards everything below it
    spinlock lock_;
    int refs_;
    size_t sz_;

    virtual size_t read(uintptr_t buf, size_t sz, size_t& offset) { return E_PERM; };
    virtual size_t write(uintptr_t buf, size_t sz, size_t& offset) { return E_PERM; };
    // virtual int link() { return E_PERM; };
    // virtual int unlink() { return E_PERM; };
    // virtual int symlink() { return E_PERM; };
    // virtual int creat() { return E_PERM; };
    // virtual int mkdir() { return E_PERM; };
    // virtual int rmdir() { return E_PERM; };
    // virtual int rename() { return E_PERM; };

    vnode() : bb_(nullptr), refs_(1), sz_(0) { };
    ~vnode();
};


struct file {
    enum type_t {
        normie, pipe, directory
    };

    type_t type_;
    bool readable_;
    bool writeable_;
    vnode* vnode_;
    
    // lock_ guards everything below it
    spinlock lock_;
    int refs_; // for threading later
    void deref();

    file() : refs_(1), off_internal_(0) { };
    ~file();

    size_t &off_ = off_internal_;

  private:
    size_t off_internal_;
};


struct fdtable {
    // lock_ guards everything below it
    spinlock lock_;
    int refs_; // for threading
    file* fds_[NFDS]; // LENGTH: global constant

    fdtable() : refs_(1), fds_{nullptr} { };
};



// fd 0-2 Keyboard/Console

struct vnode_kbc : vnode {
    const char* filename_ = "keyboard/console";
    virtual size_t read(uintptr_t buf, size_t sz, size_t& offset) override;
    virtual size_t write(uintptr_t buf, size_t sz, size_t& offset) override;
};


// pipes

struct vnode_pipe : vnode {
    const char* filename_ = "pipe";
    virtual size_t read(uintptr_t buf, size_t sz, size_t& offset) override;
    virtual size_t write(uintptr_t buf, size_t sz, size_t& offset) override;
};


// memfs files

struct vnode_memfile : vnode {
    virtual size_t read(uintptr_t buf, size_t sz, size_t& offset) override;
    virtual size_t write(uintptr_t buf, size_t sz, size_t& offset) override;

    vnode_memfile(memfile* m) : m_(m) { filename_ = m->name_; };

  private:
    memfile* m_;
};


#endif
