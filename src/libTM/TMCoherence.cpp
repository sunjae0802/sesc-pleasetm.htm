#include <cmath>
#include <algorithm>
#include <iostream>
#include "libsuc/nanassert.h"
#include "SescConf.h"
#include "libll/Instruction.h"
#include "TMCoherence.h"

using namespace std;

uint64_t HTMManager::nextUtid = 0;
HTMManager *htmManager = 0;
/////////////////////////////////////////////////////////////////////////////////////////
// Factory function for all TM Coherence objects. Use different concrete classes
// depending on SescConf
/////////////////////////////////////////////////////////////////////////////////////////
HTMManager *HTMManager::create(int32_t nCores) {
    HTMManager* newCohManager;

    string method = SescConf->getCharPtr("TransactionalMemory","method");
    int lineSize = SescConf->getInt("TransactionalMemory","lineSize");

    if(method == "TSX") {
        newCohManager = new TSXManager("TSX", nCores, lineSize);
    } else if(method == "Ideal-TSX") {
        newCohManager = new IdealTSXManager("Ideal-TSX", nCores, lineSize);
    } else {
        MSG("unknown TM method, using TSX");
        newCohManager = new TSXManager("TSX", nCores, lineSize);
    }

    return newCohManager;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Abstract super-class of all TM policies. Contains the external interface and common
// implementations
/////////////////////////////////////////////////////////////////////////////////////////
HTMManager::HTMManager(const char tmStyle[], int32_t procs, int32_t line):
        nCores(procs),
        lineSize(line),
        numCommits("tm:numCommits"),
        numAborts("tm:numAborts"),
        abortTypes("tm:abortTypes"),
        userAbortArgs("tm:userAbortArgs") {

    if(SescConf->checkInt("TransactionalMemory","smtContexts")) {
        nSMTWays = SescConf->getInt("TransactionalMemory","smtContexts");
        MSG("Using %s TM (%d core %lu-way)", tmStyle, nCores, nSMTWays);
    } else {
        nSMTWays = 1;
        MSG("Using %s TM (%d cores)", tmStyle, nCores);
    }

    nThreads = nCores * nSMTWays;

    for(Pid_t pid = 0; pid < (Pid_t)nThreads; ++pid) {
        tmStates.push_back(TMStateEngine(pid));
        abortStates.push_back(TMAbortState(pid));
        utids.push_back(INVALID_UTID);
        // Initialize maps to enable at() use
        linesRead[pid].clear();
        linesWritten[pid].clear();
    }
}

void HTMManager::beginTrans(Pid_t pid, InstDesc* inst) {
    // Do the begin
    utids.at(pid) = HTMManager::nextUtid;
    HTMManager::nextUtid += 1;

	tmStates[pid].begin();
    abortStates.at(pid).clear();
}

void HTMManager::commitTrans(Pid_t pid) {
    // Update Statistics
    numCommits.inc();

    // Do the commit
    removeTransaction(pid);
}
void HTMManager::abortTrans(Pid_t pid) {
    if(getTMState(pid) != TMStateEngine::TM_MARKABORT) {
        fail("%d should be marked abort before starting abort: %d", pid, getTMState(pid));
    }
	tmStates[pid].startAborting();
}
void HTMManager::completeAbortTrans(Pid_t pid) {
    if(getTMState(pid) != TMStateEngine::TM_ABORTING) {
        fail("%d should be doing aborting before completing it: %d", pid, getTMState(pid));
    }

    const TMAbortState& abortState = abortStates.at(pid);
    // Update Statistics
    numAborts.inc();
    abortTypes.sample(abortState.getAbortType());

    // Do the completeAbort
    removeTransaction(pid);
}
void HTMManager::markTransAborted(Pid_t victimPid, Pid_t aborterPid, VAddr caddr, TMAbortType_e abortType) {
    uint64_t aborterUtid = getUtid(aborterPid);

    if(getTMState(victimPid) != TMStateEngine::TM_ABORTING && getTMState(victimPid) != TMStateEngine::TM_MARKABORT) {
        tmStates.at(victimPid).markAbort();
        abortStates.at(victimPid).markAbort(aborterPid, aborterUtid, caddr, abortType);
    } // Else victim is already aborting, so leave it alone
}

void HTMManager::markTransAborted(std::set<Pid_t>& aborted, Pid_t aborterPid, VAddr caddr, TMAbortType_e abortType) {
	set<Pid_t>::iterator i_aborted;
    for(i_aborted = aborted.begin(); i_aborted != aborted.end(); ++i_aborted) {
		if(*i_aborted == INVALID_PID) {
            fail("Trying to abort invalid Pid?");
        }
        markTransAborted(*i_aborted, aborterPid, caddr, abortType);
	}
}
void HTMManager::readTrans(Pid_t pid, VAddr raddr, VAddr caddr) {
    if(getTMState(pid) != TMStateEngine::TM_RUNNING) {
        fail("%d in invalid state to do tm.load: %d", pid, getTMState(pid));
    }
    readers[caddr].insert(pid);
    linesRead[pid].insert(caddr);
}
void HTMManager::writeTrans(Pid_t pid, VAddr raddr, VAddr caddr) {
    if(getTMState(pid) != TMStateEngine::TM_RUNNING) {
        fail("%d in invalid state to do tm.store: %d", pid, getTMState(pid));
    }
    writers[caddr].insert(pid);
    linesWritten[pid].insert(caddr);
}
void HTMManager::removeTrans(Pid_t pid) {
    tmStates.at(pid).clear();
    utids.at(pid) = INVALID_UTID;

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
TMBCStatus HTMManager::begin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    return myBegin(inst, context, p_opStatus);
}

///
// Entry point for TM begin operation. Check for nesting and then call the real begin.
TMBCStatus HTMManager::commit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
	if(getTMState(pid) == TMStateEngine::TM_MARKABORT) {
        p_opStatus->tmCommitSubtype=TM_COMMIT_ABORTED;
		return TMBC_ABORT;
	} else {
		return myCommit(inst, context, p_opStatus);
	}
}

