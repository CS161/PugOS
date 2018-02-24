#ifndef CHICKADEE_K_WAIT_HH
#define CHICKADEE_K_WAIT_HH
#include "kernel.hh"
#include "k-list.hh"
struct wait_queue;

void debug_printf_(const char* file, const char* func, int line,
                          const char* format, ...);
const char* state_string(const proc* p);
extern volatile unsigned long ticks;

struct waiter {
    proc* p_;
    wait_queue* wq_;
    list_links links_;

    inline waiter(proc* p);
    inline ~waiter();
    inline void prepare(wait_queue* wq);
    inline void block();
    inline void clear();
    inline void wake();
};


struct wait_queue {
    list<waiter, &waiter::links_> q_;
    spinlock lock_;

    // you might want to provide some convenience methods here
};


static const unsigned WHEEL_SPOKES = 8;
struct timing_wheel {
    wait_queue wqs_[WHEEL_SPOKES];
};


inline waiter::waiter(proc* p)
    : p_(p), wq_(nullptr) {
}

inline waiter::~waiter() {
    // optional error-checking code
}


// waiter::prepare(waitq)
//    This function prepares the waiter to sleep on the named wait queue, and
//    implements the “enqueue” part of blocking. Specifically, the function:
//    - Locks the waitq data structure.
//    - Sets p->state_ to proc::blocked. This means the state is blocked even
//      though the associated kernel task is running!
//    - Adds the waiter to a linked list of waiters associated with the waitq.
//    - Unlocks the waitq data structure.

inline void waiter::prepare(wait_queue* wq) {
    wq_ = wq;
    auto irqs = wq_->lock_.lock();
    // debug_printf_(__FILE__, __FUNCTION__, __LINE__,
    //     "waiter::prepare pid %d\n", p_->pid_);
    p_->state_ = proc::blocked;
    wq_->q_.push_back(this);
    wq_->lock_.unlock(irqs);
}


// waiter::block()
//    This function implements the “block” part of blocking. Specifically, the
//    function:
//    - Calls p->yield(). If the process still has state_ == blocked, then the
//      process will block.
//    - When the yield call returns, it calls waiter::clear() to clean up the
//      waiter.

inline void waiter::block() {
    // debug_printf_(__FILE__, __FUNCTION__, __LINE__,
    //     "blocking pid %d\n", p_->pid_);
    p_->yield();
    clear();
}


// waiter::clear()
//    This function cleans up the waiter after the process has woken up, by
//    undoing the effect of any preceding waiter::prepare. Specifically, the
//    function:
//    - Locks the waitq data structure.
//    - Sets p->state_ to proc::runnable.
//    - Removes the waiter from the linked list, if it is currently linked.
//    - Unlocks the waitq data structure.

inline void waiter::clear() {
    auto irqs = wq_->lock_.lock();
    // debug_printf_(__FILE__, __FUNCTION__, __LINE__,
    //     "waiter::clear pid %d\n", p_->pid_);
    p_->state_ = proc::runnable;
    if (links_.is_linked()) {
        wq_->q_.erase(this);
    }
    wq_->lock_.unlock(irqs);
}


// waiter::wake()
//    Wakes up all waiting processes currently on the wait queue for this waiter

inline void waiter::wake() {
    auto irqs = wq_->lock_.lock();
    while (auto w = wq_->q_.pop_front()) {
        // debug_printf_(__FILE__, __FUNCTION__, __LINE__,
        //     "waiter::wake pid %d, state %s, %sresumable\n",
        //     w->p_->pid_, state_string(w->p_), w->p_->resumable() ? "" : "not ");
        w->p_->wake();
    }
    wq_->lock_.unlock(irqs);
}
#endif
