#include "k-chkfs.hh"
#include "k-devices.hh"
#include "k-chkfsiter.hh"

bufcache bufcache::bc;

#define SUPERBLOCK_BN 0

bufcache::bufcache() {
    // // put all blocks in lru list at startup
    // for (size_t i = 0; i < ne; ++i) {
    //     e_list_.push_back(&e_[i]);
    // }

    // load superblock into cache permanently
    get_disk_block(SUPERBLOCK_BN);
    e_[SUPERBLOCK_BN].ref_ = 1;
}


// bufcache::find_bufentry_slot(bn, cleaner)
//    Read disk block `bn` into the buffer cache, obtain a reference to it,
//    and return a pointer to its bufentry. The function may block. If this
//    function reads the disk block from disk, and `cleaner != nullptr`,
//    then `cleaner` is called on the block data. Returns `nullptr` if
//    there's no room for the block.

size_t bufcache::find_bufentry_slot(chickadeefs::blocknum_t bn, irqstate& irqs){
    // look for slot containing `bn`
    size_t i;
    for (i = 0; i != ne; ++i) {
        if (e_[i].bn_ == bn) {
            e_list_.erase(&e_[i]);
            break;
        }
    }

    // if not found, look for free slot
    if (i == ne) {
        // search for empty block in cache
        for (i = 0; i != ne && e_[i].bn_ != emptyblock; ++i) {
            if (e_[i].flags_ & bufentry::f_dirty && !e_[i].ref_) {
                lock_.unlock(irqs);
                sync(false);
                irqs = lock_.lock();
                i = 0;
            }
        };

        // search for 0 ref block in lru list
        if (i == ne) {
            for (auto b = e_list_.front(); b; b = e_list_.next(b)) {
                if (b->ref_ == 0 && !b->dirty_) {
                    i = (reinterpret_cast<uintptr_t>(b) -
                         reinterpret_cast<uintptr_t>(&e_)) / sizeof(bufentry);
                    e_list_.erase(b);
                    kfree(b->buf_);
                    b->clear();
                    break;
                }
            }
        }

        // search for unused prefetches
        if (i == ne) {
            for (auto b = pref_list_.front(); b; b = pref_list_.next(b)) {
                if (b->was_prefetched_ && b->fetch_status_ != E_AGAIN) {
                    i = (reinterpret_cast<uintptr_t>(b) -
                         reinterpret_cast<uintptr_t>(&e_)) / sizeof(bufentry);
                    // log_printf("Evicting bn %d from slot %d\n", b->bn_, i);
                    pref_list_.erase(b);
                    kfree(b->buf_);
                    b->clear();
                    break;
                }
            }
        }

        if (i == ne) {
            // log_printf("Failed to find a slot...\n");
            // int counter = 0;
            // log_printf("BNs in bufcache ");
            // for (int j = 0; j < ne; j++) {
            //     log_printf("%d:%d ", e_[j].bn_, e_[j].ref_);
            //     counter++;
            // }
            // log_printf("\n");
            // log_printf("LRU queue ");
            // for (auto b = e_list_.front(); b; b = e_list_.next(b)) {
            //     log_printf("%d ", b->bn_);
            // }
            // log_printf("\n");
            // log_printf("Prefetch queue ");
            // for (auto b = pref_list_.front(); b; b = pref_list_.next(b)) {
            //     log_printf("%d ", b->bn_);
            // }
            // log_printf("\n\n");

            return -1;
        }
        
    }

    e_[i].bn_ = bn;
    // log_printf("find_bufentry_slot pushing %p\n", &e_[i]);
    e_list_.push_back(&e_[i]);
    return i;
}


bool bufcache::load_disk_block(size_t i, chickadeefs::blocknum_t bn) {
    auto irqs = e_[i].lock_.lock();

    // load already done/in progress
    if ((e_[i].flags_ & bufentry::f_loading)
          || (e_[i].flags_ & bufentry::f_loaded)) {
        e_[i].lock_.unlock(irqs);
        return true;
    }

    if (!e_[i].buf_) {
        e_[i].buf_ = kalloc(chickadeefs::blocksize);
        if (!e_[i].buf_) {
            --e_[i].ref_;
            e_[i].lock_.unlock(irqs);
            return false;
        }
    }
    e_[i].flags_ |= bufentry::f_loading;
    e_[i].lock_.unlock(irqs);

    int r = sata_disk->read_nonblocking(e_[i].buf_, chickadeefs::blocksize,
                                        bn * chickadeefs::blocksize,
                                        &e_[i].fetch_status_);

    if (!r) {
        e_[i].flags_ &= ~bufentry::f_loading;
    }
    return r;
}


