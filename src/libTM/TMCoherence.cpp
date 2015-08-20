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
TMCoherence *TMCoherence::create(int32_t nProcs) {
    TMCoherence* newCohManager;

    string method = SescConf->getCharPtr("TransactionalMemory","method");
    int lineSize = SescConf->getInt("TransactionalMemory","lineSize");

    if(method == "IdealLE") {
        newCohManager = new TMIdealLECoherence("Ideal Lazy/Eager", nProcs, lineSize);
    } else if(method == "LE") {
        newCohManager = new TMLECoherence("Lazy/Eager", nProcs, lineSize);
    } else {
        MSG("unknown TM method, using LE");
        newCohManager = new TMLECoherence("Lazy/Eager", nProcs, lineSize);
    }

    return newCohManager;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Abstract super-class of all TM policies. Contains the external interface and common
// implementations
/////////////////////////////////////////////////////////////////////////////////////////
TMCoherence::TMCoherence(const char tmStyle[], int32_t procs, int32_t line):
        nProcs(procs),
        lineSize(line),
        numCommits("tm:numCommits"),
        numAborts("tm:numAborts"),
        abortTypes("tm:abortTypes") {

    MSG("Using %s TM", tmStyle);

    for(Pid_t pid = 0; pid < nProcs; ++pid) {
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
TMBCStatus TMCoherence::begin(InstDesc* inst, ThreadContext* context) {
    Pid_t pid   = context->getPid();
    return myBegin(pid, inst);
}

///
// Entry point for TM begin operation. Check for nesting and then call the real begin.
TMBCStatus TMCoherence::commit(InstDesc* inst, ThreadContext* context) {
    Pid_t pid   = context->getPid();
	if(getState(pid) == TM_MARKABORT) {
		return TMBC_ABORT;
	} else {
		return myCommit(pid);
	}
}

TMBCStatus TMCoherence::abort(InstDesc* inst, ThreadContext* context) {
    Pid_t pid   = context->getPid();
    abortStates.at(pid).setAbortIAddr(context->getIAddr());
    return myAbort(pid);
}

///
// If the abort type is driven externally (syscall/user), then mark the transaction as aborted.
// Acutal abort needs to be called later.
void TMCoherence::markAbort(InstDesc* inst, ThreadContext* context, TMAbortType_e abortType) {
    Pid_t pid   = context->getPid();
    if(abortType != TM_ATYPE_SYSCALL && abortType != TM_ATYPE_USER) {
        fail("AbortType %d cannot be set manually\n", abortType);
    }

    transStates.at(pid).markAbort();
    abortStates.at(pid).markAbort(pid, getUtid(pid), 0, abortType);
}

///
// Entry point for TM complete abort operation (to be called after an aborted TM returns to
// tm.begin).
TMBCStatus TMCoherence::completeAbort(Pid_t pid) {
    myCompleteAbort(pid);
    return TMBC_SUCCESS;
}

///
// Entry point for TM read operation. Checks transaction state and then calls the real read.
TMRWStatus TMCoherence::read(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
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
TMRWStatus TMCoherence::write(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
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
TMBCStatus TMCoherence::myBegin(Pid_t pid, InstDesc* inst) {
    beginTrans(pid, inst);
    return TMBC_SUCCESS;
}

///
// A basic type of TM abort if child does not override
TMBCStatus TMCoherence::myAbort(Pid_t pid) {
	abortTrans(pid);
	return TMBC_SUCCESS;
}

///
// A basic type of TM commit if child does not override
TMBCStatus TMCoherence::myCommit(Pid_t pid) {
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
TMIdealLECoherence::TMIdealLECoherence(const char tmStyle[], int32_t nProcs, int32_t line):
        TMCoherence(tmStyle, nProcs, line) {

    int totalSize = SescConf->getInt("TransactionalMemory", "totalSize");
    int assoc = SescConf->getInt("TransactionalMemory", "assoc");

    for(Pid_t pid = 0; pid < nProcs; pid++) {
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
// Helper function that replaces a line in the Cache
TMIdealLECoherence::Line* TMIdealLECoherence::replaceLine(Pid_t pid, VAddr raddr) {
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);
    VAddr myTag = cache->calcTag(raddr);

    Line* line = cache->findLine2Replace(raddr);
    if(line == nullptr) {
        fail("Replacement policy failed");
    }

    // Replace the line
    line->invalidate();
    line->validate(myTag, caddr);

    return line;
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
        markTransAborted(aborted, pid, caddr, abortType);
    }

    except.insert(getCache(pid));
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
    }

    except.insert(getCache(pid));
}

///
// Helper function that cleans dirty lines in each cache except pid's.
void TMIdealLECoherence::cleanDirtyLines(VAddr raddr, std::set<Cache*>& except) {
    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        if(except.find(cache) == except.end()) {
            Line* line = cache->findLine(raddr);
            if(line && line->isValid() && line->isDirty()) {
                line->makeClean();
            }
        }
    }
}

///
// Helper function that invalidates lines except pid's.
void TMIdealLECoherence::invalidateLines(VAddr raddr, std::set<Cache*>& except) {
    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        if(except.find(cache) == except.end()) {
            Line* line = cache->findLine(raddr);
            if(line && line->isValid()) {
                line->invalidate();
            }
        }
    }
}

///
// Do a transactional read.
TMRWStatus TMIdealLECoherence::TMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    std::set<Cache*> except;
    abortTMWriters(pid, caddr, true, except);
    cleanDirtyLines(caddr, except);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        line  = replaceLine(pid, raddr);
    } else {
        p_opStatus->wasHit = true;
        if(line->isTransactional() == false && line->isDirty()) {
            // If we were the previous writer, make clean and start anew
            line->makeClean();
        }
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
TMRWStatus TMIdealLECoherence::TMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    std::set<Cache*> except;
    abortTMSharers(pid, caddr, true, except);
    invalidateLines(caddr, except);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        line  = replaceLine(pid, raddr);
    } else if(line->isDirty() == false) {
        p_opStatus->wasHit = false;
    } else {
        p_opStatus->wasHit = true;
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
void TMIdealLECoherence::nonTMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    std::set<Cache*> except;
    abortTMWriters(pid, caddr, false, except);
    cleanDirtyLines(caddr, except);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        line  = replaceLine(pid, raddr);
    } else {
        p_opStatus->wasHit = true;
    }
}

///
// Do a non-transactional write, i.e. when a thread not inside a transaction.
void TMIdealLECoherence::nonTMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    std::set<Cache*> except;
    abortTMSharers(pid, caddr, false, except);
    invalidateLines(caddr, except);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        line  = replaceLine(pid, raddr);
    } else if(line->isDirty() == false) {
        p_opStatus->wasHit = false;
    } else {
        p_opStatus->wasHit = true;
    }

    // Update line
    line->makeDirty();
}

