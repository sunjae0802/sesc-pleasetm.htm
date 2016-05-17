#include "libcore/DInst.h"
#include "ThreadContext.h"
#include "ReportGen.h"
#include "ThreadStats.h"

using namespace std;
HASH_MAP<Pid_t, ThreadStats> ThreadStats::threadStats;

AtomicRegionStats::AtomicRegionStats():
    duration(0),
    inMutex(0),
    mutexQueue(0),
    committed(0),
    aborted(0),
    nackStalled(0),
    activeFBWait(0),
    backoffWait(0),
    hgWait(0)
{
}
uint64_t AtomicRegionStats::totalAccounted() const {
    return inMutex
        + mutexQueue
        + committed
        + aborted
        + nackStalled
        + activeFBWait
        + backoffWait
        + hgWait
    ;
}

void AtomicRegionStats::reportValues() const {
    if(totalAccounted() > duration) {
        fail("Accounted cycles is too high");
    }

    uint64_t totalOther = duration - totalAccounted();

    Report::field("tt_inMutex=%llu",      inMutex);
    Report::field("tt_mutexQueue=%llu",   mutexQueue);
    Report::field("tt_committed=%llu",    committed);
    Report::field("tt_aborted=%llu",      aborted);
    Report::field("tt_activeFBWait=%llu", activeFBWait);
    Report::field("tt_backoffWait=%llu",  backoffWait);
    Report::field("tt_hgWait=%llu",       hgWait);
    Report::field("tt_nackStalled=%llu",  nackStalled);
    Report::field("tt_other=%llu",        totalOther);
}

/// Add other to this statistics structure
void AtomicRegionStats::sum(const AtomicRegionStats& other) {
    duration        += other.duration;
    inMutex         += other.inMutex;
    mutexQueue      += other.mutexQueue;
    committed       += other.committed;
    aborted         += other.aborted;
    nackStalled     += other.nackStalled;
    activeFBWait    += other.activeFBWait;
    backoffWait     += other.backoffWait;
    hgWait          += other.hgWait;
}

// Uninitialize context structure
void AtomicRegionContext::clear() {
    pid = INVALID_PID;
    startPC = startAt = endAt = 0;
    events.clear();
}
// Initalize context structure
void AtomicRegionContext::init(Pid_t p, VAddr pc, Time_t at) {
    clear();
    pid     = p;
    startPC = pc;
    startAt = at;
}
// Create new AtomicRegion event
void AtomicRegionContext::newAREvent(enum AREventType type) {
    AtomicRegionEvents newEvent(type, globalClock);
    events.push_back(newEvent);
}
// Create new AtomicRegion event at a given time
void AtomicRegionContext::newAREvent(enum AREventType type, Time_t at) {
    AtomicRegionEvents newEvent(type, at);
    events.push_back(newEvent);
}

/// If the DInst is at a function boundary, update statistics
void AtomicRegionContext::markRetireFuncBoundary(DInst* dinst, const FuncBoundaryData& funcData) {
    switch(funcData.funcName) {
        case FUNC_TM_BEGIN_FALLBACK:
            if(funcData.isCall) {
                newAREvent(AR_EVENT_LOCK_REQUEST);
            } else {
                newAREvent(AR_EVENT_LOCK_ACQUIRE);
            }
            break;
        case FUNC_TM_END_FALLBACK:
            if(funcData.isCall) {
                newAREvent(AR_EVENT_LOCK_RELEASE);
            }
            break;
        case FUNC_TM_WAIT:
            if(funcData.isCall) {
                if(funcData.arg0 == 0) {
                    newAREvent(AR_EVENT_ACTIVEFB_WAIT_BEGIN);
                } else if(funcData.arg0 == 1) {
                    newAREvent(AR_EVENT_BACKOFF_BEGIN);
                } else if(funcData.arg0 == 2) {
                    newAREvent(AR_EVENT_HOURGLASS_BEGIN);
                } else {
                    fail("Unhandled wait arg! %d\n", funcData.arg0);
                }
            } else {
                newAREvent(AR_EVENT_WAIT_END);
            }
            break;
        default:
            // Do nothing
            break;
    } // end switch(funcName)
}