// bufcache::get_disk_block(bn, cleaner)
//    Read disk block `bn` into the buffer cache, obtain a reference to it,
//    and return a pointer to its data. The function may block. If this
//    function reads the disk block from disk, and `cleaner != nullptr`,
//    then `cleaner` is called on the block data. Returns `nullptr` if
//    there's no room for the block.

bufentry* bufcache::get_disk_entry(chickadeefs::blocknum_t bn,
                               clean_block_function cleaner) {
    assert(chickadeefs::blocksize == PAGESIZE);
    auto irqs = lock_.lock();
    bool prefetching = bn != SUPERBLOCK_BN;
    bool prefetch_resolved = false;

    auto i = find_bufentry_slot(bn, irqs);
    if (i == (size_t) -1) {
        lock_.unlock(irqs);
        log_printf("bufcache: no room for block %u\n", bn);
        return nullptr;
    }
    // debug_printf("get_disk_block %d\n", bn);

    // mark reference
    if (bn != SUPERBLOCK_BN) {
        ++e_[i].ref_;
    }

    // only prefetch if we're loading the block for the first time
    if ((e_[i].flags_ & bufentry::f_loaded)
          || (e_[i].flags_ & bufentry::f_loading)) {
        prefetching = false;
    }

    lock_.unlock(irqs);
    if (!load_disk_block(i, bn)) {
        return nullptr;
    }

    if (prefetching && n_prefetch) {
        for (unsigned n = 1; n <= n_prefetch; ++n) {
            irqs = lock_.lock();
            debug_printf("attempting prefetch of block %d\n", bn + n);
            auto pref_i = find_bufentry_slot(bn + n, irqs);
            if (pref_i == (size_t) -1) {
                lock_.unlock(irqs);
                debug_printf("\tprefetch failed, no space in bufcache\n");
                break;
            }
            debug_printf("\tprefetching into slot %d, initial ref_ = %d\n",
                         pref_i, e_[pref_i].ref_);
            if (e_[pref_i].ref_ == 0) {
                debug_printf("\tblock not in cache, doing first load\n");
                // move block to prefetch list
                e_list_.erase(&e_[pref_i]);
                // log_printf("swapping to pref_list %p\n", &e_[pref_i]);
                pref_list_.push_front(&e_[pref_i]);
                e_[pref_i].was_prefetched_ = true;
            }
            lock_.unlock(irqs);

            if (!load_disk_block(pref_i, bn + n)) {
                debug_printf("\tprefetch load failed\n");
                break;
            }
        }
    }

    irqs = sata_disk->lock_.lock();
    while (e_[i].fetch_status_ == E_AGAIN) {
        waiter(current()).block_until(sata_disk->wq_, [&] () {
                return e_[i].fetch_status_ != E_AGAIN;
            }, sata_disk->lock_, irqs);
    }
    sata_disk->lock_.unlock(irqs);

    irqs = e_[i].lock_.lock();
    if (e_[i].flags_ & bufentry::f_loading) {
        debug_printf("disk load completed\n");

        if (e_[i].fetch_status_ == E_IO) {
            // SEND HELP
            log_printf("EORAGJIESRKVKSEOIRGJO:EFKWACAE\n");
        }
        
        e_[i].flags_ = (e_[i].flags_ & ~bufentry::f_loading)
            | bufentry::f_loaded;
        if (cleaner) {
            cleaner(e_[i].buf_);
        }
        if (e_[i].was_prefetched_) {
            debug_printf("detected completed prefetch\n");
            prefetch_resolved = true;
            e_[i].was_prefetched_ = false;
        }
    }

    e_[i].lock_.unlock(irqs);

    if (prefetch_resolved) {
        irqs = lock_.lock();
        pref_list_.erase(&e_[i]);
        // log_printf("prefetch resolution pushing %p\n", &e_[i]);
        e_list_.push_back(&e_[i]);
        lock_.unlock(irqs);
    }


    // for (auto b = e_list_.front(); b; b = e_list_.next(b)) {
    //     auto index = (reinterpret_cast<uintptr_t>(b) -
    //               reinterpret_cast<uintptr_t>(&e_)) / sizeof(bufentry);
    //     if (index >= ne) {
    //         log_printf("DANGEROUS BLOCK: %p\n", b);
    //     }
    // }

    return &e_[i];
}


