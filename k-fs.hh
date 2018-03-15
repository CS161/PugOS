#ifndef CHICKADEE_K_FS_HH
#define CHICKADEE_K_FS_HH

const unsigned NFDS = 256;


struct vnode {
	const char* filename_;
	
	// lock_ guards everything below it
	spinlock lock_;
	int refs_;
	size_t sz_;

	virtual size_t read(void* buf, size_t sz, off_t offset);
	virtual size_t write(void* buf, size_t sz, off_t offset);
	virtual int link();
	virtual int unlink();
	virtual int symlink();
	virtual int creat();
	virtual int mkdir();
	virtual int rmdir();
	virtual int rename();
};


struct file {
	enum type_t {
		normie, pipe, directory
	};

	const type_t type_;
	const bool readable_;
	const bool writeable_;
	vnode* vnode_;
	
	// lock_ guards everything below it
	spinlock lock_;
	int refs_; // for threading later
	size_t off_;
};


struct fdtable {
	// lock_ guards everything below it
	spinlock lock_;
	int refs_;
	file* fds_[NFDS]; // LENGTH: global constant
};


#endif