/// If the DInst is a TM instruction, update statistics
void AtomicRegionContext::markRetireTM(DInst* dinst) {
    Pid_t pid = dinst->context->getPid();

    if(dinst->tmAbortCompleteOp()) {
        newAREvent(AR_EVENT_HTM_ABORT);
    } else if(dinst->tmBeginOp()) {
        switch(dinst->getTMBeginSubtype()) {
            case TM_BEGIN_REGULAR:
                newAREvent(AR_EVENT_HTM_BEGIN);
                break;
            default:
                fail("Unhandled tmBeginSubtype: %d\n", dinst->getTMBeginSubtype());
        }
    } else if(dinst->tmCommitOp()) {
        switch(dinst->getTMCommitSubtype()) {
            case TM_COMMIT_REGULAR:
                newAREvent(AR_EVENT_HTM_COMMIT);
                break;
            case TM_COMMIT_ABORTED:
                newAREvent(AR_EVENT_HTM_ABORT);
                break;
            default:
                fail("Unhandled tmCommitSubtype: %d\n", dinst->getTMCommitSubtype());
        }
    }
}

// Print all the events stored in an AtomicRegion, with ``current'' marked
void AtomicRegionContext::printEvents(const AtomicRegionEvents& current) const {
    std::cout << pid << ": [B 0], ";
    for(size_t eid = 0; eid < events.size(); eid++) {
        const AtomicRegionEvents& event = events.at(eid);
        if(current.getTimestamp() == event.getTimestamp()) {
            std::cout << "{" << event.getType() << " " << (event.getTimestamp() - startAt) << "}, ";
        } else {
            std::cout << "[" << event.getType() << " " << (event.getTimestamp() - startAt) << "], ";
        }
    }
    std::cout << "[E " << (endAt - startAt) << "]";
    std::cout << std::endl;
}

/// Add this region's statistics to p_stats.
void AtomicRegionContext::calculate(AtomicRegionStats* p_stats) {
    // Make sure the stats are valid
    if(startPC == 0) {
        fail("Un-initialized region\n");
    }
    if(startAt == 0 || endAt == 0) {
        fail("Unclosed region\n");
    }

    // Handle subregions events
    size_t eid = 0;
    size_t INVALID_EID = events.size() + 1;
    size_t htm_begin_eid = INVALID_EID;
    uint64_t totalMemNacked = 0;
    while(eid < events.size()) {
        const AtomicRegionEvents& event = events.at(eid);
        switch(event.getType()) {
            case AR_EVENT_HTM_BEGIN: {
                if(totalMemNacked > 0) {
                    printEvents(event);
                    fail("[%d] begin started with leftover nacks\n", pid);
                }
                htm_begin_eid = eid;
                eid += 1;
                break;
            }
            case AR_EVENT_HTM_ABORT: {
                if(htm_begin_eid == INVALID_EID) {
                    printEvents(event);
                    fail("[%d] abort should be proceeded by begin\n", pid);
                }
                const AtomicRegionEvents& beginEvent = events.at(htm_begin_eid);

                p_stats->aborted   += event.getTimestamp() - beginEvent.getTimestamp();

                htm_begin_eid = INVALID_EID;
                totalMemNacked = 0;
                eid += 1;
                break;
            }
            case AR_EVENT_HTM_COMMIT: {
                if(htm_begin_eid == INVALID_EID) {
                    printEvents(event);
                    fail("[%d] commit should be proceeded by begin\n", pid);
                }
                const AtomicRegionEvents& beginEvent = events.at(htm_begin_eid);

                p_stats->committed += event.getTimestamp() - beginEvent.getTimestamp();

                htm_begin_eid = INVALID_EID;
                totalMemNacked = 0;
                eid += 1;
                break;
            }
            case AR_EVENT_MEMNACK_START: {
                const AtomicRegionEvents& nackEndEvent = events.at(eid + 1);
                if(nackEndEvent.getType() != AR_EVENT_MEMNACK_END) {
                    printEvents(nackEndEvent);
                    fail("[%d] nack start doesn't have an end\n", pid);
                }
                totalMemNacked += nackEndEvent.getTimestamp() - event.getTimestamp();
                eid += 2;
                break;
            }
            case AR_EVENT_LOCK_REQUEST: {
                // Lock Request/Acquire/Release
                if(eid + 2 >= events.size()) {
                    fail("Lock end event is not found\n");
                }
                const AtomicRegionEvents& lockAcqEvent = events.at(eid + 1);
                const AtomicRegionEvents& lockRelEvent = events.at(eid + 2);
                if(lockAcqEvent.getType() != AR_EVENT_LOCK_ACQUIRE) {
                    fail("Unknown event after lock request: %d\n", lockAcqEvent.getType());
                }
                if(lockRelEvent.getType() != AR_EVENT_LOCK_RELEASE) {
                    fail("Unknown event after lock release: %d\n", lockRelEvent.getType());
                }

                p_stats->mutexQueue += lockAcqEvent.getTimestamp() - event.getTimestamp();
                p_stats->inMutex    += lockRelEvent.getTimestamp() - lockAcqEvent.getTimestamp();
                eid += 3;
                break;
            }
            case AR_EVENT_ACTIVEFB_WAIT_BEGIN: {
                // Wait Events
                if(eid + 1 >= events.size()) {
                    fail("wait end event is not found\n");
                }
                const AtomicRegionEvents& endEvent = events.at(eid + 1);
                if(endEvent.getType() != AR_EVENT_WAIT_END) {
                    fail("Unknown event after wait begin: %d\n", endEvent.getType());
                }
                p_stats->activeFBWait   +=  endEvent.getTimestamp() - event.getTimestamp();
                eid += 2;
                break;
            }
            case AR_EVENT_BACKOFF_BEGIN: {
                // Wait Events
                if(eid + 1 >= events.size()) {
                    fail("wait end event is not found\n");
                }
                const AtomicRegionEvents& endEvent = events.at(eid + 1);
                if(endEvent.getType() != AR_EVENT_WAIT_END) {
                    fail("Unknown event after backoff begin: %d\n", endEvent.getType());
                }
                p_stats->backoffWait +=  endEvent.getTimestamp() - event.getTimestamp();
                eid += 2;
                break;
            }
            case AR_EVENT_HOURGLASS_BEGIN: {
                // Wait Events
                if(eid + 1 >= events.size()) {
                    fail("wait end event is not found\n");
                }
                const AtomicRegionEvents& endEvent = events.at(eid + 1);
                if(endEvent.getType() != AR_EVENT_WAIT_END) {
                    fail("Unknown event after backoff begin: %d\n", endEvent.getType());
                }
                p_stats->hgWait +=  endEvent.getTimestamp() - event.getTimestamp();
                eid += 2;
                break;
            }
            default:
                printEvents(event);
                fail("Unknown event: %d\n", event.getType());
        }
    }

    if(totalMemNacked > 0) {
        printEvents(events.at(0));
        fail("[%d] sequence ended with leftover nacks\n", pid);
    }

    // Compute total length
    p_stats->duration += endAt - startAt;

    if(p_stats->totalAccounted() > p_stats->duration) {
        printEvents(events.at(0));
        fail("[%d] Accounted cycles is too high", pid);
    }
}

