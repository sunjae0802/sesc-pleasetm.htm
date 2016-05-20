#include "libll/ThreadContext.h"
#include "PleaseTMManager.h"
#include "Snippets.h"

using namespace std;

/////////////////////////////////////////////////////////////////////////////////////////
// PleaseTM with conflict resolution done with plea bits
/////////////////////////////////////////////////////////////////////////////////////////
PleaseTM::PleaseTM(const char tmStyle[], int32_t nCores, int32_t line):
        HTMManager(tmStyle, nCores, line),
        getSMsg("tm:getSMsg"),
        fwdGetSMsg("tm:fwdGetSMsg"),
        getMMsg("tm:getMMsg"),
        invMsg("tm:invMsg"),
        flushMsg("tm:flushMsg"),
        fwdGetSConflictMsg("tm:fwdGetSConflictMsg"),
        invConflictMsg("tm:invConflictMsg"),
        rfchSuccMsg("tm:rfchSuccMsg"),
        rfchFailMsg("tm:rfchFailMsg") {

    int totalSize = SescConf->getInt("TransactionalMemory", "totalSize");
    int assoc = SescConf->getInt("TransactionalMemory", "assoc");
    if(SescConf->checkInt("TransactionalMemory","overflowSize")) {
        maxOverflowSize = SescConf->getInt("TransactionalMemory","overflowSize");
    } else {
        maxOverflowSize = 4;
        MSG("Using default overflow size of %ld\n", maxOverflowSize);
    }

    for(int coreId = 0; coreId < nCores; coreId++) {
        caches.push_back(new CacheAssocTM(totalSize, assoc, lineSize, 1));
    }
}

///
// Destructor for PrivateCache. Delete all allocated members
PleaseTM::~PleaseTM() {
    while(caches.size() > 0) {
        Cache* cache = caches.back();
        caches.pop_back();
        delete cache;
    }
}

///
// If this line had been sent to the overflow set, bring it back.
void PleaseTM::updateOverflow(Pid_t pid, VAddr newCaddr) {
    set<Pid_t> peers;
    getPeers(pid, peers);

    for(Pid_t peer: peers) {
        if(overflow[peer].find(newCaddr) != overflow[peer].end()) {
            overflow[peer].erase(newCaddr);
        }
    }
}

///
// Return the set of co-located Pids for SMT
void PleaseTM::getPeers(Pid_t pid, std::set<Pid_t>& peers) {
    peers.insert(pid);

    if(nSMTWays == 2) {
        Pid_t peer = pid + 1;
        if(pid % 2 == 1) {
            peer = pid - 1;
        }
        peers.insert(peer);
    }
}


///
// Helper function that replaces a line in the Cache
PleaseTM::Line* PleaseTM::replaceLine(Pid_t pid, VAddr raddr) {
    Cache* cache= getCache(pid);

    Line* replaced = cache->findLine2Replace(raddr);
    if(replaced == nullptr) {
        fail("Replacement policy failed");
    }

    // If replaced is transactional, check for capacity aborts
    if(replaced->isTransactional()) {
        if(replaced->isDirty()) {
            // Dirty transactional lines always trigger set conflict
            markTransAborted(replaced->getWriter(), pid, replaced->getCaddr(), TM_ATYPE_SETCONFLICT);
        } else {
            // Clean lines only do so on overflow set overflows
            for(Pid_t reader: replaced->getReaders()) {
                if(overflow[reader].size() < maxOverflowSize) {
                    overflow[reader].insert(replaced->getCaddr());
                } else {
                    markTransAborted(reader, pid, replaced->getCaddr(), TM_ATYPE_SETCONFLICT);
                }
            }
        }
    } // end if(replaced->isTransactional())

    // Replace the line
    VAddr caddr = addrToCacheLine(raddr);
    VAddr myTag = cache->calcTag(raddr);
    replaced->invalidate();
    replaced->validate(myTag, caddr);

    return replaced;
}

