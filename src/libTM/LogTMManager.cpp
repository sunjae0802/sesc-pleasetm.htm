#include "libll/ThreadContext.h"
#include "LogTMManager.h"

using namespace std;

/////////////////////////////////////////////////////////////////////////////////////////
// IdealLogTM coherence. Tries to mimic LogTM as closely as possible
/////////////////////////////////////////////////////////////////////////////////////////
IdealLogTM::IdealLogTM(const char tmStyle[], int32_t nProcs, int32_t line):
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

    // Setting up nack RNG
    unsigned int randomSeed = SescConf->getInt("TransactionalMemory", "randomSeed");
    memset(rbuf, 0, RBUF_SIZE);
    initstate_r(randomSeed, rbuf, RBUF_SIZE, &randBuf);

    nackBase = SescConf->getInt("TransactionalMemory", "nackBase");
    nackCap = SescConf->getInt("TransactionalMemory", "nackCap");

    MSG("Using seed %d with %d/%d", randomSeed, nackBase, nackCap);

    for(Pid_t pid = 0; pid < nProcs; pid++) {
        caches.push_back(new CacheAssocTM(totalSize, assoc, lineSize, 1));
    }
}

///
// Destructor for PrivateCache. Delete all allocated members
IdealLogTM::~IdealLogTM() {
    while(caches.size() > 0) {
        Cache* cache = caches.back();
        caches.pop_back();
        delete cache;
    }
}

uint32_t IdealLogTM::getNackRetryStallCycles(ThreadContext* context) {
    Pid_t pid = context->getPid();
    uint32_t nackMax = nackBase;

    if(nackCount[pid] > 0) {
        if(nackCount[pid] > nackCap) {
            nackMax = nackBase * nackCap * nackCap;
        } else {
            nackMax = nackBase * nackCount[pid] * nackCount[pid];
        }
    }

    int32_t r = 0;
    random_r(&randBuf, &r);
    return ((r % nackMax) + 1);
}

///
// Return true if pid is higher or equal priority than conflictPid
bool IdealLogTM::isHigherOrEqualPriority(Pid_t pid, Pid_t conflictPid) {
    Time_t otherTimestamp = getStartTime(conflictPid);
    Time_t myTimestamp = getStartTime(pid);

    return myTimestamp <= otherTimestamp;
}

///
// We have a conflict, so either NACK pid (the requester), or if there is a circular NACK, abort
TMRWStatus IdealLogTM::handleConflicts(Pid_t pid, VAddr caddr, std::set<Pid_t>& conflicting) {
    Pid_t highestPid = INVALID_PID;
    Pid_t higherPid = INVALID_PID;
    for(Pid_t c: conflicting) {
        if(highestPid == INVALID_PID) {
            highestPid = c;
        } else if(isHigherOrEqualPriority(c, highestPid)) {
            highestPid = c;
        }

        if(isHigherOrEqualPriority(pid, c)) {
            // A lower c is sending an NACK to me
            cycleFlags[c] = true;
        }
        if(isHigherOrEqualPriority(c, pid)) {
            // I received a NACK from higher c
            higherPid = c;
        }
    }
    if(higherPid != INVALID_PID && cycleFlags[pid]) {
        markTransAborted(pid, higherPid, caddr, TM_ATYPE_DEFAULT);
        return TMRW_ABORT;
    } else {
        nackMsg.inc();

        nackCount[pid]++;
        nackedBy[pid] = highestPid;
        return TMRW_NACKED;
    }
}

