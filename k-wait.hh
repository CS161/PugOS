#ifndef CHICKADEE_K_WAIT_HH
#define CHICKADEE_K_WAIT_HH
#include "kernel.hh"
#include "k-list.hh"
struct wait_queue;

extern wait_queue waitpid_wq;


struct waiter {
    proc* p_;
    wait_queue* wq_;
    list_links links_;

    inline waiter(proc* p);
    inline ~waiter();
    NO_COPY_OR_ASSIGN(waiter);
    inline void prepare(wait_queue& wq);
    inline void prepare(wait_queue* wq);
    inline void block();
    inline void clear();
    inline void wake();

    template <typename F>
    inline void block_until(wait_queue& wq, F predicate);
    template <typename F>
    inline irqstate block_until(wait_queue& wq, F predicate, spinlock& lock);
    template <typename F>
    inline void block_until(wait_queue& wq, F predicate,
                            spinlock& lock, irqstate& irqs);
};


struct wait_queue {
    list<waiter, &waiter::links_> q_;
    mutable spinlock lock_;

    // you might want to provide some convenience methods here
    inline void wake_all();
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

inline void waiter::prepare(wait_queue& wq) {
    prepare(&wq);
}

inline void waiter::prepare(wait_queue* wq) {
    auto irqs = wq->lock_.lock();
    // debug_printf_(__FILE__, __FUNCTION__, __LINE__,
    //     "waiter::prepare pid %d\n", p_->pid_);
    p_->state_ = proc::blocked;
    wq->q_.push_back(this);
    wq_ = wq;
    wq->lock_.unlock(irqs);
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
//    Wakes up all the process waiting on this waiter

inline void waiter::wake() {
    p_->wake();
}


// Forward declaration
proc* current();
void log_printf(const char* format, ...);

// waiter::block_until(wq, predicate)
//    Block on `wq` until `predicate()` returns true.
template <typename F>
inline void waiter::block_until(wait_queue& wq, F predicate) {
    proc* p;
    while (1) {
        prepare(wq);
        p = current();
        if (p->exiting_) {
            log_printf("block_until caught exiting thread %d\n", p->pid_);
            clear();
            p->state_ = proc::broken;
            waitpid_wq.wake_all();
            p->yield_noreturn();
        }
        else if (predicate()) {
            break;
        }
        block();
    }
    clear();
}

// waiter::block_until(wq, predicate, lock)
//    Lock `lock`, then block on `wq` until `predicate()` returns
//    true. All calls to `predicate` have `lock` locked. Returns
//    with `lock` locked; the return value is the relevant `irqstate`.
template <typename F>
inline irqstate waiter::block_until(wait_queue& wq, F predicate,
                                    spinlock& lock) {
    auto irqs = lock.lock();
    block_until(wq, predicate, lock, irqs);
    return std::move(irqs);
}

// waiter::block_until(wq, predicate, lock, irqs)
//    Block on `wq` until `predicate()` returns true. The `lock`
//    must be locked; it is unlocked before blocking (if blocking
//    is necessary). All calls to `predicate` have `lock` locked,
//    and `lock` is locked on return.
template <typename F>
inline void waiter::block_until(wait_queue& wq, F predicate,
                                spinlock& lock, irqstate& irqs) {
    proc* p;
    while (1) {
        prepare(wq);
        p = current();
        if (p->exiting_) {
            lock.unlock(irqs);
            log_printf("block_until caught exiting thread %d\n", p->pid_);
            clear();
            p->state_ = proc::broken;
            waitpid_wq.wake_all();
            p->yield_noreturn();
        }
        else if (predicate()) {
            break;
        }
        lock.unlock(irqs);
        block();
        irqs = lock.lock();
    }
    clear();
}

// wait_queue::wake_all()
//    Lock the wait queue, then clear it by waking all waiters.
inline void wait_queue::wake_all() {
    auto irqs = lock_.lock();
    while (auto w = q_.pop_front()) {
        w->wake();
    }
    lock_.unlock(irqs);
}

#endif
