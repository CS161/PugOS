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
