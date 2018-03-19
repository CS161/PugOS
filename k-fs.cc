#include "kernel.hh"
#include "k-devices.hh"


// returns 1 if it freed itself, 0 if it still exists
int file::deref() {
	refs_--;
	assert(refs_ >= 0);
	if (refs_ == 0) {
		kdelete<file>(this);
		return 1;
	}
	else {
		return 0;
	}
}

file::~file() {
	vnode_->refs_--;
	assert(vnode_->refs_ >= 0);
	if (vnode_->refs_ == 0) {
		kdelete<vnode>(vnode_);
	}
}


fdtable::~fdtable() {
    for (unsigned i = 0; fds_[i] && i < NFDS; i++) {
        fds_[i]->deref();
    }
}



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



size_t vn_pipe::read(uintptr_t buf, size_t sz) {
    size_t pos = 0;
    char* input_buf = reinterpret_cast<char*>(buf);

    auto irqs = bb_.lock_.lock();
    while (pos < sz) {
        size_t ncopy = sz - pos;
        if (ncopy > sizeof(bb_.buf_) - bb_.pos_) {
            ncopy = sizeof(bb_.buf_) - bb_.pos_;
        }
        if (ncopy > bb_.len_) {
            ncopy = bb_.len_;
        }
        memcpy(&input_buf[pos], &bb_.buf_[bb_.pos_], ncopy);
        bb_.pos_ = (bb_.pos_ + ncopy) % sizeof(bb_.buf_);
        bb_.len_ -= ncopy;
        pos += ncopy;
        if (ncopy == 0) {
            if (bb_.write_closed_ || pos > 0) {
                break;
            }
            // pthread_cond_wait(&bb_.nonempty, &bb_.mutex);
        }
    }
    int write_closed = bb_.write_closed_;
    bb_.lock_.unlock(irqs);
    if (pos == 0 && sz > 0 && !write_closed) {
        return -1;  // cannot happen
    } else {
        if (pos > 0) {
            // pthread_cond_broadcast(&bb_.nonfull);
        }
        return pos;
    }
}


size_t vn_pipe::write(uintptr_t buf, size_t sz) {
    size_t pos = 0;
    const char* input_buf = reinterpret_cast<const char*>(buf);

    auto irqs = bb_.lock_.lock();
    assert(!bb_.write_closed_);
    while (pos < sz) {
        size_t bb_index = (bb_.pos_ + bb_.len_) % sizeof(bb_.buf_);
        size_t ncopy = sz - pos;
        if (ncopy > sizeof(bb_.buf_) - bb_index) {
            ncopy = sizeof(bb_.buf_) - bb_index;
        }
        if (ncopy > sizeof(bb_.buf_) - bb_.len_) {
            ncopy = sizeof(bb_.buf_) - bb_.len_;
        }
        memcpy(&bb_.buf_[bb_index], &input_buf[pos], ncopy);
        bb_.len_ += ncopy;
        pos += ncopy;
        if (ncopy == 0) {
            if (pos > 0) {
                break;
            }
            // pthread_cond_wait(&bb_.nonfull, &bb_.mutex);
        }
    }
    bb_.lock_.unlock(irqs);
    if (pos == 0 && sz > 0) {
        return -1;  // cannot happen
    } else {
        if (pos > 0) {
            // pthread_cond_broadcast(&bb_.nonempty);
        }
        return pos;
    }
}