// bufcache::find_entry(buf)
//    Return the `bufentry` containing pointer `buf`. This entry
//    must have a nonzero `ref_`.

bufentry* bufcache::find_entry(void* buf) {
    if (buf) {
        buf = ROUNDDOWN(buf, chickadeefs::blocksize);

        // Synchronization is not necessary!
        // 1. The relevant entry has nonzero `ref_`, so its `buf_`
        //    will not change.
        // 2. No other entry has the same `buf_` because nonempty
        //    entries have unique `buf_`s.
        // (XXX Really, though, `buf_` should be std::atomic<void*>.)
        for (size_t i = 0; i != ne; ++i) {
            if (e_[i].buf_ == buf) {
                return &e_[i];
            }
        }
        assert(false);
    }
    return nullptr;
}


// bufcache::put_entry(e)
//    Decrement the reference count for buffer cache entry `e`.

void bufcache::put_entry(bufentry* e) {
    if (e) {
        auto irqs = e->lock_.lock();
        // drop reference
        if (e->bn_ != SUPERBLOCK_BN) {
            --e->ref_;
            // if (e_[i].ref_ == 0) {
            //     e_list_.erase(&e_[i]);
            //     kfree(e_[i].buf_);
            //     e_[i].clear();
            // }
        }
        e->lock_.unlock(irqs);
    }
}


// bufcache::get_write(e)
//    Obtain a write reference for `e`.

void bufcache::get_write(bufentry* e) {
    auto irqs = e->lock_.lock();

    waiter(current()).block_until(read_wq_, [&] () {
            return e->write_ref_ == 0;
        }, e->lock_, irqs);
    e->write_ref_ = 1;
    if (!e->dirty_) {
        dirty_list_.push_front(e);
    }
    e->dirty_ = true;
    e->lock_.unlock(irqs);
}


// bufcache::put_write(e)
//    Release a write reference for `e`.

void bufcache::put_write(bufentry* e) {
    auto irqs = e->lock_.lock();
    --e->write_ref_;
    e->lock_.unlock(irqs);

    read_wq_.wake_all();
}


// bufcache::sync(drop)
//    Write all dirty buffers to disk (blocking until complete).
//    Additionally free all buffer cache contents, except referenced
//    blocks, if `drop` is true.

int bufcache::sync(bool drop) {
    // Write dirty buffers to disk
    // Swap list to local copy to prevent new entries being added during sync
    list<bufentry, &bufentry::dirty_link_> temp_dirty;
    temp_dirty.swap(dirty_list_);
    while (bufentry* e = temp_dirty.pop_front()) {
        get_write(e);
        int r = sata_disk->write(e->buf_, chickadeefs::blocksize,
                e->bn_ * chickadeefs::blocksize, &e->fetch_status_);
        auto irqs = e->lock_.lock();
        e->flags_ &= ~bufentry::f_dirty;
        e->lock_.unlock(irqs);
        put_write(e);
    }

    if (drop) {
        auto irqs = lock_.lock();

        for (size_t i = 0; i != ne; ++i) {
            if (!e_[i].was_prefetched_ && e_[i].bn_ != emptyblock
                  && !e_[i].ref_) {
                e_list_.erase(&e_[i]);
                kfree(e_[i].buf_);
                e_[i].clear();
            }
        }
        lock_.unlock(irqs);
    }

    return 0;
}


// clean_inode_block(buf)
//    This function is called when loading an inode block into the
//    buffer cache. It clears values that are only used in memory.

static void clean_inode_block(void* buf) {
    auto is = reinterpret_cast<chickadeefs::inode*>(buf);
    for (unsigned i = 0; i != chickadeefs::inodesperblock; ++i) {
        is[i].mlock = is[i].mref = 0;
    }
}


