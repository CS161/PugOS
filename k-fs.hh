#ifndef CHICKADEE_K_FS_HH
#define CHICKADEE_K_FS_HH
#include "kernel.hh"

#define NFDS 256


struct vnode {
	const char* filename_;
	
	// lock_ guards everything below it
	spinlock lock_;
	int refs_;
	size_t sz_;

	virtual size_t read(uintptr_t buf, size_t sz) { return E_PERM; };
	virtual size_t write(uintptr_t buf, size_t sz) { return E_PERM; };
	virtual int link() { return E_PERM; };
	virtual int unlink() { return E_PERM; };
	virtual int symlink() { return E_PERM; };
	virtual int creat() { return E_PERM; };
	virtual int mkdir() { return E_PERM; };
	virtual int rmdir() { return E_PERM; };
	virtual int rename() { return E_PERM; };

	vnode() : refs_(1), sz_(0) { };
};


struct file {
	enum type_t {
		normie, stream, directory
	};

	type_t type_;
	bool readable_;
	bool writeable_;
	vnode* vnode_;
	
	// lock_ guards everything below it
	spinlock lock_;
	int refs_; // for threading later
	int deref();
	size_t off_;

	file() : refs_(1), off_(0) { };
	~file();
};


struct fdtable {
	// lock_ guards everything below it
	spinlock lock_;
	int refs_; // for threading
	file* fds_[NFDS]; // LENGTH: global constant

	fdtable() : fds_{nullptr} { };
	~fdtable();
};



// fd 0-2 Keyboard/Console

struct vn_keyboard_console : vnode {
	const char* filename_ = "keyboard/console";
    virtual size_t read(uintptr_t buf, size_t sz);
    virtual size_t write(uintptr_t buf, size_t sz);
};


// // pipes

// struct bbuffer {
//    char buf_[BBUFFER_SIZE];
//    size_t pos_;
//    size_t len_;
//    int write_closed_;
//    spinlock lock_;

//    ssize_t read(char* buf, size_t sz);
//    ssize_t write()

//    bbuffer() : buf_{0}, pos_(0), len_(0), write_closed_(0) { };
// };


// ssize_t read(bbuffer* bb, char* buf, size_t sz) {
//     size_t pos = 0;
//     pthread_mutex_lock(&bb->mutex);
//     while (pos < sz) {
//         size_t ncopy = sz - pos;
//         if (ncopy > sizeof(bb->buf) - bb->pos) {
//             ncopy = sizeof(bb->buf) - bb->pos;
//         }
//         if (ncopy > bb->len) {
//             ncopy = bb->len;
//         }
//         memcpy(&buf[pos], &bb->buf[bb->pos], ncopy);
//         bb->pos = (bb->pos + ncopy) % sizeof(bb->buf);
//         bb->len -= ncopy;
//         pos += ncopy;
//         if (ncopy == 0) {
//             if (bb->write_closed || pos > 0) {
//                 break;
//             }
//             pthread_cond_wait(&bb->nonempty, &bb->mutex);
//         }
//     }
//     int write_closed = bb->write_closed;
//     pthread_mutex_unlock(&bb->mutex);
//     if (pos == 0 && sz > 0 && !write_closed) {
//         return -1;  // cannot happen
//     } else {
//         if (pos > 0) {
//             pthread_cond_broadcast(&bb->nonfull);
//         }
//         return pos;
//     }
// }


// ssize_t bbuffer_write(bbuffer* bb, const char* buf, size_t sz) {
//     size_t pos = 0;
//     pthread_mutex_lock(&bb->mutex);
//     assert(!bb->write_closed);
//     while (pos < sz) {
//         size_t bb_index = (bb->pos + bb->len) % sizeof(bb->buf);
//         size_t ncopy = sz - pos;
//         if (ncopy > sizeof(bb->buf) - bb_index) {
//             ncopy = sizeof(bb->buf) - bb_index;
//         }
//         if (ncopy > sizeof(bb->buf) - bb->len) {
//             ncopy = sizeof(bb->buf) - bb->len;
//         }
//         memcpy(&bb->buf[bb_index], &buf[pos], ncopy);
//         bb->len += ncopy;
//         pos += ncopy;
//         if (ncopy == 0) {
//             if (pos > 0) {
//                 break;
//             }
//             pthread_cond_wait(&bb->nonfull, &bb->mutex);
//         }
//     }
//     pthread_mutex_unlock(&bb->mutex);
//     if (pos == 0 && sz > 0) {
//         return -1;  // cannot happen
//     } else {
//         if (pos > 0) {
//             pthread_cond_broadcast(&bb->nonempty);
//         }
//         return pos;
//     }
// }


#endif
