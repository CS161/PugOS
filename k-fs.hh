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

	fdtable() : refs_(1), fds_{nullptr} { };
	~fdtable();
};



// fd 0-2 Keyboard/Console

struct vn_keyboard_console : vnode {
	const char* filename_ = "keyboard/console";
    virtual size_t read(uintptr_t buf, size_t sz);
    virtual size_t write(uintptr_t buf, size_t sz);
};


// pipes

#define BBUFFER_SIZE 128

struct bbuffer {
   char buf_[BBUFFER_SIZE];
   size_t pos_;
   size_t len_;
   int write_closed_;
   spinlock lock_;

   bbuffer() : buf_{0}, pos_(0), len_(0), write_closed_(0) { };
};

struct vn_pipe : vnode {
	const char* filename_ = "pipe";
	virtual size_t read(uintptr_t buf, size_t sz);
    virtual size_t write(uintptr_t buf, size_t sz);

  private:
  	bbuffer bb_;
};


#endif
