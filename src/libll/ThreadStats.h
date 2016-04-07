#ifndef THREAD_STATS_H
#define THREAD_STATS_H

#include <vector>

class DInst;
struct FuncBoundaryData;

// Struct that contains overall statistics about subsections within an atomic region.
struct TimeTrackerStats {
    TimeTrackerStats();
    uint64_t totalAccounted() const;
    void print() const;
    void sum(const TimeTrackerStats& other);

    uint64_t duration;
    uint64_t inMutex;
    uint64_t mutexQueue;
    uint64_t committed;
    uint64_t aborted;
    uint64_t activeFBWait;
    uint64_t backoffWait;
    uint64_t hgWait;
};

// Enum of various atomic region events
enum AREventType {
    AR_EVENT_INVALID            = 0, // Invalid, uninitialized event
    AR_EVENT_HTM_BEGIN          = 1, // HTM begin event
    AR_EVENT_HTM_ABORT          = 2, // HTM abort event
    AR_EVENT_HTM_COMMIT         = 3, // HTM commit event

    AR_EVENT_LOCK_REQUEST       = 10, // Lock request event (calling lock)
    AR_EVENT_LOCK_ACQUIRE       = 11, // Lock acquire event (returning from lock)
    AR_EVENT_LOCK_RELEASE       = 12, // Lock release event (calling  unlock)

    AR_EVENT_ACTIVEFB_WAIT_BEGIN= 90, // Wait for active lock (calling wait(0))
    AR_EVENT_BACKOFF_BEGIN      = 91, // Random backoff       (calling wait(1))
    AR_EVENT_HOURGLASS_BEGIN    = 92, // Wait for hourglass   (calling wait(2))
    AR_EVENT_WAIT_END           = 99, // Wait end             (returning from wait)

    AR_EVENT_NUM_TYPES
};

// Pair of values that indicate an event within an atomic region
class AtomicRegionEvents {
public:
    AtomicRegionEvents(enum AREventType t, Time_t at): type(t), timestamp(at) {}
    enum AREventType getType() const { return type; }
    Time_t getTimestamp() const { return timestamp; }
private:
    enum AREventType type;
    Time_t timestamp;
};

// Used to track timing statistics of atomic regions (between tm_begin and tm_end).
// All timing is done at retire-time of DInsts.
struct AtomicRegionStats {
    AtomicRegionStats() { clear(); }
    void clear();
    void init(Pid_t p, VAddr pc, Time_t at);
    void markEnd(Time_t at) {
        endAt = at;
    }
    void printEvents(const AtomicRegionEvents& current) const;
    void markRetireFuncBoundary(DInst* dinst, const FuncBoundaryData& funcData);
    void markRetireTM(DInst* dinst);
    void calculate(TimeTrackerStats* p_stats);
    void newAREvent(enum AREventType type);

    Pid_t               pid;
    VAddr               startPC;
    Time_t              startAt;
    Time_t              endAt;
    std::vector<AtomicRegionEvents> events;
};

#endif
