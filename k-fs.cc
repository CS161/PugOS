#include "kernel.hh"
#include "k-devices.hh"



vnode::~vnode() {
	kdelete(bb_);
}


// file

// returns 1 if it freed itself, 0 if it still exists
// DO NOT CALL WITH FILE LOCK HELD
void file::deref() {
	auto irqs = lock_.lock();
	--refs_;
	assert(refs_ >= 0);
	auto refs = refs_;
	lock_.unlock(irqs);
	if (refs == 0)
		kdelete(this);
}

file::~file() {
	auto irqs = vnode_->lock_.lock();
	--vnode_->refs_;
	assert(vnode_->refs_ >= 0);

	if (type_ == file::pipe && vnode_->bb_) {
		auto irqs = vnode_->bb_->lock_.lock();
		if (type_ == file::pipe && readable_) {
			vnode_->bb_->read_closed_ = true;
			vnode_->bb_->nonfull_wq_.wake_all();
		}
		if (type_ == file::pipe && writeable_) {
			vnode_->bb_->write_closed_ = true;
			vnode_->bb_->nonempty_wq_.wake_all();
		}
		vnode_->bb_->lock_.unlock(irqs);
	}

	auto refs = vnode_->refs_;
	vnode_->lock_.unlock(irqs);
	if (refs == 0) {
		kdelete(vnode_);
	}
}


// fdtable

fdtable::~fdtable() {
    for (unsigned i = 0; i < NFDS; i++) {
    	if (fds_[i]) {
        	fds_[i]->deref();
    	}
    }
}



// vn_keyboard_console

size_t vn_keyboard_console::read(uintptr_t buf, size_t sz) {
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


size_t vn_keyboard_console::write(uintptr_t buf, size_t sz) {
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



// vn_pipe

size_t vn_pipe::read(uintptr_t buf, size_t sz) {
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

    if (bb_->write_closed_) {
    	bb_->lock_.unlock(irqs);
    	return 0;
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


size_t vn_pipe::write(uintptr_t buf, size_t sz) {
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