TMBCStatus HTMManager::abort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    abortStates.at(pid).setAbortIAddr(context->getIAddr());
    return myAbort(inst, context, p_opStatus);
}

///
// If the abort type is driven externally by a syscall, then mark the transaction as aborted.
// Acutal abort needs to be called later.
void HTMManager::markSyscallAbort(InstDesc* inst, const ThreadContext* context) {
    Pid_t pid   = context->getPid();
    if(getTMState(pid) != TMStateEngine::TM_ABORTING && getTMState(pid) != TMStateEngine::TM_MARKABORT) {
        markTransAborted(pid, pid, 0, TM_ATYPE_SYSCALL);
    }
}

///
// If the abort type is driven externally by the user, then mark the transaction as aborted.
// Acutal abort needs to be called later.
void HTMManager::markUserAbort(InstDesc* inst, const ThreadContext* context, uint32_t abortArg) {
    Pid_t pid   = context->getPid();
    if(getTMState(pid) != TMStateEngine::TM_ABORTING && getTMState(pid) != TMStateEngine::TM_MARKABORT) {
        markTransAborted(pid, pid, 0, TM_ATYPE_USER);
        userAbortArgs.sample(abortArg);
    }
}

///
// Entry point for TM complete abort operation (to be called after an aborted TM returns to
// tm.begin).
TMBCStatus HTMManager::completeAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    p_opStatus->tmBeginSubtype=TM_COMPLETE_ABORT;

    myCompleteAbort(pid);
    return TMBC_SUCCESS;
}

///
// Entry point for TM read operation. Checks transaction state and then calls the real read.
TMRWStatus HTMManager::read(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
	if(getTMState(pid) == TMStateEngine::TM_MARKABORT) {
		return TMRW_ABORT;
	} else if(getTMState(pid) == TMStateEngine::TM_INVALID) {
        nonTMRead(inst, context, raddr, p_opStatus);
        return TMRW_NONTM;
	} else {
        return TMRead(inst, context, raddr, p_opStatus);
    }
}

///
// Entry point for TM write operation. Checks transaction state and then calls the real write.
TMRWStatus HTMManager::write(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
	if(getTMState(pid) == TMStateEngine::TM_MARKABORT) {
		return TMRW_ABORT;
	} else if(getTMState(pid) == TMStateEngine::TM_INVALID) {
        nonTMWrite(inst, context, raddr, p_opStatus);
        return TMRW_NONTM;
	} else {
        return TMWrite(inst, context, raddr, p_opStatus);
    }
}

///
// A basic type of TM begin that will be used if child does not override
TMBCStatus HTMManager::myBegin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    p_opStatus->tmBeginSubtype=TM_BEGIN_REGULAR;
    beginTrans(pid, inst);

    return TMBC_SUCCESS;
}

///
// A basic type of TM abort if child does not override
TMBCStatus HTMManager::myAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
	abortTrans(pid);
	return TMBC_SUCCESS;
}

///
// A basic type of TM commit if child does not override
TMBCStatus HTMManager::myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();

    p_opStatus->tmLat           = 4 + getNumWrites(pid);
    p_opStatus->tmCommitSubtype =TM_COMMIT_REGULAR;

    commitTrans(pid);
    return TMBC_SUCCESS;
}

///
// A basic type of TM complete abort if child does not override
void HTMManager::myCompleteAbort(Pid_t pid) {
    completeAbortTrans(pid);
}
void HTMManager::removeTransaction(Pid_t pid) {
    removeTrans(pid);
}

/////////////////////////////////////////////////////////////////////////////////////////
// Infinite-capacity version of TSX HTM
/////////////////////////////////////////////////////////////////////////////////////////
IdealTSXManager::IdealTSXManager(const char tmStyle[], int32_t nCores, int32_t line):
        HTMManager(tmStyle, nCores, line) {

    int totalSize = SescConf->getInt("TransactionalMemory", "totalSize");
    int assoc = SescConf->getInt("TransactionalMemory", "assoc");

    for(int coreId = 0; coreId < nCores; coreId++) {
        caches.push_back(new CacheAssocTM(totalSize, assoc, lineSize, 1));
    }
}

