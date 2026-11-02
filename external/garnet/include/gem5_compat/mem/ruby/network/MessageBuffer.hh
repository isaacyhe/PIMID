/**
 * @file MessageBuffer.hh
 * @brief Message buffer for network interface
 */

#ifndef __GARNET_COMPAT_MEM_RUBY_NETWORK_MESSAGEBUFFER_HH__
#define __GARNET_COMPAT_MEM_RUBY_NETWORK_MESSAGEBUFFER_HH__

#include <cassert>
#include <deque>
#include <functional>
#include <memory>
#include <string>

#include "base/types.hh"
#include "sim/clocked_object.hh"
#include "../slicc_interface/Message.hh"

namespace gem5 {
namespace ruby {

// Forward declaration
class Consumer;

/**
 * MessageBuffer - queue for messages between protocol and network
 */
class MessageBuffer : public ClockedObject {
public:
    struct Params : public ClockedObject::Params {
        bool ordered = false;
        int max_msgs = 0;  // 0 = unlimited
        std::string vnet_type;
    };

    MessageBuffer(const Params& p)
        : ClockedObject(p),
          ordered_(p.ordered),
          maxMsgs_(p.max_msgs),
          vnetType_(p.vnet_type),
          consumer_(nullptr),
          vnet_(-1) {}

    virtual ~MessageBuffer() = default;

    // Register consumer for wakeups
    void setConsumer(Consumer* consumer) { consumer_ = consumer; }

    // Ready hook: while set, every enqueue lowers *hook to the new message's
    // ready time if that is earlier. A reader that polls several buffers keeps
    // a lower bound on their front ready times this way and polls only once
    // the current tick reaches it. Null (the default) leaves enqueue as it was.
    void setReadyHook(Tick* hook) { readyHook_ = hook; }

    // Set virtual network ID
    void setVnet(int vnet) { vnet_ = vnet; }
    int getVnet() const { return vnet_; }

    // Check if buffer is ready (has messages)
    bool isReady(Tick current_time) const {
        return !buffer_.empty() && buffer_.front().ready_time <= current_time;
    }

    // Check if buffer has any messages
    bool hasMessages() const { return !buffer_.empty(); }

    // Check if N slots are available in the buffer
    bool areNSlotsAvailable(int n, Tick current_time) const {
        if (maxMsgs_ == 0) return true;  // Unlimited
        return (buffer_.size() + n) <= static_cast<size_t>(maxMsgs_);
    }

    // Get message ready time
    Tick readyTime() const {
        if (buffer_.empty()) return MaxTick;
        return buffer_.front().ready_time;
    }

    // Peek at front message without removing
    const MsgPtr& peekMsgPtr() const {
        assert(!buffer_.empty());
        return buffer_.front().msg;
    }

    // Dequeue message
    MsgPtr dequeue(Tick current_time) {
        assert(!buffer_.empty());
        assert(buffer_.front().ready_time <= current_time);

        MsgPtr msg = buffer_.front().msg;
        buffer_.pop_front();

        dequeues_++;

        // Notify a registered consumer that a slot just freed. A NetworkInterface
        // that had to STALL an arriving tail flit (because this protocol buffer
        // was full) registers a dequeue callback so it can retry delivery and
        // return the held VC credit once space reopens. Real gem5's
        // MessageBuffer::dequeue fires this; this compat shim previously omitted
        // it, so the NI never recovered -> the destination sink stayed
        // credit-starved -> the up/down DRAM tree HARD DEADLOCKED under all-to-all
        // traffic (dump: ~9 packets stuck at "out Local, freeVCinCls=1 credit=0",
        // back-pressuring the whole chokepoint). The NI callback only schedules a
        // next-cycle wakeup (no re-entrant buffer mutation), so firing it here is
        // safe. (`dequeueCallback_` is null unless the NI is actually stalled.)
        if (dequeueCallback_) dequeueCallback_();
        return msg;
    }

    // Enqueue message with delay
    void enqueue(MsgPtr msg, Tick current_time, Tick delta) {
        Tick ready_time = current_time + delta;

        if (ordered_) {
            // Ordered - add at back
            buffer_.push_back({msg, ready_time});
        } else {
            // Unordered - insert by ready time
            auto it = buffer_.begin();
            while (it != buffer_.end() && it->ready_time <= ready_time) {
                ++it;
            }
            buffer_.insert(it, {msg, ready_time});
        }

        enqueues_++;
        if (readyHook_ && ready_time < *readyHook_) *readyHook_ = ready_time;

        // Schedule consumer wakeup
        if (consumer_) {
            consumer_->scheduleEventAbsolute(ready_time);
        }
    }

    // Enqueue with randomization and warmup flags (gem5 compatibility)
    void enqueue(MsgPtr msg, Tick current_time, Tick delta,
                 bool randomization, bool warmup_enabled) {
        // Ignore randomization and warmup for standalone
        enqueue(msg, current_time, delta);
    }

    // Register dequeue callback
    void registerDequeueCallback(std::function<void()> callback) {
        dequeueCallback_ = callback;
    }

    // Unregister dequeue callback
    void unregisterDequeueCallback() {
        dequeueCallback_ = nullptr;
    }

    // Clear buffer
    void clear() {
        buffer_.clear();
    }

    // Statistics
    uint64_t getEnqueues() const { return enqueues_; }
    uint64_t getDequeues() const { return dequeues_; }
    size_t getSize() const { return buffer_.size(); }

    void print(std::ostream& out) const {
        out << "MessageBuffer [size=" << buffer_.size() << "]";
    }

private:
    struct BufferEntry {
        MsgPtr msg;
        Tick ready_time;
    };

    std::deque<BufferEntry> buffer_;
    bool ordered_;
    int maxMsgs_;
    std::string vnetType_;
    Consumer* consumer_;
    int vnet_;
    std::function<void()> dequeueCallback_;
    Tick* readyHook_ = nullptr;

    // Statistics
    uint64_t enqueues_ = 0;
    uint64_t dequeues_ = 0;
};

inline std::ostream& operator<<(std::ostream& out, const MessageBuffer& buf) {
    buf.print(out);
    return out;
}

} // namespace ruby
} // namespace gem5

#endif // __GARNET_COMPAT_MEM_RUBY_NETWORK_MESSAGEBUFFER_HH__
