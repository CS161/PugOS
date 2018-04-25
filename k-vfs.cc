#include "kernel.hh"
#include "k-chkfsiter.hh"
#include "k-devices.hh"
#include "k-vfs.hh"



vnode::~vnode() {
	kdelete(bb_);
}


// file

// DO NOT CALL WITH FILE LOCK HELD
void file::deref() {
	auto irqs = lock_.lock();
	--refs_;
	assert(refs_ >= 0);
	auto refs = refs_;
	lock_.unlock(irqs);
	if (refs == 0) {
		kdelete(this);
    }
}

file::~file() {
	auto irqs = vnode_->lock_.lock();
	--vnode_->refs_;
	assert(vnode_->refs_ >= 0);

	if (type_ == file::pipe && vnode_->bb_) {
		vnode_->bb_->lock_.lock_noirq();
		if (readable_) {
			vnode_->bb_->read_closed_ = true;
			vnode_->bb_->nonfull_wq_.wake_all();
		}
		if (writeable_) {
			vnode_->bb_->write_closed_ = true;
			vnode_->bb_->nonempty_wq_.wake_all();
		}
		vnode_->bb_->lock_.unlock_noirq();
	}

	auto refs = vnode_->refs_;
	vnode_->lock_.unlock(irqs);
	if (refs == 0) {
        // irqs = vnode_->bb_->lock_.lock();
        // vnode_->bb_->nonfull_wq_.q_.reset();
        // vnode_->bb_->nonempty_wq_.q_.reset();
        // vnode_->bb_->lock_.unlock(irqs);
		kdelete(vnode_);
	}
}



// vnode_kbc

size_t vnode_kbc::read(uintptr_t buf, size_t sz, size_t& off) {
	(void) off;
	auto& kbd = keyboardstate::get();
    auto irqs = kbd.lock_.lock();

    // mark that we are now reading from the keyboard
    // (so `q` should not power off)
    if (kbd.state_ == kbd.boot) {
        kbd.state_ = kbd.input;
    }

    // block until a line is available
    waiter(current()).block_until(kbd.wq_, [&] () {
            return sz == 0 || kbd.eol_ != 0;
        }, kbd.lock_, irqs);

    // read that line or lines
    size_t n = 0;
    while (kbd.eol_ != 0 && n < sz) {
        if (kbd.buf_[kbd.pos_] == 0x04) {
            // Ctrl-D means EOF
            if (n == 0) {
                kbd.consume(1);
            }
            break;
        } else {
            *reinterpret_cast<char*>(buf) = kbd.buf_[kbd.pos_];
            ++buf;
            ++n;
            kbd.consume(1);
        }
    }

    kbd.lock_.unlock(irqs);
    return n;
}


size_t vnode_kbc::write(uintptr_t buf, size_t sz, size_t& off) {
	(void) off;
    auto& csl = consolestate::get();
    auto irqs = csl.lock_.lock();

    size_t n = 0;
    while (n < sz) {
        int ch = *reinterpret_cast<const char*>(buf);
        ++buf;
        ++n;
        console_printf(0x0F00, "%c", ch);
    }

    csl.lock_.unlock(irqs);
    return n;
}



// vnode_pipe

size_t vnode_pipe::read(uintptr_t buf, size_t sz, size_t& off) {
	(void) off;
    size_t input_pos = 0;
    char* input_buf = reinterpret_cast<char*>(buf);

    auto irqs = bb_->lock_.lock();
    assert(!bb_->read_closed_);

    if (bb_->len_ == 0) {
	    // block until data is available
	    waiter(current()).block_until(bb_->nonempty_wq_, [&] () {
	            return sz == 0 || bb_->len_ > 0 || bb_->write_closed_;
	        }, bb_->lock_, irqs);
	}

    if (bb_->write_closed_ && bb_->len_ == 0) {
        bb_->lock_.unlock(irqs);
        return 0; // EOF
    }

    while (input_pos < sz && bb_->len_ > 0) {
        size_t ncopy = sz - input_pos;
        if (ncopy > BBUFFER_SIZE - bb_->pos_) {
            ncopy = BBUFFER_SIZE - bb_->pos_;
        }
        if (ncopy > bb_->len_) {
            ncopy = bb_->len_;
        }
        memcpy(&input_buf[input_pos], &bb_->buf_[bb_->pos_], ncopy);
        bb_->pos_ = (bb_->pos_ + ncopy) % BBUFFER_SIZE;
        bb_->len_ -= ncopy;
        input_pos += ncopy;
    }
    bb_->lock_.unlock(irqs);

    if (input_pos == 0 && sz > 0) {
    	return -1;
    } else {
    	bb_->nonfull_wq_.wake_all();
    	return input_pos;
    }
}


