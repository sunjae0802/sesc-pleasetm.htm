#include "libll/ThreadContext.h"
#include "FasTMManager.h"

using namespace std;

/////////////////////////////////////////////////////////////////////////////////////////
// FasTM-Abort coherence. Tries to mimic LogTM as closely as possible
/////////////////////////////////////////////////////////////////////////////////////////
FasTMAbort::FasTMAbort(const char tmStyle[], int32_t nProcs, int32_t line):
        HTMManager(tmStyle, nProcs, line),
        getSMsg("tm:getSMsg"),
        fwdGetSMsg("tm:fwdGetSMsg"),
        getMMsg("tm:getMMsg"),
        invMsg("tm:invMsg"),
        flushMsg("tm:flushMsg"),
        fwdGetSConflictMsg("tm:fwdGetSConflictMsg"),
        invConflictMsg("tm:invConflictMsg"),
        nackMsg("tm:nackMsg") {

    int totalSize = SescConf->getInt("TransactionalMemory", "totalSize");
    int assoc = SescConf->getInt("TransactionalMemory", "assoc");

    for(Pid_t pid = 0; pid < nProcs; pid++) {
        caches.push_back(new CacheAssocTM(totalSize, assoc, lineSize, 1));
    }
}

///
// Destructor for PrivateCache. Delete all allocated members
FasTMAbort::~FasTMAbort() {
    while(caches.size() > 0) {
        Cache* cache = caches.back();
        caches.pop_back();
        delete cache;
    }
}

///
// We have a conflict, so either NACK pid (the requester), or if the requester is higher priority,
// abort all conflicting
TMRWStatus FasTMAbort::handleConflicts(Pid_t pid, VAddr caddr, std::set<Pid_t>& conflicting) {
    Pid_t highestPid = INVALID_PID;
    std::set<Pid_t> surviving;
    for(Pid_t c: conflicting) {
        if(isHigherOrEqualPriority(pid, c)) {
            markTransAborted(c, pid, caddr, TM_ATYPE_DEFAULT);
        } else {
            surviving.insert(c);
            if(highestPid == INVALID_PID || isHigherOrEqualPriority(c, highestPid)) {
                highestPid = c;
            }
        }
    }

    if(surviving.size() > 0) {
        markTransAborted(pid, highestPid, caddr, TM_ATYPE_DEFAULT);
        conflicting = surviving;
        return TMRW_ABORT;
    } else {
        return TMRW_SUCCESS;
    }
}

FasTMAbort::Line* FasTMAbort::replaceLine(Pid_t pid, VAddr raddr) {
    Cache* cache = getCache(pid);
	VAddr  caddr = addrToCacheLine(raddr);
    VAddr  myTag = cache->calcTag(raddr);

    Line* line  = cache->findLine2Replace(raddr);
    if(line == nullptr) {
        fail("Replacement policy failed");
    }

    // Replace the line
    line->invalidate();
    line->validate(myTag, caddr);

    return line;
}

