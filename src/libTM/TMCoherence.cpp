#include <cmath>
#include <algorithm>
#include <iostream>
#include "libsuc/nanassert.h"
#include "SescConf.h"
#include "libll/Instruction.h"
#include "TMCoherence.h"

using namespace std;

TMCoherence *tmCohManager = 0;
/////////////////////////////////////////////////////////////////////////////////////////
// Factory function for all TM Coherence objects. Use different concrete classes
// depending on SescConf
/////////////////////////////////////////////////////////////////////////////////////////
TMCoherence *TMCoherence::create(int32_t nCores) {
    TMCoherence* newCohManager;

    string method = SescConf->getCharPtr("TransactionalMemory","method");
    int lineSize = SescConf->getInt("TransactionalMemory","lineSize");

    if(method == "IdealLE") {
        newCohManager = new TMIdealLECoherence("Ideal Lazy/Eager", nCores, lineSize);
    } else {
        MSG("unknown TM method, using LE");
        newCohManager = new TMIdealLECoherence("Ideal Lazy/Eager", nCores, lineSize);
    }

    return newCohManager;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Abstract super-class of all TM policies. Contains the external interface and common
// implementations
/////////////////////////////////////////////////////////////////////////////////////////
TMCoherence::TMCoherence(const char tmStyle[], int32_t procs, int32_t line):
        nCores(procs),
        lineSize(line),
        numCommits("tm:numCommits"),
        numAborts("tm:numAborts"),
        abortTypes("tm:abortTypes") {

    if(SescConf->checkInt("TransactionalMemory","smtContexts")) {
        nSMTWays = SescConf->getInt("TransactionalMemory","smtContexts");
        MSG("Using %s TM (%d core %lu-way)", tmStyle, nCores, nSMTWays);
    } else {
        nSMTWays = 1;
        MSG("Using %s TM (%d cores)", tmStyle, nCores);
    }

    nThreads = nCores * nSMTWays;

    for(Pid_t pid = 0; pid < (Pid_t)nThreads; ++pid) {
        transStates.push_back(TransState(pid));
        abortStates.push_back(TMAbortState(pid));
        // Initialize maps to enable at() use
        linesRead[pid].clear();
        linesWritten[pid].clear();
    }
}

void TMCoherence::beginTrans(Pid_t pid, InstDesc* inst) {
    // Do the begin
	transStates[pid].begin();
    abortStates.at(pid).clear();
}

void TMCoherence::commitTrans(Pid_t pid) {
    // Update Statistics
    numCommits.inc();

    // Do the commit
    removeTransaction(pid);
}
void TMCoherence::abortTrans(Pid_t pid) {
    if(getState(pid) != TM_MARKABORT) {
        fail("%d should be marked abort before starting abort: %d", pid, getState(pid));
    }
	transStates[pid].startAborting();
}
void TMCoherence::completeAbortTrans(Pid_t pid) {
    if(getState(pid) != TM_ABORTING) {
        fail("%d should be doing aborting before completing it: %d", pid, getState(pid));
    }

    const TMAbortState& abortState = abortStates.at(pid);
    // Update Statistics
    numAborts.inc();
    abortTypes.sample(abortState.getAbortType());

    // Do the completeAbort
    removeTransaction(pid);
}
void TMCoherence::markTransAborted(Pid_t victimPid, Pid_t aborterPid, VAddr caddr, TMAbortType_e abortType) {
    uint64_t aborterUtid = getUtid(aborterPid);

    if(getState(victimPid) != TM_ABORTING && getState(victimPid) != TM_MARKABORT) {
        transStates.at(victimPid).markAbort();
        abortStates.at(victimPid).markAbort(aborterPid, aborterUtid, caddr, abortType);
    } // Else victim is already aborting, so leave it alone
}

void TMCoherence::markTransAborted(std::set<Pid_t>& aborted, Pid_t aborterPid, VAddr caddr, TMAbortType_e abortType) {
	set<Pid_t>::iterator i_aborted;
    for(i_aborted = aborted.begin(); i_aborted != aborted.end(); ++i_aborted) {
		if(*i_aborted == INVALID_PID) {
            fail("Trying to abort invalid Pid?");
        }
        markTransAborted(*i_aborted, aborterPid, caddr, abortType);
	}
}
void TMCoherence::readTrans(Pid_t pid, VAddr raddr, VAddr caddr) {
    if(getState(pid) != TM_RUNNING) {
        fail("%d in invalid state to do tm.load: %d", pid, getState(pid));
    }
    readers[caddr].insert(pid);
    linesRead[pid].insert(caddr);
}
void TMCoherence::writeTrans(Pid_t pid, VAddr raddr, VAddr caddr) {
    if(getState(pid) != TM_RUNNING) {
        fail("%d in invalid state to do tm.store: %d", pid, getState(pid));
    }
    writers[caddr].insert(pid);
    linesWritten[pid].insert(caddr);
}
void TMCoherence::removeTrans(Pid_t pid) {
    transStates.at(pid).clear();

    std::map<VAddr, std::set<Pid_t> >::iterator i_line;
    for(VAddr caddr:  linesWritten[pid]) {
        i_line = writers.find(caddr);
        if(i_line == writers.end()) {
            fail("writers and linesWritten mismatch");
        }
        set<Pid_t>& myWriters = i_line->second;
        if(myWriters.find(pid) == myWriters.end()) {
            fail("writers does not contain pid");
        }
        myWriters.erase(pid);
        if(myWriters.empty()) {
            writers.erase(i_line);
        }
    }
    for(VAddr caddr:  linesRead[pid]) {
        i_line = readers.find(caddr);
        if(i_line == readers.end()) {
            fail("readers and linesRead mismatch");
        }
        set<Pid_t>& myReaders = i_line->second;
        if(myReaders.find(pid) == myReaders.end()) {
            fail("readers does not contain pid");
        }
        myReaders.erase(pid);
        if(myReaders.empty()) {
            readers.erase(i_line);
        }
    }
    linesRead[pid].clear();
    linesWritten[pid].clear();
}

///
// Entry point for TM begin operation. Check for nesting and then call the real begin.
TMBCStatus TMCoherence::begin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    return myBegin(inst, context, p_opStatus);
}