TMBCStatus TMIdealLECoherence::myCommit(Pid_t pid) {
    // On commit, we clear all transactional bits, but otherwise leave lines alone
    Cache* cache = getCache(pid);
    LineTMComparator tmCmp;
    std::vector<Line*> lines;
    cache->collectLines(lines, tmCmp);

    for(Line* line: lines) {
        line->clearTransactional();
    }

    commitTrans(pid);
    return TMBC_SUCCESS;
}

TMBCStatus TMIdealLECoherence::myAbort(Pid_t pid) {
    // On abort, we need to throw away the work we've done so far, so invalidate them
    Cache* cache = getCache(pid);
    LineTMComparator tmCmp;
    std::vector<Line*> lines;
    cache->collectLines(lines, tmCmp);

    for(Line* line: lines) {
        if(line->isDirty()) {
            line->invalidate();
        } else {
            line->clearTransactional();
        }
    }

    abortTrans(pid);
    return TMBC_SUCCESS;
}


/////////////////////////////////////////////////////////////////////////////////////////
// TSX-style coherence using Lazy-eager coherence with cache overflow aborts.
/////////////////////////////////////////////////////////////////////////////////////////
TMLECoherence::TMLECoherence(const char tmStyle[], int32_t nProcs, int32_t line):
        TMCoherence(tmStyle, nProcs, line),
        getSMsg("tm:getSMsg"),
        fwdGetSMsg("tm:fwdGetSMsg"),
        getMMsg("tm:getMMsg"),
        invMsg("tm:invMsg"),
        flushMsg("tm:flushMsg"),
        fwdGetSConflictMsg("tm:fwdGetSConflictMsg"),
        invConflictMsg("tm:invConflictMsg") {

    int totalSize = SescConf->getInt("TransactionalMemory", "totalSize");
    int assoc = SescConf->getInt("TransactionalMemory", "assoc");
    if(SescConf->checkInt("TransactionalMemory","overflowSize")) {
        maxOverflowSize = SescConf->getInt("TransactionalMemory","overflowSize");
    } else {
        maxOverflowSize = 4;
        MSG("Using default overflow size of %ld\n", maxOverflowSize);
    }
    for(Pid_t pid = 0; pid < nProcs; pid++) {
        caches.push_back(new CacheAssocTM(totalSize, assoc, lineSize, 1));
    }
}