/// Initialize the threadStats for pid.
void ThreadStats::initialize(Pid_t pid) {
    threadStats.insert(make_pair(pid, ThreadStats()));
}

/// Print stats of all threads.
void ThreadStats::report(const char* str) {
    Report::field("BEGIN ThreadStats::report %s", str);
    AtomicRegionStats allStats;

    HASH_MAP<Pid_t, ThreadStats>::const_iterator iStats;
    for(iStats = threadStats.begin(); iStats != threadStats.end(); ++iStats) {
        allStats.sum(iStats->second.regionStats);
    }

    allStats.reportValues();
    Report::field("END ThreadStats::report %s", str);
}

/// Keep track of statistics for each retired DInst.
void ThreadStats::markRetire(DInst* dinst) {
    const Instruction* inst = dinst->getInst();
    Pid_t pid               = dinst->context->getPid();
    ThreadStats& myStats    = getThread(pid);

    // Everything below is working on myStats as `this'
    myStats.nRetiredInsts++;

    if(inst->isTM()) {
        myStats.currentRegion.markRetireTM(dinst);
    }
    if(dinst->getTMMemopHadStalled()) {
        myStats.currentRegion.newAREvent(AR_EVENT_MEMNACK_START, myStats.prevDInstRetired);
        myStats.currentRegion.newAREvent(AR_EVENT_MEMNACK_END);
    }

    // Track function boundaries, by for example initializing and ending atomic regions.
    for(std::vector<FuncBoundaryData>::const_iterator i_funcData = dinst->getInstContext().funcData.begin();
            i_funcData != dinst->getInstContext().funcData.end(); ++i_funcData) {
        switch(i_funcData->funcName) {
            case FUNC_TM_BEGIN:
                myStats.currentRegion.init(pid, dinst->getInst()->getAddr(), globalClock);
                break;
            case FUNC_TM_END: {
                AtomicRegionStats currentStats;
                myStats.currentRegion.markEnd(globalClock);
                myStats.currentRegion.calculate(&currentStats);
                myStats.currentRegion.clear();

                myStats.regionStats.sum(currentStats);
                break;
            }
            default:
                myStats.currentRegion.markRetireFuncBoundary(dinst, *i_funcData);
                break;
        } // end switch(funcName)
    } // end foreach funcBoundaryData

    myStats.prevDInstRetired = globalClock;
}