///
// Destructor for PrivateCache. Delete all allocated members
IdealTSXManager::~IdealTSXManager() {
    while(caches.size() > 0) {
        Cache* cache = caches.back();
        caches.pop_back();
        delete cache;
    }
}


///
// Helper function that replaces a line in the Cache
IdealTSXManager::Line* IdealTSXManager::replaceLine(Pid_t pid, VAddr raddr) {
    Cache* cache= getCache(pid);

    Line* replaced = cache->findLine2Replace(raddr);
    if(replaced == nullptr) {
        fail("Replacement policy failed");
    }

    // Replace the line
    VAddr caddr = addrToCacheLine(raddr);
    VAddr myTag = cache->calcTag(raddr);
    replaced->invalidate();
    replaced->validate(myTag, caddr);

    return replaced;
}

///
// Helper function that aborts all transactional readers
void IdealTSXManager::abortTMWriters(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except) {
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
            } else if(getTMState(a) == TMStateEngine::TM_RUNNING) {
                fail("[%d] Aborting non-writer %d?: 0x%lx", pid, a, caddr);
            }
        }
        markTransAborted(aborted, pid, caddr, abortType);
    }
}

///
// Helper function that aborts all transactional readers and writers
void IdealTSXManager::abortTMSharers(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except) {
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
            }
        }
    }
}

///
// Helper function that cleans dirty lines in each cache except pid's.
void IdealTSXManager::cleanDirtyLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except) {
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
void IdealTSXManager::invalidateLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except) {
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
TMRWStatus IdealTSXManager::TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
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

    if(getTMState(pid) == TMStateEngine::TM_MARKABORT) {
        return TMRW_ABORT;
    }

    // Update line
    line->markTransactional();
    line->addReader(pid);

    // Do the read
    readTrans(pid, raddr, caddr);

    return TMRW_SUCCESS;
}


///
// Do a transactional write.
TMRWStatus IdealTSXManager::TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
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
    if(getTMState(pid) == TMStateEngine::TM_MARKABORT) {
        return TMRW_ABORT;
    }

    // Update line
    line->markTransactional();
    line->makeTransactionalDirty(pid);

    // Do the write
    writeTrans(pid, raddr, caddr);

    return TMRW_SUCCESS;
}

///
// Do a non-transactional read, i.e. when a thread not inside a transaction.
void IdealTSXManager::nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
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
}

///
// Do a non-transactional write, i.e. when a thread not inside a transaction.
void IdealTSXManager::nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
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
}

TMBCStatus IdealTSXManager::myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
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

    commitTrans(pid);
    return TMBC_SUCCESS;
}

TMBCStatus IdealTSXManager::myAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
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

    abortTrans(pid);
    return TMBC_SUCCESS;
}


/////////////////////////////////////////////////////////////////////////////////////////
// The most simple style of TM used in TSX
/////////////////////////////////////////////////////////////////////////////////////////
TSXManager::TSXManager(const char tmStyle[], int32_t nCores, int32_t line):
        HTMManager(tmStyle, nCores, line) {

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
TSXManager::~TSXManager() {
    while(caches.size() > 0) {
        Cache* cache = caches.back();
        caches.pop_back();
        delete cache;
    }
}

///
// If this line had been sent to the overflow set, bring it back.
void TSXManager::updateOverflow(Pid_t pid, VAddr newCaddr) {
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
void TSXManager::getPeers(Pid_t pid, std::set<Pid_t>& peers) {
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
TSXManager::Line* TSXManager::replaceLine(Pid_t pid, VAddr raddr) {
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
void TSXManager::abortTMWriters(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except) {
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
            } else if(getTMState(a) == TMStateEngine::TM_RUNNING) {
                fail("[%d] Aborting non-writer %d?: 0x%lx", pid, a, caddr);
            }
        }
        markTransAborted(aborted, pid, caddr, abortType);
    }
}

///
// Helper function that aborts all transactional readers and writers
void TSXManager::abortTMSharers(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except) {
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
            } else if(overflow[a].find(caddr) == overflow[a].end() && getTMState(a) == TMStateEngine::TM_RUNNING) {
                fail("[%d] Aborting non-sharer %d?: 0x%lx", pid, a, caddr);
            }
        }
    }
}

///
// Helper function that cleans dirty lines in each cache except pid's.
void TSXManager::cleanDirtyLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except) {
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
void TSXManager::invalidateLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except) {
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
TMRWStatus TSXManager::TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
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

    if(getTMState(pid) == TMStateEngine::TM_MARKABORT) {
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
TMRWStatus TSXManager::TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
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
    if(getTMState(pid) == TMStateEngine::TM_MARKABORT) {
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
void TSXManager::nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
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
void TSXManager::nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
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

TMBCStatus TSXManager::myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
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

TMBCStatus TSXManager::myAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
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