///
// Destructor for PrivateCache. Delete all allocated members
TMLECoherence::~TMLECoherence() {
    while(caches.size() > 0) {
        Cache* cache = caches.back();
        caches.pop_back();
        delete cache;
    }
}

///
// Do a transactional read.
TMRWStatus TMLECoherence::TMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= getCache(pid);

    // Lookup line
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;

        // Handle any sharers
        cleanWriters(pid, raddr, true);

        line  = replaceLineTM(pid, raddr);
    } else {
        p_opStatus->wasHit = true;
        if(line->isTransactional() == false && line->isDirty()) {
            // If we were the previous writer, make clean and start anew
            line->makeClean();
            flushMsg.inc();
        }
    }

    // Return abort
    if(getState(pid) == TM_MARKABORT) {
        return TMRW_ABORT;
    }

    // Update line
    line->markTransactional();
    line->addReader(pid);

    readTrans(pid, raddr, caddr);

    return TMRW_SUCCESS;
}

///
// Do a transactional write.
TMRWStatus TMLECoherence::TMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= getCache(pid);

    // Lookup line
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;

        // Handle any sharers
        invalidateSharers(pid, raddr, true);

        line  = replaceLineTM(pid, raddr);
    } else if(line->isDirty() == false) {
        p_opStatus->wasHit = false;

        // Handle any sharers
        invalidateSharers(pid, raddr, true);

        // Do NOT replace line, though. We just need to mark dirty below
    } else {
        p_opStatus->wasHit = true;
    }

    // Return abort
    if(getState(pid) == TM_MARKABORT) {
        return TMRW_ABORT;
    }

    // Update line
    line->markTransactional();
    line->makeTransactionalDirty(pid);

    writeTrans(pid, raddr, caddr);

    return TMRW_SUCCESS;
}

///
// Do a non-transactional read, i.e. when a thread not inside a transaction.
void TMLECoherence::nonTMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= getCache(pid);

    // Lookup line
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;

        // Handle any sharers
        cleanWriters(pid, raddr, false);

        line  = replaceLine(pid, raddr);
    } else {
        p_opStatus->wasHit = true;
    }
}

///
// Do a non-transactional write, i.e. when a thread not inside a transaction.
void TMLECoherence::nonTMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= getCache(pid);

    // Lookup line
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;

        // Handle any sharers
        invalidateSharers(pid, raddr, false);

        line  = replaceLine(pid, raddr);
    } else if(line->isDirty() == false) {
        p_opStatus->wasHit = false;

        // Handle any sharers
        invalidateSharers(pid, raddr, false);

        // Do NOT replace line, though. We just need to mark dirty below
    } else {
        p_opStatus->wasHit = true;
    }

    // Update line
    line->makeDirty();
}

TMLECoherence::Line* TMLECoherence::replaceLine(Pid_t pid, VAddr raddr) {
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);
    VAddr myTag = cache->calcTag(raddr);
    Line* line = nullptr;

    // Find line to replace
    line = cache->findLine2Replace(raddr);
    if(line == nullptr) {
        fail("Replacement policy failed");
    }

    if(line->isValid() && line->isTransactional()) {
        abortReplaced(line, pid, caddr, TM_ATYPE_NONTM);
    }
    if(line->isValid() && line->isTransactional() == false && line->isDirty()) {
        flushMsg.inc();
    }

    // Replace the line
    line->invalidate();
    line->validate(myTag, caddr);
    return line;
}

TMLECoherence::Line* TMLECoherence::replaceLineTM(Pid_t pid, VAddr raddr) {
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);
    VAddr myTag = cache->calcTag(raddr);
    Line* line = nullptr;

    line = cache->findLine2Replace(raddr);
    if(line == nullptr) {
        fail("Replacement policy failed");
    }

    if(line->isValid() && line->isTransactional()) {
        abortReplaced(line, pid, caddr, TM_ATYPE_SETCONFLICT);
    }
    if(line->isValid() && line->isTransactional() == false && line->isDirty()) {
        flushMsg.inc();
    }

    // Replace the line
    line->invalidate();
    line->validate(myTag, caddr);

    // If this line had been sent to the overflow set, bring it back.
    if(overflow[pid].find(caddr) != overflow[pid].end()) {
        overflow[pid].erase(caddr);
    }

    return line;
}