///
// Entry point for TM begin operation. Check for nesting and then call the real begin.
TMBCStatus TMCoherence::commit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
	if(getState(pid) == TM_MARKABORT) {
        p_opStatus->tmCommitSubtype=TM_COMMIT_ABORTED;
		return TMBC_ABORT;
	} else {
		return myCommit(inst, context, p_opStatus);
	}
}

TMBCStatus TMCoherence::abort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    abortStates.at(pid).setAbortIAddr(context->getIAddr());
    return myAbort(inst, context, p_opStatus);
}

///
// If the abort type is driven externally (syscall/user), then mark the transaction as aborted.
// Acutal abort needs to be called later.
void TMCoherence::markAbort(InstDesc* inst, const ThreadContext* context, TMAbortType_e abortType) {
    Pid_t pid   = context->getPid();
    if(abortType != TM_ATYPE_SYSCALL && abortType != TM_ATYPE_USER) {
        fail("AbortType %d cannot be set manually\n", abortType);
    }

    markTransAborted(pid, pid, 0, abortType);
}

///
// Entry point for TM complete abort operation (to be called after an aborted TM returns to
// tm.begin).
TMBCStatus TMCoherence::completeAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    p_opStatus->tmBeginSubtype=TM_COMPLETE_ABORT;

    myCompleteAbort(pid);
    return TMBC_SUCCESS;
}

///
// Entry point for TM read operation. Checks transaction state and then calls the real read.
TMRWStatus TMCoherence::read(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
	if(getState(pid) == TM_MARKABORT) {
		return TMRW_ABORT;
	} else if(getState(pid) == TM_INVALID) {
        nonTMRead(inst, context, raddr, p_opStatus);
        return TMRW_NONTM;
	} else {
        return TMRead(inst, context, raddr, p_opStatus);
    }
}

