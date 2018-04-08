#include "k-chkfs.hh"
#include "k-devices.hh"

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


size_t bufcache::find_bufentry(chickadeefs::blocknum_t bn) {
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
        for (i = 0; i != ne && e_[i].bn_ != emptyblock; ++i) { };

        if (i == ne) {
            // search for 0 ref block in lru list
            for (auto b = e_list_.front(); b; b = e_list_.next(b)) {
                if (b->ref_ == 0) {
                    i = (reinterpret_cast<uintptr_t>(b) -
                         reinterpret_cast<uintptr_t>(&e_)) / sizeof(bufentry);
                    e_list_.erase(b);
                    kfree(b->buf_);
                    b->clear();
                    break;
                }
            }
        }

        // no free block found
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

void* bufcache::get_disk_block(chickadeefs::blocknum_t bn,
                               clean_block_function cleaner) {
    assert(chickadeefs::blocksize == PAGESIZE);
    auto irqs = lock_.lock();
    bool prefetching = bn != SUPERBLOCK_BN;
    bool prefetch_resolved = false;

    auto i = find_bufentry(bn);
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
            auto pref_i = find_bufentry(bn + n);
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
        }
    }


    // return memory
    auto buf = e_[i].buf_;
    e_[i].lock_.unlock(irqs);

    if (prefetch_resolved) {
        irqs = lock_.lock();
        pref_list_.erase(&e_[i]);
        e_list_.push_back(&e_[i]);
        lock_.unlock(irqs);
    }

    return buf;
}


// bufcache::put_block(buf)
//    Decrement the reference count for buffer cache block `buf`.

void bufcache::put_block(void* buf) {
    if (!buf) {
        return;
    }

    auto irqs = lock_.lock();

    // find block
    size_t i;
    for (i = 0; i != ne; ++i) {
        if (e_[i].ref_ != 0 && e_[i].buf_ == buf) {
            break;
        }
    }
    assert(i != ne);

    // drop reference
    if (e_[i].bn_ != SUPERBLOCK_BN) {
        --e_[i].ref_;
        // if (e_[i].ref_ == 0) {
        //     e_list_.erase(&e_[i]);
        //     kfree(e_[i].buf_);
        //     e_[i].clear();
        // }
    }

    lock_.unlock(irqs);
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
        } else if (mlock.compare_exchange_weak(v, v + 1,
                                               std::memory_order_acquire)) {
            return;
        } else {
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
    assert(mlock.load(std::memory_order_relaxed) == uint32_t(-1));
    mlock.store(0, std::memory_order_release);
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
            (bc.get_disk_block(inode_bn + inum / chickadeefs::inodesperblock));
    }
    if (ino != nullptr) {
        ino += inum % chickadeefs::inodesperblock;
        // ++ino->mref;
    }
    return ino;
}


// chkfsstate::put_inode(ino)
//    Drop the reference to `ino`.
void chkfsstate::put_inode(inode* ino) {
    if (ino) {
        // --ino->mref;
        bufcache::get().put_block(ROUNDDOWN(ino, PAGESIZE));
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

    // look up data block number
    unsigned bi = off / blocksize;
    chickadeefs::blocknum_t databn = 0;
    if (bi < chickadeefs::ndirect) {
        databn = ino->direct[bi];
    } else if (bi < chickadeefs::ndirect + chickadeefs::nindirect) {
        if (ino->indirect != 0) {
            auto indirect_data = bc.get_disk_block(ino->indirect);
            assert(indirect_data);
            databn = reinterpret_cast<chickadeefs::blocknum_t*>(indirect_data)
                [bi - chickadeefs::ndirect];
            bc.put_block(indirect_data);
        }
    } else {
        chickadeefs::blocknum_t indirbn = 0;
        if (ino->indirect2 != 0) {
            auto indirect2_data = bc.get_disk_block(ino->indirect2);
            assert(indirect2_data);
            bi -= chickadeefs::ndirect + chickadeefs::nindirect;
            indirbn = reinterpret_cast<chickadeefs::blocknum_t*>(indirect2_data)
                [bi / chickadeefs::nindirect];
            bc.put_block(indirect2_data);
        }

        if (indirbn != 0) {
            auto indirect_data = bc.get_disk_block(databn);
            assert(indirect_data);
            databn = reinterpret_cast<chickadeefs::blocknum_t*>(indirect_data)
                [bi % chickadeefs::nindirect];
            bc.put_block(indirect_data);
        }
    }

    // load data block
    void* data = nullptr;
    if (databn) {
        data = bc.get_disk_block(databn);
    }

    // clean up
    return reinterpret_cast<unsigned char*>(data);
}


// chkfsstate::lookup_inode(dirino, filename)
//    Look up `filename` in the directory inode `dirino`, returning the
//    corresponding inode (or nullptr if not found). The caller should
//    eventually call `put_inode` on the returned inode pointer.
chickadeefs::inode* chkfsstate::lookup_inode(inode* dirino,
                                             const char* filename) {
    auto& bc = bufcache::get();

    // read directory to find file inode
    chickadeefs::inum_t in = 0;
    for (size_t diroff = 0; !in; diroff += blocksize) {
        void* directory_data = get_data_block(dirino, diroff);
        if (!directory_data) {
            break;
        }

        size_t bsz = min(dirino->size - diroff, blocksize);
        auto dirent = reinterpret_cast<chickadeefs::dirent*>(directory_data);
        for (unsigned i = 0; i * sizeof(*dirent) < bsz; ++i, ++dirent) {
            if (dirent->inum && strcmp(dirent->name, filename) == 0) {
                in = dirent->inum;
                break;
            }
        }

        bc.put_block(directory_data);
    }

    return get_inode(in);
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

    size_t nread = 0;
    while (sz > 0) {
        size_t ncopy = 0;

        // read inode contents, copy data
        size_t blockoff = ROUNDDOWN(off, fs.blocksize);
        if (void* data = fs.get_data_block(ino, blockoff)) {
            size_t bsz = min(ino->size - blockoff, fs.blocksize);
            size_t boff = off - blockoff;
            if (bsz > boff) {
                ncopy = bsz - boff;
                if (ncopy > sz) {
                    ncopy = sz;
                }
                memcpy(reinterpret_cast<unsigned char*>(buf) + nread,
                       reinterpret_cast<unsigned char*>(data) + boff,
                       ncopy);
            }
            bc.put_block(data);
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