///
// Collect transactions that would be aborted and remove from conflicting.
void PleaseTM::handleConflicts(Pid_t pid, VAddr caddr, bool isTM, set<Pid_t>& conflicting) {
    set<Pid_t> winners, losers;
    for(Pid_t c: conflicting) {
        if(isTM == false || shouldAbort(pid, caddr, c)) {
            losers.insert(c);
        } else {
            winners.insert(c);
        }
    }

    // Abort all losers
    TMAbortType_e abortType = isTM ? TM_ATYPE_DEFAULT : TM_ATYPE_NONTM;
    if(winners.size() > 0) {
        markTransAborted(pid, (*winners.begin()), caddr, TM_ATYPE_DEFAULT);
        rfchSuccMsg.add(conflicting.size());
    } else {
        markTransAborted(conflicting, pid, caddr, abortType);
        rfchFailMsg.add(conflicting.size());
        conflicting.clear();
    }
}
///
// Helper function that aborts all transactional readers and writers
void PleaseTM::abortTMWriters(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except, InstContext* p_opStatus) {
    set<Pid_t> conflicting;
    rwSetManager.getWriters(caddr, conflicting);
    conflicting.erase(pid);

    handleConflicts(pid, caddr, isTM, conflicting);

    // If any winners are around, we self abort and add them to the except set
    if(conflicting.size() > 0) {
        for(Pid_t c: conflicting) {
            p_opStatus->needRefetch.insert(c);
            except.insert(getCache(c));
        }
    }
}
///
// Helper function that aborts all transactional readers and writers
void PleaseTM::abortTMSharers(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except, InstContext* p_opStatus) {
    set<Pid_t> conflicting;
    rwSetManager.getWriters(caddr, conflicting);
    rwSetManager.getReaders(caddr, conflicting);
    conflicting.erase(pid);

    handleConflicts(pid, caddr, isTM, conflicting);

    // If any winners are around, we self abort and add them to the except set
    if(conflicting.size() > 0) {
        for(Pid_t c: conflicting) {
            p_opStatus->needRefetch.insert(c);
            except.insert(getCache(c));
        }
    }
}

///
// Helper function that cleans dirty lines in each cache except pid's.
void PleaseTM::cleanDirtyLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except) {
    getSMsg.inc();

    Cache* myCache = getCache(pid);
    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        Line* line = cache->findLine(caddr);
        if(line && line->isValid() && line->isDirty()) {
            if(cache == myCache) {
                // Requester's cache should be left alone
            } else if(except.find(cache) == except.end()) {
                if(line->isTransactional()) {
                    fwdGetSConflictMsg.inc();
                } else {
                    fwdGetSMsg.inc();
                }
                line->makeClean();
            } else {
                // In except set so leave line alone
                if(line->isTransactional()) {
                    fwdGetSConflictMsg.inc();
                } else {
                    fwdGetSMsg.inc();
                }
            }
        }
    }
}

///
// Helper function that invalidates lines except pid's.
void PleaseTM::invalidateLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except) {
    getMMsg.inc();

    Cache* myCache = getCache(pid);
    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        Line* line = cache->findLine(caddr);
        if(line && line->isValid()) {
            if(cache == myCache) {
                // Requester's cache should be left alone
            } else if(except.find(cache) == except.end()) {
                if(line->isTransactional()) {
                    invConflictMsg.inc();
                } else {
                    invMsg.inc();
                }
                line->invalidate();
            } else {
                // In except set so leave line alone
                if(line->isTransactional()) {
                    invConflictMsg.inc();
                } else {
                    invMsg.inc();
                }
            }
        }
    }
}
///
// Do a transactional read.
TMRWStatus PleaseTM::TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    std::set<Cache*> except;
    abortTMWriters(pid, caddr, true, except, p_opStatus);

    if(getTMState(pid) == TMStateEngine::TM_MARKABORT) {
        cleanDirtyLines(pid, caddr, except);
        return TMRW_ABORT;
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
TMRWStatus PleaseTM::TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    std::set<Cache*> except;
    abortTMSharers(pid, caddr, true, except, p_opStatus);

    if(getTMState(pid) == TMStateEngine::TM_MARKABORT) {
        invalidateLines(pid, caddr, except);
        return TMRW_ABORT;
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
void PleaseTM::nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    std::set<Cache*> except;
    abortTMWriters(pid, caddr, false, except, p_opStatus);

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
void PleaseTM::nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    std::set<Cache*> except;
    abortTMSharers(pid, caddr, false, except, p_opStatus);

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

TMBCStatus PleaseTM::myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    // On commit, we clear all transactional bits, but otherwise leave lines alone
    Pid_t pid   = context->getPid();

    p_opStatus->tmLat           = 4 + rwSetManager.getNumWrites(pid);
    p_opStatus->tmCommitSubtype =TM_COMMIT_REGULAR;

    // On commit, we clear all transactional bits, but otherwise leave lines alone
    Cache* cache = getCache(pid);

    LineTMComparator tmCmp;
    std::vector<Line*> lines;
    cache->collectLines(lines, tmCmp);

    for(Line* line: lines) {
        line->clearTransactional(pid);
    }
    overflow[pid].clear();

    return TMBC_SUCCESS;
}

void PleaseTM::myStartAborting(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    // On abort, we need to throw away the work we've done so far, so invalidate them
    Pid_t pid   = context->getPid();
    Cache* cache = getCache(pid);

    LineTMComparator tmCmp;
    std::vector<Line*> lines;
    cache->collectLines(lines, tmCmp);

    for(Line* line: lines) {
        if(line->isDirty() && line->getWriter() == pid) {
            line->invalidate();
        } else {
            line->clearTransactional(pid);
        }
    }
    overflow[pid].clear();
}

/////////////////////////////////////////////////////////////////////////////////////////
// PleaseTM with requester always losing
/////////////////////////////////////////////////////////////////////////////////////////
PTMRequesterLoses::PTMRequesterLoses(const char tmStyle[], int32_t nCores, int32_t line):
        PleaseTM(tmStyle, nCores, line) {
}

bool PTMRequesterLoses::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    return false;
}