// inode lock functions
//    The inode lock protects the inode's size and data references.
//    It is a read/write lock; multiple readers can hold the lock
//    simultaneously.
//    IMPORTANT INVARIANT: If a kernel task has an inode lock, it
//    must also hold a reference to the disk page containing that
//    inode.

namespace chickadeefs {

void inode::lock_read() {
    uint32_t v = mlock.load(std::memory_order_relaxed);
    while (1) {
        if (v == uint32_t(-1)) {
            current()->yield();
            v = mlock.load(std::memory_order_relaxed);
        } else if (mlock.compare_exchange_weak(v, v + 1,
                                               std::memory_order_acquire)) {
            return;
        } else {
            // `compare_exchange_weak` already reloaded `v`
            pause();
        }
    }
}

void inode::unlock_read() {
    uint32_t v = mlock.load(std::memory_order_relaxed);
    assert(v != 0 && v != uint32_t(-1));
    while (!mlock.compare_exchange_weak(v, v - 1,
                                        std::memory_order_release)) {
        pause();
    }
}

void inode::lock_write() {
    uint32_t v = 0;
    while (!mlock.compare_exchange_weak(v, uint32_t(-1),
                                        std::memory_order_acquire)) {
        current()->yield();
        v = 0;
    }
}

void inode::unlock_write() {
    assert(has_write_lock());
    mlock.store(0, std::memory_order_release);
}

bool inode::has_write_lock() const {
    return mlock.load(std::memory_order_relaxed) == uint32_t(-1);
}

}


// chickadeefs state

chkfsstate chkfsstate::fs;

chkfsstate::chkfsstate() {
}


// chkfsstate::get_inode(inum)
//    Return inode number `inum`, or `nullptr` if there's no such inode.
//    The returned pointer should eventually be passed to `put_inode`.
chickadeefs::inode* chkfsstate::get_inode(inum_t inum) {
    auto& bc = bufcache::get();

    unsigned char* superblock_data = reinterpret_cast<unsigned char*>
        (bc.get_disk_block(0));
    assert(superblock_data);
    auto sb = reinterpret_cast<chickadeefs::superblock*>
        (&superblock_data[chickadeefs::superblock_offset]);
    auto inode_bn = sb->inode_bn;
    auto ninodes = sb->ninodes;
    bc.put_block(superblock_data);

    chickadeefs::inode* ino = nullptr;
    if (inum > 0 && inum < ninodes) {
        ino = reinterpret_cast<inode*>
            (bc.get_disk_block(inode_bn + inum / chickadeefs::inodesperblock,
                               clean_inode_block));
    }
    if (ino != nullptr) {
        ino += inum % chickadeefs::inodesperblock;
        ++ino->mref;
    }
    return ino;
}


// chkfsstate::put_inode(ino)
//    Drop the reference to `ino`.
void chkfsstate::put_inode(inode* ino) {
    if (ino) {
        --ino->mref;
        if (ino->mref <= 0) {
            bufcache::get().put_block(ROUNDDOWN(ino, PAGESIZE));
        }
    }
}


