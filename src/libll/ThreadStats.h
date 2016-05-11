#ifndef THREAD_STATS_H
#define THREAD_STATS_H

#include <vector>
#include "estl.h"

// Forward decls to avoid circular includes
class DInst;
struct FuncBoundaryData;

// Struct that contains overall statistics about subsections within an atomic region.
struct AtomicRegionStats {
public:
    AtomicRegionStats();
    uint64_t totalAccounted() const;
    void reportValues() const;
    void sum(const AtomicRegionStats& other);

    uint64_t duration;
    uint64_t inMutex;
    uint64_t mutexQueue;
    uint64_t committed;
    uint64_t aborted;
    uint64_t nackStalled;
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

    AR_EVENT_MEMNACK_START      = 30, // Memory operation NACKED start event
    AR_EVENT_MEMNACK_END        = 31, // Memory operation NACKED end event

    AR_EVENT_ACTIVEFB_WAIT_BEGIN= 90, // Wait for active lock (calling wait(0))
    AR_EVENT_BACKOFF_BEGIN      = 91, // Random backoff       (calling wait(1))
    AR_EVENT_HOURGLASS_BEGIN    = 92, // Wait for hourglass   (calling wait(2))
    AR_EVENT_WAIT_END           = 99, // Wait end             (returning from wait)
};

// Pair of values that indicate an event within an atomic region
struct AtomicRegionEvents {
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
class AtomicRegionContext {
public:
    AtomicRegionContext() { clear(); }
    void clear();
    void init(Pid_t p, VAddr pc, Time_t at);
    void markEnd(Time_t at) {
        endAt = at;
    }
    void printEvents(const AtomicRegionEvents& current) const;
    void markRetireFuncBoundary(DInst* dinst, const FuncBoundaryData& funcData);
    void markRetireTM(DInst* dinst);
    void calculate(AtomicRegionStats* p_stats);
    void newAREvent(enum AREventType type);
    void newAREvent(enum AREventType type, Time_t at);

private:
    // Pid of the owner thread
    Pid_t               pid;
    // The start PC value
    VAddr               startPC;
    // The globalClock entry
    Time_t              startAt;
    // The globalClock exit
    Time_t              endAt;
    // List of events in this atomic region
    std::vector<AtomicRegionEvents> events;
};

class ThreadStats {
public:
    ThreadStats(): nRetiredInsts(0), nExedInsts(0), prevDInstRetired(0) {}
    static void initialize(Pid_t pid);
    static void markRetire(DInst* dinst);
    static void incNExedInsts(Pid_t pid) {
        getThread(pid).nExedInsts++;
    }

    static void report(const char *str);
    static ThreadStats& getThread(Pid_t pid) {
        return threadStats.at(pid);
    }
    size_t getNRetiredInsts(void) const {
        return nRetiredInsts;
    }
    size_t getNExedInsts(void) const {
        return nExedInsts;
    }
private:
    // Global stats map, one for each pid
    static HASH_MAP<Pid_t, ThreadStats> threadStats;
    // Holds the events for the current pid's atomic region
    AtomicRegionContext       currentRegion;
    // Holds the current pid's atomic region stats
    AtomicRegionStats         regionStats;
    // Number of retired DInsts during this thread's lifetime
    size_t    nRetiredInsts;
    // Number of executed DInsts during this thread's lifetime
    size_t    nExedInsts;
    // Time when the previous DInst for this thread had retired
    Time_t    prevDInstRetired;
};

#endif