/////////////////////////////////////////////////////////////////////////////////////////
// PleaseTM with more reads wins
/////////////////////////////////////////////////////////////////////////////////////////
PTMMoreReadsWins::PTMMoreReadsWins(const char tmStyle[], int32_t nCores, int32_t line):
        PleaseTM(tmStyle, nCores, line) {
}

bool PTMMoreReadsWins::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    return rwSetManager.getNumReads(other) < rwSetManager.getNumReads(pid);
}

/////////////////////////////////////////////////////////////////////////////////////////
// PleaseTM coherence with more reads wins; reads in log2
/////////////////////////////////////////////////////////////////////////////////////////
PTMLog2MoreCoherence::PTMLog2MoreCoherence(const char tmStyle[], int32_t nCores, int32_t line):
        PleaseTM(tmStyle, nCores, line) {
}

bool PTMLog2MoreCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    uint32_t log2Num = log2i(rwSetManager.getNumReads(pid));
    uint32_t log2OtherNum = log2i(rwSetManager.getNumReads(other));
    return log2OtherNum < log2Num;
}

/////////////////////////////////////////////////////////////////////////////////////////
// PleaseTM coherence with more reads wins; reads capped
/////////////////////////////////////////////////////////////////////////////////////////
PTMCappedMoreCoherence::PTMCappedMoreCoherence(const char tmStyle[], int32_t nCores, int32_t line):
        PleaseTM(tmStyle, nCores, line), m_cap(128) {
    MSG("Using cap of %lu", m_cap);
}

bool PTMCappedMoreCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    uint32_t cappedMyNum = rwSetManager.getNumReads(pid);
    uint32_t cappedOtherNum = rwSetManager.getNumReads(other);
    if(cappedMyNum > m_cap) {
        cappedMyNum = m_cap;
    }
    if(cappedOtherNum > m_cap) {
        cappedOtherNum = m_cap;
    }
    return cappedOtherNum < cappedMyNum;
}
/////////////////////////////////////////////////////////////////////////////////////////
// PleaseTM with older wins
/////////////////////////////////////////////////////////////////////////////////////////
PTMOlderWins::PTMOlderWins(const char tmStyle[], int32_t nCores, int32_t line):
        PleaseTM(tmStyle, nCores, line) {
}

bool PTMOlderWins::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    if(startTime.find(pid) == startTime.end()) { fail("%d has not started?\n", pid); }
    if(startTime.find(other) == startTime.end()) { fail("%d has not started?\n", other); }
    return startTime[pid] < startTime[other];
}
TMBCStatus PTMOlderWins::myBegin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid = context->getPid();
    startTime[pid] = globalClock;
    return PleaseTM::myBegin(inst, context, p_opStatus);
}
TMBCStatus PTMOlderWins::myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid = context->getPid();
    startTime.erase(pid);
    return PleaseTM::myCommit(inst, context, p_opStatus);
}
void PTMOlderWins::completeFallback(Pid_t pid) {
    startTime.erase(pid);
    return PleaseTM::completeFallback(pid);
}

/////////////////////////////////////////////////////////////////////////////////////////
// PleaseTM with older all wins
/////////////////////////////////////////////////////////////////////////////////////////
PTMOlderAllWins::PTMOlderAllWins(const char tmStyle[], int32_t nCores, int32_t line):
        PleaseTM(tmStyle, nCores, line) {
}

bool PTMOlderAllWins::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    if(startTime.find(pid) == startTime.end()) { fail("%d has not started?\n", pid); }
    if(startTime.find(other) == startTime.end()) { fail("%d has not started?\n", other); }
    return startTime[pid] < startTime[other];
}
TMBCStatus PTMOlderAllWins::myBegin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid = context->getPid();
    if(startTime.find(pid) == startTime.end()) {
        startTime[pid] = globalClock;
    }
    return PleaseTM::myBegin(inst, context, p_opStatus);
}
TMBCStatus PTMOlderAllWins::myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid = context->getPid();
    startTime.erase(pid);
    return PleaseTM::myCommit(inst, context, p_opStatus);
}
void PTMOlderAllWins::completeFallback(Pid_t pid) {
    startTime.erase(pid);
    return PleaseTM::completeFallback(pid);
}

/////////////////////////////////////////////////////////////////////////////////////////
// PleaseTM with more aborts wins
/////////////////////////////////////////////////////////////////////////////////////////
PTMMoreAbortsWins::PTMMoreAbortsWins(const char tmStyle[], int32_t nCores, int32_t line):
        PleaseTM(tmStyle, nCores, line) {
}

bool PTMMoreAbortsWins::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    return abortsSoFar[other] < abortsSoFar[pid];
}