IdealLogTM::Line* IdealLogTM::replaceLine(Pid_t pid, VAddr raddr) {
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
TMRWStatus IdealLogTM::abortTMWriters(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except) {
    set<Pid_t> conflicting;
    rwSetManager.getWriters(caddr, conflicting);
    conflicting.erase(pid);

    // If any winners are around, we self abort and add them to the except set
    if(conflicting.size() > 0) {
        for(Pid_t c: conflicting) {
            except.insert(getCache(c));
        }
        if(isTM) {
            return handleConflicts(pid, caddr, conflicting);
        } else {
            markTransAborted(conflicting, pid, caddr, TM_ATYPE_NONTM);
            return TMRW_SUCCESS;
        }
    } else {
        except.insert(getCache(pid));
        return TMRW_SUCCESS;
    }
}
///
// Helper function that aborts all transactional readers and writers
TMRWStatus IdealLogTM::abortTMSharers(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except) {
    set<Pid_t> conflicting;
    rwSetManager.getWriters(caddr, conflicting);
    rwSetManager.getReaders(caddr, conflicting);
    conflicting.erase(pid);

    // If any winners are around, we self abort and add them to the except set
    if(conflicting.size() > 0) {
        for(Pid_t c: conflicting) {
            except.insert(getCache(c));
        }
        if(isTM) {
            return handleConflicts(pid, caddr, conflicting);
        } else {
            markTransAborted(conflicting, pid, caddr, TM_ATYPE_NONTM);
            return TMRW_SUCCESS;
        }
    } else {
        except.insert(getCache(pid));
        return TMRW_SUCCESS;
    }
}
///
// Helper function that cleans dirty lines in each cache except pid's.
void IdealLogTM::cleanDirtyLines(VAddr raddr, std::set<Cache*>& except) {
    getSMsg.inc();

    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        if(except.find(cache) == except.end()) {
            Line* line = cache->findLine(raddr);
            if(line && line->isValid() && line->isDirty()) {
                if(line->isTransactional()) {
                    fwdGetSConflictMsg.inc();
                } else {
                    fwdGetSMsg.inc();
                }
                line->makeClean();
            }
        }
    }
}

///
// Helper function that invalidates lines except pid's.
void IdealLogTM::invalidateLines(VAddr raddr, std::set<Cache*>& except) {
    getMMsg.inc();

    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        if(except.find(cache) == except.end()) {
            Line* line = cache->findLine(raddr);
            if(line && line->isValid()) {
                if(line->isTransactional()) {
                    invConflictMsg.inc();
                } else {
                    invMsg.inc();
                }
                line->invalidate();
            }
        }
    }
}

///
// Do a transactional read.
TMRWStatus IdealLogTM::TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= getCache(pid);

    std::set<Cache*> except;
    TMRWStatus status = abortTMWriters(pid, caddr, true, except);

    if(status != TMRW_SUCCESS) {
        cleanDirtyLines(caddr, except);
        return status;
    }

    // Clear nackCount if we go through
    nackCount[pid] = 0;
    nackedBy.erase(pid);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        cleanDirtyLines(caddr, except);
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
TMRWStatus IdealLogTM::TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= getCache(pid);

    std::set<Cache*> except;
    TMRWStatus status = abortTMSharers(pid, caddr, true, except);

    if(status != TMRW_SUCCESS) {
        invalidateLines(caddr, except);
        return status;
    }

    // Clear nackCount if we go through
    nackCount[pid] = 0;
    nackedBy.erase(pid);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        invalidateLines(caddr, except);
        line  = replaceLine(pid, raddr);
    } else if(line->isDirty() == false) {
        p_opStatus->wasHit = false;
        invalidateLines(caddr, except);
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
void IdealLogTM::nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= getCache(pid);

    std::set<Cache*> except;
    TMRWStatus status = abortTMWriters(pid, caddr, false, except);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        cleanDirtyLines(caddr, except);
        line  = replaceLine(pid, raddr);
    } else {
        p_opStatus->wasHit = true;
    }
}

///
// Do a non-transactional write, i.e. when a thread not inside a transaction.
void IdealLogTM::nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= getCache(pid);

    std::set<Cache*> except;
    TMRWStatus status = abortTMSharers(pid, caddr, false, except);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        invalidateLines(caddr, except);
        line  = replaceLine(pid, raddr);
    } else if(line->isDirty() == false) {
        p_opStatus->wasHit = false;
        invalidateLines(caddr, except);
    } else {
        p_opStatus->wasHit = true;
    }

    // Update line
    line->makeDirty();
}

///
// TM begin that also initializes firstStartTime
TMBCStatus IdealLogTM::myBegin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    if(startTime.find(pid) == startTime.end()) {
        startTime[pid] = globalClock;
    }
    p_opStatus->tmBeginSubtype=TM_BEGIN_REGULAR;
    return TMBC_SUCCESS;
}
///
// TM commit also clears firstStartTime
TMBCStatus IdealLogTM::myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
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

	cycleFlags[pid] = false;
    nackCount[pid] = 0;
    nackedBy.erase(pid);
    startTime.erase(pid);

    return TMBC_SUCCESS;
}
///
// Clear firstStartTime when completing fallback, too
void IdealLogTM::completeFallback(Pid_t pid) {
    startTime.erase(pid);
}

void IdealLogTM::myStartAborting(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
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
	cycleFlags[pid] = false;
    nackCount[pid] = 0;
    nackedBy.erase(pid);
}