///
// Helper function that aborts all transactional readers and writers
TMRWStatus FasTMAbort::abortTMWriters(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except) {
    set<Pid_t> conflicting;
    rwSetManager.getWriters(caddr, conflicting);
    conflicting.erase(pid);

    // If any winners are around, we do conflict resolution
    if(conflicting.size() > 0) {
        if(isTM) {
            TMRWStatus status = handleConflicts(pid, caddr, conflicting);
            for(Pid_t c: conflicting) {
                except.insert(getCache(c));
            }
            return status;
        } else {
            markTransAborted(conflicting, pid, caddr, TM_ATYPE_NONTM);
            return TMRW_SUCCESS;
        }
    } else {
        return TMRW_SUCCESS;
    }
}
///
// Helper function that aborts all transactional readers and writers
TMRWStatus FasTMAbort::abortTMSharers(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except) {
    set<Pid_t> conflicting;
    rwSetManager.getWriters(caddr, conflicting);
    rwSetManager.getReaders(caddr, conflicting);
    conflicting.erase(pid);

    // If any winners are around, we do conflict resolution
    if(conflicting.size() > 0) {
        TMRWStatus status = TMRW_SUCCESS;
        if(isTM) {
            status = handleConflicts(pid, caddr, conflicting);
            for(Pid_t c: conflicting) {
                except.insert(getCache(c));
            }
            return status;
        } else {
            markTransAborted(conflicting, pid, caddr, TM_ATYPE_NONTM);
            return TMRW_SUCCESS;
        }
    } else {
        return TMRW_SUCCESS;
    }
}
///
// Helper function that cleans dirty lines in each cache except pid's.
void FasTMAbort::cleanDirtyLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except) {
    getSMsg.inc();

    Cache* myCache = getCache(pid);
    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        Line* line = cache->findLine(caddr);
        if(line && line->isValid() && line->isDirty()) {
            if(except.find(cache) == except.end()) {
                // TODO: Also for except set?
                if(line->isTransactional()) {
                    fwdGetSConflictMsg.inc();
                } else {
                    fwdGetSMsg.inc();
                }
                line->makeClean();
            } else if(cache == myCache) {
                // Requester's cache should be left alone
            } else {
                // In except set so leave line alone
                if(line->isTransactional()) {
                    fwdGetSConflictMsg.inc();
                    nackMsg.inc();
                } else {
                    fwdGetSMsg.inc();
                    nackMsg.inc();
                }
            }
        }
    }
}

///
// Helper function that invalidates lines except pid's.
void FasTMAbort::invalidateLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except) {
    getMMsg.inc();

    Cache* myCache = getCache(pid);
    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        Line* line = cache->findLine(caddr);
        if(line && line->isValid()) {
            if(except.find(cache) == except.end()) {
                if(line->isTransactional()) {
                    invConflictMsg.inc();
                } else {
                    invMsg.inc();
                }
                line->invalidate();
            } else if(cache == myCache) {
                // Requester's cache should be left alone
            } else {
                // In except set so leave line alone
                if(line->isTransactional()) {
                    invConflictMsg.inc();
                } else {
                    invMsg.inc();
                    nackMsg.inc();
                }
            }
        }
    }
}

///
// Do a transactional read.
TMRWStatus FasTMAbort::TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= getCache(pid);

    std::set<Cache*> except;
    TMRWStatus status = abortTMWriters(pid, caddr, true, except);

    if(status != TMRW_SUCCESS) {
        p_opStatus->wasNacked = true;
        cleanDirtyLines(pid, caddr, except);
        return status;
    }

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        cleanDirtyLines(pid, caddr, except);
        line  = replaceLine(pid, raddr);
    } else {
        p_opStatus->wasHit = true;
        if(line->isTransactional() == false && line->isDirty()) {
            // If we were the previous writer, make clean and start anew
            line->makeClean();
            flushMsg.inc();
        }
    }

    // Update line
    line->markTransactional();
    line->addReader(pid);

    return TMRW_SUCCESS;
}

///
// Do a transactional write.
TMRWStatus FasTMAbort::TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= getCache(pid);

    std::set<Cache*> except;
    TMRWStatus status = abortTMSharers(pid, caddr, true, except);

    if(status != TMRW_SUCCESS) {
        p_opStatus->wasNacked = true;
        invalidateLines(pid, caddr, except);
        return status;
    }

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        invalidateLines(pid, caddr, except);
        line  = replaceLine(pid, raddr);
    } else if(line->isDirty() == false) {
        p_opStatus->wasHit = false;
        invalidateLines(pid, caddr, except);
    } else {
        p_opStatus->wasHit = true;
    }

    // Update line
    line->markTransactional();
    line->makeTransactionalDirty(pid);

    return TMRW_SUCCESS;
}