///
// Entry point for TM write operation. Checks transaction state and then calls the real write.
TMRWStatus TMCoherence::write(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
	if(getState(pid) == TM_MARKABORT) {
		return TMRW_ABORT;
	} else if(getState(pid) == TM_INVALID) {
        nonTMWrite(inst, context, raddr, p_opStatus);
        return TMRW_NONTM;
	} else {
        return TMWrite(inst, context, raddr, p_opStatus);
    }
}

///
// A basic type of TM begin that will be used if child does not override
TMBCStatus TMCoherence::myBegin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    p_opStatus->tmBeginSubtype=TM_BEGIN_REGULAR;
    beginTrans(pid, inst);

    return TMBC_SUCCESS;
}

///
// A basic type of TM abort if child does not override
TMBCStatus TMCoherence::myAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
	abortTrans(pid);
	return TMBC_SUCCESS;
}

///
// A basic type of TM commit if child does not override
TMBCStatus TMCoherence::myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();

    p_opStatus->tmLat           = 4 + getNumWrites(pid);
    p_opStatus->tmCommitSubtype =TM_COMMIT_REGULAR;

    commitTrans(pid);
    return TMBC_SUCCESS;
}

///
// A basic type of TM complete abort if child does not override
void TMCoherence::myCompleteAbort(Pid_t pid) {
    completeAbortTrans(pid);
}
void TMCoherence::removeTransaction(Pid_t pid) {
    removeTrans(pid);
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence. This is the most simple style of TM, and used in TSX
/////////////////////////////////////////////////////////////////////////////////////////
TMIdealLECoherence::TMIdealLECoherence(const char tmStyle[], int32_t nCores, int32_t line):
        TMCoherence(tmStyle, nCores, line) {

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
TMIdealLECoherence::~TMIdealLECoherence() {
    while(caches.size() > 0) {
        Cache* cache = caches.back();
        caches.pop_back();
        delete cache;
    }
}

///
// If this line had been sent to the overflow set, bring it back.
void TMIdealLECoherence::updateOverflow(Pid_t pid, VAddr newCaddr) {
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
void TMIdealLECoherence::getPeers(Pid_t pid, std::set<Pid_t>& peers) {
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
TMIdealLECoherence::Line* TMIdealLECoherence::replaceLine(Pid_t pid, VAddr raddr) {
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
// Helper function that aborts all transactional readers
void TMIdealLECoherence::abortTMWriters(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except) {
    // Collect writers
    set<Pid_t> aborted;
    if(numWriters(caddr) != 0) {
        aborted.insert(writers.at(caddr).begin(), writers.at(caddr).end());
    }
    aborted.erase(pid);

    TMAbortType_e abortType = isTM ? TM_ATYPE_DEFAULT : TM_ATYPE_NONTM;

    if(aborted.size() > 0) {
        // Do the abort
        for(Pid_t a: aborted) {
            Cache* cache = getCache(a);
            Line* line = cache->findLine(caddr);
            if(line) {
                line->clearTransactional(a);
            } else if(getState(a) == TM_RUNNING) {
                fail("[%d] Aborting non-writer %d?: 0x%lx", pid, a, caddr);
            }
        }
        markTransAborted(aborted, pid, caddr, abortType);
    }
}

///
// Helper function that aborts all transactional readers and writers
void TMIdealLECoherence::abortTMSharers(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except) {
    // Collect sharers
    set<Pid_t> aborted;
    if(numWriters(caddr) != 0) {
        aborted.insert(writers.at(caddr).begin(), writers.at(caddr).end());
    }
    if(numReaders(caddr) != 0) {
        aborted.insert(readers.at(caddr).begin(), readers.at(caddr).end());
    }
    aborted.erase(pid);

    TMAbortType_e abortType = isTM ? TM_ATYPE_DEFAULT : TM_ATYPE_NONTM;

    if(aborted.size() > 0) {
        // Do the abort
        markTransAborted(aborted, pid, caddr, abortType);
        for(Pid_t a: aborted) {
            Cache* cache = getCache(a);
            Line* line = cache->findLine(caddr);
            if(line) {
                line->clearTransactional(a);
            } else if(overflow[a].find(caddr) == overflow[a].end() && getState(a) == TM_RUNNING) {
                fail("[%d] Aborting non-sharer %d?: 0x%lx", pid, a, caddr);
            }
        }
    }
}

///
// Helper function that cleans dirty lines in each cache except pid's.
void TMIdealLECoherence::cleanDirtyLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except) {
    Cache* myCache = getCache(pid);
    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        Line* line = cache->findLine(caddr);
        if(line && line->isValid() && line->isDirty()) {
            if(cache == myCache) {
                // Requester's cache should be left alone
            } else if(except.find(cache) == except.end()) {
                line->makeClean();
            } else {
                // In except set so don't do anything
            }
        }
    }
}

///
// Helper function that invalidates lines except pid's.
void TMIdealLECoherence::invalidateLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except) {
    Cache* myCache = getCache(pid);
    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        Line* line = cache->findLine(caddr);
        if(line && line->isValid()) {
            if(cache == myCache) {
                // Requester's cache should be left alone
            } else if(except.find(cache) == except.end()) {
                line->invalidate();
            } else {
                // In except set so don't do anything
            }
        }
    }
}

///
// Do a transactional read.
TMRWStatus TMIdealLECoherence::TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    std::set<Cache*> except;
    abortTMWriters(pid, caddr, true, except);

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
        }
    }
    if(line->isValid() == false || line->getCaddr() != caddr) {
        fail("got wrong line");
    }

    if(getState(pid) == TM_MARKABORT) {
        return TMRW_ABORT;
    }

    // Update line
    line->markTransactional();
    line->addReader(pid);
    updateOverflow(pid, caddr);

    // Do the read
    readTrans(pid, raddr, caddr);

    return TMRW_SUCCESS;
}