// chkfsstate::get_data_block(ino, off)
//    Return a pointer to the data page at offset `off` into inode `ino`.
//    `off` must be a multiple of `blocksize`. May return `nullptr` if
//    no block has been allocated there. If the file is being read,
//    then at most `min(blocksize, ino->size - off)` bytes of data
//    in the returned page are valid.
unsigned char* chkfsstate::get_data_block(inode* ino, size_t off) {
    assert(off % blocksize == 0);
    auto& bc = bufcache::get();

    bufentry* i2e = nullptr;       // bufentry for indirect2 block (if needed)
    blocknum_t* iptr = nullptr;    // pointer to indirect block # (if needed)
    bufentry* ie = nullptr;        // bufentry for indirect block (if needed)
    blocknum_t* dptr = nullptr;    // pointer to direct block #
    bufentry* de = nullptr;        // bufentry for direct block

    unsigned bi = off / blocksize;

    // Set `iptr` to point to the relevant indirect block number
    // (if one is needed). This is either a pointer into the
    // indirect2 block, or a pointer to `ino->indirect`, or null.
    if (bi >= chickadeefs::ndirect + chickadeefs::nindirect) {
        if (ino->indirect2 != 0) {
            i2e = bc.get_disk_entry(ino->indirect2);
        }
        if (!i2e) {
            goto done;
        }
        iptr = reinterpret_cast<blocknum_t*>(i2e->buf_)
            + chickadeefs::bi_indirect_index(bi);
    } else if (bi >= chickadeefs::ndirect) {
        iptr = &ino->indirect;
    }

    // Set `dptr` to point to the relevant data block number.
    // This is either a pointer into an indirect block, or a
    // pointer to one of the `ino->direct` entries.
    if (iptr) {
        if (*iptr != 0) {
            ie = bc.get_disk_entry(*iptr);
        }
        if (!ie) {
            goto done;
        }
        dptr = reinterpret_cast<blocknum_t*>(ie->buf_)
            + chickadeefs::bi_direct_index(bi);
    } else {
        dptr = &ino->direct[chickadeefs::bi_direct_index(bi)];
    }

    // Finally, load the data block.
    if (*dptr != 0) {
        de = bc.get_disk_entry(*dptr);
    }

 done:
    // We don't need the indirect and doubly-indirect entries.
    bc.put_entry(ie);
    bc.put_entry(i2e);
    return de ? reinterpret_cast<unsigned char*>(de->buf_) : nullptr;
}


// chkfsstate::lookup_inode(dirino, filename)
//    Look up `filename` in the directory inode `dirino`, returning the
//    corresponding inode (or nullptr if not found). The caller should
//    eventually call `put_inode` on the returned inode pointer.
chickadeefs::inode* chkfsstate::lookup_inode(inode* dirino,
                                             const char* filename) {
    auto& bc = bufcache::get();
    chkfs_fileiter it(dirino);

    // read directory to find file inode
    chickadeefs::inum_t in = 0;
    for (size_t diroff = 0; !in; diroff += blocksize) {
        bufentry* e;
        if (!(it.find(diroff).present()
              && (e = bc.get_disk_entry(it.blocknum())))) {
            break;
        }

        size_t bsz = min(dirino->size - diroff, blocksize);
        auto dirent = reinterpret_cast<chickadeefs::dirent*>(e->buf_);
        for (unsigned i = 0; i * sizeof(*dirent) < bsz; ++i, ++dirent) {
            if (dirent->inum && strcmp(dirent->name, filename) == 0) {
                in = dirent->inum;
                break;
            }
        }

        bc.put_entry(e);
    }

    return get_inode(in);
}


// chkfsstate::allocate_block()
//    Allocate and return the number of a fresh block. The returned
//    block need not be initialized (but it should not be in flight
//    to the disk or part of any incomplete journal transaction).
//    Returns the block number or an error code on failure. Errors
//    can be distinguished by `blocknum >= blocknum_t(E_MINERROR)`.

auto chkfsstate::allocate_block() -> blocknum_t {
    auto& bc = bufcache::get();

    // load superblock
    unsigned char* superblock_data = reinterpret_cast<unsigned char*>
        (bc.get_disk_block(0));
    assert(superblock_data);
    auto sb = reinterpret_cast<chickadeefs::superblock*>
        (&superblock_data[chickadeefs::superblock_offset]);

    // load free block bitmap
    auto fbb_e = bc.get_disk_entry(sb->fbb_bn);
    assert(fbb_e);
    auto fbb = reinterpret_cast<unsigned char*>(fbb_e->buf_);

    auto nblocks = sb->nblocks;
    bc.put_block(superblock_data);
    bc.put_block(sb);

    // find and mark a free block
    bc.get_write(fbb_e);
    blocknum_t bn;
    for (bn = 0; bn < nblocks; ++bn) {
        if (fbb[bn / 8] & (1 << (bn % 8))) {
            fbb[bn / 8] &= ~(1 << (bn % 8));
            break;
        }
    }

    bc.put_write(fbb_e);
    bc.put_block(fbb);
    if (bn == nblocks) {
        return E_NOSPC;
    }
    else {
        return bn;
    }
}


