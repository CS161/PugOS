#ifndef CHICKADEE_K_CHKFS_HH
#define CHICKADEE_K_CHKFS_HH
#include "kernel.hh"
#include "chickadeefs.hh"
#include "k-lock.hh"
#include "k-wait.hh"

// buffer cache

struct bufentry {
    using blocknum_t = chickadeefs::blocknum_t;
    static constexpr blocknum_t emptyblock = blocknum_t(-1);

    spinlock lock_;                  // protects modification to `flags_`
                                     // and initial setting of `buf_`
    blocknum_t bn_ = emptyblock;     // disk block number or `emptyblock`
    unsigned ref_ = 0;               // refcount: protects entry
    unsigned write_ref_ = 0;         // write refcount: protect during writes
    unsigned flags_ = 0;             // flags
    void* buf_ = nullptr;            // memory buffer used for entry

    list_links entry_link_;
    list_links dirty_link_;
    volatile int fetch_status_ = 0;
    bool prefetched_ = false;

    enum {
        f_loaded = 1, f_loading = 2, f_dirty = 4
    };

    inline bool loadeding();
    inline void clear();
};

struct bufcache {
    using blocknum_t = bufentry::blocknum_t;
    static constexpr blocknum_t emptyblock = bufentry::emptyblock;

    static constexpr size_t ne = 100;
    static constexpr size_t n_prefetch = ne / 5;

    spinlock lock_;                  // protects all entries' bn_ and ref_
    wait_queue read_wq_;
    bufentry e_[ne];
    list<bufentry, &bufentry::entry_link_> e_list_;
    list<bufentry, &bufentry::entry_link_> pref_list_;
    list<bufentry, &bufentry::dirty_link_> dirty_list_;

    static inline bufcache& get();

    typedef void (*clean_block_function)(void*);

    int get_bufentry_slot(chickadeefs::blocknum_t bn, irqstate& irqs);
    bool load_disk_block(size_t i, chickadeefs::blocknum_t bn);

    bufentry* get_disk_entry(blocknum_t bn, clean_block_function = nullptr);
    void put_entry(bufentry* e);

    inline void* get_disk_block(blocknum_t bn, clean_block_function = nullptr);
    inline void put_block(void* pg);

    bufentry* find_entry(void* data);

    void get_write(bufentry* e);
    void put_write(bufentry* e);

    int sync(bool drop);

    void visualize();
    void sully(bufentry* e);

 private:
    static bufcache bc;

    bufcache();
    NO_COPY_OR_ASSIGN(bufcache);
};


// chickadeefs state: a Chickadee file system on a specific disk
// (Our implementation only speaks to `sata_disk`.)

struct chkfsstate {
    using blocknum_t = chickadeefs::blocknum_t;
    using inum_t = chickadeefs::inum_t;
    using inode = chickadeefs::inode;
    static constexpr size_t blocksize = chickadeefs::blocksize;
    static constexpr blocknum_t emptyblock = bufentry::emptyblock;


    static inline chkfsstate& get();

    inode* get_inode(inum_t inum);
    void put_inode(inode* ino);

    unsigned char* get_data_block(inode* ino, size_t off);

    inode* lookup_inode(inode* dirino, const char* name);

    blocknum_t allocate_block();
    ssize_t find_empty_inode();
    chickadeefs::dirent* find_empty_direntry(chickadeefs::inode* dirino);


  private:
    static chkfsstate fs;

    chkfsstate();
    NO_COPY_OR_ASSIGN(chkfsstate);
};

inline bool bufentry::loadeding() {
    return (flags_ & f_loaded) || (flags_ & f_loading);
}

inline void bufentry::clear() {
    bn_ = emptyblock;
    assert(ref_ == 0);
    flags_ = 0;
    buf_ = nullptr;
    prefetched_ = false;
}

inline bufcache& bufcache::get() {
    return bc;
}

// bufcache::get_disk_block, bufcache::put_block
//    Wrapper functions around get_disk_entry and put_entry.
inline void* bufcache::get_disk_block(blocknum_t bn,
                                      clean_block_function cleaner) {
    auto e = get_disk_entry(bn, cleaner);
    return e ? e->buf_ : nullptr;
}
inline void bufcache::put_block(void* buf) {
    put_entry(find_entry(buf));
}

inline chkfsstate& chkfsstate::get() {
    return fs;
}

size_t chickadeefs_read_file_data(const char* filename,
                                  void* buf, size_t sz, size_t off);

void visualize_root();


#endif