///
// Do a non-transactional read, i.e. when a thread not inside a transaction.
void FasTMAbort::nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= getCache(pid);

    std::set<Cache*> except;
    TMRWStatus status = abortTMWriters(pid, caddr, false, except);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        cleanDirtyLines(pid, caddr, except);
        line  = replaceLine(pid, raddr);
    } else {
        p_opStatus->wasHit = true;
    }
}

///
// Do a non-transactional write, i.e. when a thread not inside a transaction.
void FasTMAbort::nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= getCache(pid);

    std::set<Cache*> except;
    TMRWStatus status = abortTMSharers(pid, caddr, false, except);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        invalidateLines(pid, caddr, except);
        line  = replaceLine(pid, raddr);
    } else if(line->isDirty() == false) {
        p_opStatus->wasHit = false;
        invalidateLines(pid, caddr, except);
    } else {
        p_opStatus->wasHit = true;
    }

    // Update line
    line->makeDirty();
}

///
// TM commit also clears firstStartTime
TMBCStatus FasTMAbort::myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    // On commit, we clear all transactional bits, but otherwise leave lines alone
    Pid_t pid   = context->getPid();
    Cache* cache = getCache(pid);

    p_opStatus->tmLat           = 4 + rwSetManager.getNumWrites(pid);
    p_opStatus->tmCommitSubtype =TM_COMMIT_REGULAR;

    LineTMComparator tmCmp;
    std::vector<Line*> lines;
    cache->collectLines(lines, tmCmp);

    for(Line* line: lines) {
        line->clearTransactional(pid);
    }

    return TMBC_SUCCESS;
}

void FasTMAbort::myStartAborting(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    // On abort, we need to throw away the work we've done so far, so invalidate them
    Pid_t pid   = context->getPid();
    Cache* cache = getCache(pid);

    LineTMComparator tmCmp;
    std::vector<Line*> lines;
    cache->collectLines(lines, tmCmp);

    for(Line* line: lines) {
        if(line->isDirty()) {
            line->invalidate();
        } else {
            line->clearTransactional(pid);
        }
    }
}
/////////////////////////////////////////////////////////////////////////////////////////
// FasTMAbort with more reads wins
/////////////////////////////////////////////////////////////////////////////////////////
FasTMAbortMoreReadsWins::FasTMAbortMoreReadsWins(const char tmStyle[], int32_t nProcs, int32_t line):
        FasTMAbort(tmStyle, nProcs, line) {
}

///
// Return true if pid is higher or equal priority than conflictPid
bool FasTMAbortMoreReadsWins::isHigherOrEqualPriority(Pid_t pid, Pid_t conflictPid) {
    return rwSetManager.getNumReads(conflictPid) < rwSetManager.getNumReads(pid);
}

/////////////////////////////////////////////////////////////////////////////////////////
// FasTMAbort with older wins
/////////////////////////////////////////////////////////////////////////////////////////
FasTMAbortOlderWins::FasTMAbortOlderWins(const char tmStyle[], int32_t nProcs, int32_t line):
        FasTMAbort(tmStyle, nProcs, line) {
}

///
// Return true if pid is higher or equal priority than conflictPid
bool FasTMAbortOlderWins::isHigherOrEqualPriority(Pid_t pid, Pid_t conflictPid) {
    Time_t otherTimestamp = getStartTime(conflictPid);
    Time_t myTimestamp = getStartTime(pid);

    return myTimestamp <= otherTimestamp;
}

///
// TM begin that also initializes firstStartTime
TMBCStatus FasTMAbortOlderWins::myBegin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    if(startTime.find(pid) == startTime.end()) {
        startTime[pid] = globalClock;
    }
    p_opStatus->tmBeginSubtype=TM_BEGIN_REGULAR;
    return TMBC_SUCCESS;
}

///
// TM commit also clears firstStartTime
TMBCStatus FasTMAbortOlderWins::myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    startTime.erase(pid);

    return FasTMAbort::myCommit(inst, context, p_opStatus);
}
///
// Clear firstStartTime when completing fallback, too
void FasTMAbortOlderWins::completeFallback(Pid_t pid) {
    startTime.erase(pid);
}