// chkfsstate::find_empty_inode()
//      Traverses the arrays of inode entries until it finds an empty entry.
//      Returns the number of the empty inode.
ssize_t chkfsstate::find_empty_inode() {
    auto& bc = bufcache::get();

    unsigned char* superblock_data = reinterpret_cast<unsigned char*>
        (bc.get_disk_block(0));
    assert(superblock_data);
    auto sb = reinterpret_cast<chickadeefs::superblock*>
        (&superblock_data[chickadeefs::superblock_offset]);
    auto inode_bn = sb->inode_bn;
    auto ninodes = sb->ninodes;
    bc.put_block(superblock_data);

    chickadeefs::inode* ino = nullptr;
    bufentry* e = nullptr;
    for (ssize_t inum = 0; inum < ninodes; inum++) {
        // get the next block for inode* array
        if (inum % chickadeefs::inodesperblock == 0) {
            e = bc.get_disk_entry(
                    inode_bn + inum / chickadeefs::inodesperblock,
                    clean_inode_block);
            ino = reinterpret_cast<inode*>(e->buf_);
        }
        // ignore the null inode and root inode
        if (inum < 2) {
            continue;
        }
        // if it is an empty inode entry
        chickadeefs::inode* ino_curr = &ino[inum % chickadeefs::inodesperblock];
        if (ino_curr->type == 0) {
            // mark the inode entry as dirty
            bc.get_write(e);
            bc.put_write(e);
            bc.put_entry(e);
            return inum;
        }
    }
    return 0;
}


// chkfsstate::find_empty_entry()
//      Traverses the arrays of directory entries until it finds an empty entry.
//      Returns that empty entry for allocation.
//      TODO: allocate new block for direntries if last is full
chickadeefs::dirent* chkfsstate::find_empty_direntry(inode* dirino) {
    auto& bc = bufcache::get();
    chkfs_fileiter it(dirino);

    // read directory to find file inode
    chickadeefs::inum_t in = 0;
    for (size_t diroff = 0; !in; diroff += blocksize) {
        bufentry* e;
        if (!(it.find(diroff).present()
              && (e = bc.get_disk_entry(it.blocknum())))) {
            break;
        }
        bc.get_write(e);

        size_t bsz = min(dirino->size - diroff, blocksize);
        auto dirent = reinterpret_cast<chickadeefs::dirent*>(e->buf_);
        for (unsigned i = 0; i * sizeof(*dirent) < bsz; ++i, ++dirent) {
            if (!dirent->inum) {
                bc.put_write(e);
                bc.put_entry(e);
                return dirent;
            }
        }

        bc.put_write(e);
        bc.put_entry(e);
    }

    return nullptr;
}


// chickadeefs_read_file_data(filename, buf, sz, off)
//    Read up to `sz` bytes, from the file named `filename` in the
//    disk's root directory, into `buf`, starting at file offset `off`.
//    Returns the number of bytes read.

size_t chickadeefs_read_file_data(const char* filename,
                                  void* buf, size_t sz, size_t off) {
    auto& bc = bufcache::get();
    auto& fs = chkfsstate::get();

    // read directory to find file inode number
    auto dirino = fs.get_inode(1);
    assert(dirino);
    dirino->lock_read();

    auto ino = fs.lookup_inode(dirino, filename);

    dirino->unlock_read();
    fs.put_inode(dirino);

    if (!ino) {
        return 0;
    }

    // read file inode
    ino->lock_read();
    chkfs_fileiter it(ino);

    size_t nread = 0;
    while (sz > 0) {
        size_t ncopy = 0;

        // read inode contents, copy data
        bufentry* e;
        if (it.find(off).present()
            && (e = bc.get_disk_entry(it.blocknum()))) {
            size_t blockoff = ROUNDDOWN(off, fs.blocksize);
            size_t bsz = min(ino->size - blockoff, fs.blocksize);
            size_t boff = off - blockoff;
            if (bsz > boff) {
                ncopy = bsz - boff;
                if (ncopy > sz) {
                    ncopy = sz;
                }
                memcpy(reinterpret_cast<unsigned char*>(buf) + nread,
                       reinterpret_cast<unsigned char*>(e->buf_) + boff,
                       ncopy);
            }
            bc.put_entry(e);
        }

        // account for copied data
        if (ncopy == 0) {
            break;
        }
        nread += ncopy;
        off += ncopy;
        sz -= ncopy;
    }

    ino->unlock_read();
    fs.put_inode(ino);
    return nread;
}