///
// Do a transactional write.
TMRWStatus TMIdealLECoherence::TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    std::set<Cache*> except;
    abortTMSharers(pid, caddr, true, except);

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
    if(line->isValid() == false || line->getCaddr() != caddr) {
        fail("got wrong line");
    }
    if(getState(pid) == TM_MARKABORT) {
        return TMRW_ABORT;
    }

    // Update line
    line->markTransactional();
    line->makeTransactionalDirty(pid);
    updateOverflow(pid, caddr);

    // Do the write
    writeTrans(pid, raddr, caddr);

    return TMRW_SUCCESS;
}

///
// Do a non-transactional read, i.e. when a thread not inside a transaction.
void TMIdealLECoherence::nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    std::set<Cache*> except;
    abortTMWriters(pid, caddr, false, except);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        cleanDirtyLines(pid, caddr, except);
        line  = replaceLine(pid, raddr);
    } else {
        p_opStatus->wasHit = true;
    }
    if(line->isValid() == false || line->getCaddr() != caddr) {
        fail("got wrong line");
    }

    updateOverflow(pid, caddr);
}

///
// Do a non-transactional write, i.e. when a thread not inside a transaction.
void TMIdealLECoherence::nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    std::set<Cache*> except;
    abortTMSharers(pid, caddr, false, except);

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
    if(line->isValid() == false || line->getCaddr() != caddr) {
        fail("got wrong line");
    }


    // Update line
    line->makeDirty();
    updateOverflow(pid, caddr);
}

TMBCStatus TMIdealLECoherence::myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();

    p_opStatus->tmLat           = 4 + getNumWrites(pid);
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

    commitTrans(pid);
    return TMBC_SUCCESS;
}

TMBCStatus TMIdealLECoherence::myAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
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

    abortTrans(pid);
    return TMBC_SUCCESS;
}