size_t vnode_pipe::write(uintptr_t buf, size_t sz, size_t& off) {
	(void) off;
    size_t input_pos = 0;
    const char* input_buf = reinterpret_cast<const char*>(buf);

    auto irqs = bb_->lock_.lock();
    assert(!bb_->write_closed_);

    if (bb_->len_ == BBUFFER_SIZE) {
	    // block until data is available
	    waiter(current()).block_until(bb_->nonfull_wq_, [&] () {
	            return sz == 0 || bb_->len_ < BBUFFER_SIZE || bb_->read_closed_;
	        }, bb_->lock_, irqs);
	}

    if (bb_->read_closed_) {
    	bb_->lock_.unlock(irqs);
    	return E_PIPE;
    }

    while (input_pos < sz && bb_->len_ < BBUFFER_SIZE) {
        size_t bb_index = (bb_->pos_ + bb_->len_) % BBUFFER_SIZE;
        size_t ncopy = sz - input_pos;
        if (ncopy > BBUFFER_SIZE - bb_index) {
            ncopy = BBUFFER_SIZE - bb_index;
        }
        if (ncopy > BBUFFER_SIZE - bb_->len_) {
            ncopy = BBUFFER_SIZE - bb_->len_;
        }
        memcpy(&bb_->buf_[bb_index], &input_buf[input_pos], ncopy);
        bb_->len_ += ncopy;
        input_pos += ncopy;
    }
    bb_->lock_.unlock(irqs);

    if (input_pos == 0 && sz > 0) {
        return -1;
    } else {
    	bb_->nonempty_wq_.wake_all();
        return input_pos;
    }
}



// vnode_memfile

size_t vnode_memfile::read(uintptr_t buf, size_t sz, size_t& off) {
    auto irqs = memfile::lock_.lock();

    // fuck blocking

    // read that line or lines
    size_t n = 0;
    while (n + off < m_->len_ && n < sz) {
        *reinterpret_cast<char*>(buf) = m_->data_[n + off];
        ++buf;
        ++n;
    }

    off += n;
    memfile::lock_.unlock(irqs);
    return n;
}


size_t vnode_memfile::write(uintptr_t buf, size_t sz, size_t& off) {
    auto irqs = memfile::lock_.lock();

    size_t n = 0;
    while (n < sz) {
        if (m_->len_ == m_->capacity_) {
            // Do some crazy shit -- allocate more memory
            unsigned char* new_data = reinterpret_cast<unsigned char*>(
                kalloc(m_->capacity_ << 1));
            memcpy(new_data, m_->data_, m_->len_);
            kfree(m_->data_);
            m_->data_ = new_data;
            m_->capacity_ <<= 1;
        }
        else {
            m_->data_[m_->len_] = *reinterpret_cast<const char*>(buf);
            ++buf;
            ++n;
            ++m_->len_;
        }
    }

    off += n;
    memfile::lock_.unlock(irqs);
    return n;
}



vnode_inode::~vnode_inode() {
    chkfsstate::get().put_inode(i_);
}

size_t vnode_inode::read(uintptr_t buf, size_t sz, size_t& off) {
    auto& bc = bufcache::get();
    auto& fs = chkfsstate::get();

    i_->lock_read();

    size_t nread = 0;
    while (sz > 0) {
        size_t ncopy = 0;

        // read inode contents, copy data
        size_t blockoff = ROUNDDOWN(off, fs.blocksize);
        // log_printf("\n\nblockoff %d\n", blockoff);
        if (void* data = fs.get_data_block(i_, blockoff)) {
            size_t block_end = ROUNDDOWN(off, fs.blocksize) + fs.blocksize;
            if (off + sz > block_end) {
                ncopy = fs.blocksize - (off % fs.blocksize);
            }
            else {
                ncopy = sz;
            }
            // log_printf("sz %d, ncopy %d, block_end %d\n", sz, ncopy, block_end);

            // don't read more data than there is
            if (ncopy > i_->size - off) {
                ncopy = i_->size - off;
            }
            memcpy(reinterpret_cast<unsigned char*>(buf) + nread,
                   reinterpret_cast<unsigned char*>(data) + (off - blockoff),
                   ncopy);
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
    i_->unlock_read();
    return nread;
}

size_t vnode_inode::write(uintptr_t buf, size_t sz, size_t& off) {
    auto& bc = bufcache::get();
    auto& fs = chkfsstate::get();

    i_->lock_write();
    chkfs_fileiter it(i_);

    size_t start_size = i_->size;
    size_t start_sz = sz;

    size_t nwritten = 0;
    while (sz > 0) {
        size_t ncopy = 0;

        // read inode contents, copy data
        size_t blockoff = ROUNDDOWN(off, fs.blocksize);

        if (void* data = fs.get_data_block(i_, blockoff)) {

            size_t block_end = blockoff + fs.blocksize;
            if (off + sz > block_end) {
                ncopy = fs.blocksize - (off % fs.blocksize);
                // allocate another block for this file and put it in the inode
                if (!fs.get_data_block(i_, block_end)) {
                    it.find(block_end);
                    auto new_b = fs.allocate_block();
                    assert(new_b != (chkfsstate::blocknum_t) E_NOSPC);
                    int r = it.map(new_b);
                    assert(r == 0 && "map new block failed");
                }
            }
            else {
                ncopy = sz;
            }

            // find bufentry for this data block
            bufentry* e = bc.find_entry(data);
            assert(e);
            bc.get_write(e);

            // write data
            auto irqs = e->lock_.lock();
            memcpy(reinterpret_cast<unsigned char*>(data) + (off - blockoff),
                   reinterpret_cast<unsigned char*>(buf) + nwritten,
                   ncopy);
            e->lock_.unlock(irqs);

            bc.put_write(e);
            bc.put_block(data);
        }

        // account for copied data
        if (ncopy == 0) {
            break;
        }
        nwritten += ncopy;
        off += ncopy;
        sz -= ncopy;
        if (off > i_->size) {
            i_->size = off;
        }
    }
    i_->unlock_write();
    return nwritten;
}

size_t vnode_inode::size() {
    i_->lock_read();
    auto r = i_->size;
    i_->unlock_read();
    return r;
}