///
// Abort all transactions that had accessed the line ``replaced.''
void TMLECoherence::abortReplaced(Line* replaced, Pid_t byPid, VAddr byCaddr, TMAbortType_e abortType) {
    Pid_t writer = replaced->getWriter();
    if(writer != INVALID_PID) {
        markTransAborted(writer, byPid, byCaddr, abortType);
        replaced->clearTransactional(writer);
    }
    for(Pid_t reader: replaced->getReaders()) {
        if(overflow[reader].size() < maxOverflowSize) {
            overflow[reader].insert(replaced->getCaddr());
        } else {
            markTransAborted(reader, byPid, byCaddr, abortType);
        }
        replaced->clearTransactional(reader);
    }
}

///
// Helper function that looks at all private caches and invalidates all sharers, while aborting
// transactions.
void TMLECoherence::invalidateSharers(Pid_t pid, VAddr raddr, bool isTM) {
    getMMsg.inc();

    set<Pid_t> sharers;

    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        Line* line = cache->findLine(raddr);
        if(line) {
            bool causedAbort = false;
            set<Pid_t> lineSharers;
            line->getAccessors(lineSharers);

            for(Pid_t s: lineSharers) {
                if(s != pid) {
                    causedAbort = true;
                    line->clearTransactional(s);
                    sharers.insert(s);
                }
            }
            if(getCache(pid) != cache) {
                // "Other" cache, so invalidate
                line->invalidate();
                if(causedAbort) {
                    invConflictMsg.inc();
                } else {
                    invMsg.inc();
                }
            }
        }
    }

	VAddr caddr = addrToCacheLine(raddr);
    // Look at everyone's overflow set to see if the line is in there
    for(Pid_t p = 0; p < (Pid_t)nProcs; ++p) {
        if(p != pid) {
            if(overflow[p].find(caddr) != overflow[p].end()) {
                invConflictMsg.inc();
                sharers.insert(p);
            }
        }
    } // End foreach(pid)

    if(isTM) {
        markTransAborted(sharers, pid, caddr, TM_ATYPE_DEFAULT);
    } else {
        markTransAborted(sharers, pid, caddr, TM_ATYPE_NONTM);
    }
}

///
// Helper function that looks at all private caches and makes clean writers, while aborting
// transactional writers.
void TMLECoherence::cleanWriters(Pid_t pid, VAddr raddr, bool isTM) {
	VAddr caddr = addrToCacheLine(raddr);

    getSMsg.inc();

    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        Line* line = cache->findLine(raddr);
        if(line && line->isDirty()) {
            if(line->isTransactional()) {
                Pid_t writer = line->getWriter();
                if(writer == pid) { fail("Why clean my own write?\n"); }

                if(isTM) {
                    markTransAborted(writer, pid, caddr, TM_ATYPE_DEFAULT);
                } else {
                    markTransAborted(writer, pid, caddr, TM_ATYPE_NONTM);
                }
                fwdGetSConflictMsg.inc();
            } else {
                fwdGetSMsg.inc();
            }
            line->makeClean();
        }
    } // End foreach(cache)
}

TMBCStatus TMLECoherence::myCommit(Pid_t pid) {
    // On commit, we clear all transactional bits, but otherwise leave lines alone
    Cache* cache = getCache(pid);
    LineTMComparator tmCmp;
    std::vector<Line*> lines;
    cache->collectLines(lines, tmCmp);

    for(Line* line: lines) {
        line->clearTransactional();
    }

    // Clear our overflow set
    overflow[pid].clear();

    commitTrans(pid);
    return TMBC_SUCCESS;
}

TMBCStatus TMLECoherence::myAbort(Pid_t pid) {
    // On abort, we need to throw away the work we've done so far, so invalidate them
    Cache* cache = getCache(pid);
    LineTMComparator tmCmp;
    std::vector<Line*> lines;
    cache->collectLines(lines, tmCmp);

    for(Line* line: lines) {
        if(line->isDirty()) {
            line->invalidate();
        } else {
            line->clearTransactional();
        }
    }

    // Clear our overflow set
    overflow[pid].clear();

    abortTrans(pid);
    return TMBC_SUCCESS;
}
