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
    } else if(method == "IdealRequesterLoses") {
        newCohManager = new TMIdealRequesterLoses("Ideal Requester Loses", nProcs, lineSize);
    } else if(method == "IdealMoreReadsWins") {
        newCohManager = new TMIdealRequesterLoses("Ideal More Reads Wins", nProcs, lineSize);
    } else if(method == "RequesterLoses") {
        newCohManager = new TMRequesterLoses("Requester Loses", nProcs, lineSize);
    } else if(method == "MoreReadsWins") {
        newCohManager = new TMMoreReadsWinsCoherence("More Reads Wins", nProcs, lineSize);
    } else if(method == "EE") {
        newCohManager = new TMEECoherence("Eager/Eager", nProcs, lineSize);
    } else if(method == "EENumReads") {
        newCohManager = new TMEENumReadsCoherence("Eager/Eager More Reads", nProcs, lineSize);
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
	transStates[pid].startAborting();
}
void TMCoherence::completeAbortTrans(Pid_t pid) {
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
    if(getState(pid) == TM_ABORTING) {
        myCompleteAbort(pid);
    }
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
void TMIdealLECoherence::abortTMWriters(Pid_t pid, VAddr caddr, TMAbortType_e abortType) {
    // Collect writers
    set<Pid_t> aborted;
    if(numWriters(caddr) != 0) {
        aborted.insert(writers.at(caddr).begin(), writers.at(caddr).end());
    }
    aborted.erase(pid);

    if(aborted.size() > 0) {
        // Do the abort
        markTransAborted(aborted, pid, caddr, abortType);
    }
    cleanDirtyLines(pid, caddr);
}

///
// Helper function that aborts all transactional readers and writers
void TMIdealLECoherence::abortTMSharers(Pid_t pid, VAddr caddr, TMAbortType_e abortType) {
    // Collect sharers
    set<Pid_t> aborted;
    if(numWriters(caddr) != 0) {
        aborted.insert(writers.at(caddr).begin(), writers.at(caddr).end());
    }
    if(numReaders(caddr) != 0) {
        aborted.insert(readers.at(caddr).begin(), readers.at(caddr).end());
    }
    aborted.erase(pid);

    if(aborted.size() > 0) {
        // Do the abort
        markTransAborted(aborted, pid, caddr, abortType);
    }
    invalidateLines(pid, caddr);
}

///
// Helper function that cleans dirty lines in each cache except pid's.
void TMIdealLECoherence::cleanDirtyLines(Pid_t pid, VAddr raddr) {
    Cache* myCache = getCache(pid);

    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        if(cache != myCache) {
            Line* line = cache->findLine(raddr);
            if(line && line->isValid() && line->isDirty()) {
                line->makeClean();
            }
        }
    }
}

///
// Helper function that invalidates lines except pid's.
void TMIdealLECoherence::invalidateLines(Pid_t pid, VAddr raddr) {
    Cache* myCache = getCache(pid);

    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        if(cache != myCache) {
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

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        abortTMWriters(pid, caddr, TM_ATYPE_DEFAULT);
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

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        abortTMSharers(pid, caddr, TM_ATYPE_DEFAULT);
        line  = replaceLine(pid, raddr);
    } else if(line->isDirty() == false) {
        p_opStatus->wasHit = false;
        abortTMSharers(pid, caddr, TM_ATYPE_DEFAULT);
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

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        abortTMWriters(pid, caddr, TM_ATYPE_NONTM);
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

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        abortTMSharers(pid, caddr, TM_ATYPE_NONTM);
        line  = replaceLine(pid, raddr);
    } else if(line->isDirty() == false) {
        p_opStatus->wasHit = false;
        abortTMSharers(pid, caddr, TM_ATYPE_NONTM);
    } else {
        p_opStatus->wasHit = true;
    }

    // Update line
    line->makeDirty();
}

void TMIdealLECoherence::removeTransaction(Pid_t pid) {
    Cache* cache = getCache(pid);
    cache->clearTransactional();

    removeTrans(pid);
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
        if(line) {
            Pid_t writer = line->getWriter();
            if(writer != INVALID_PID && writer != pid) {
                if(isTM) {
                    markTransAborted(writer, pid, caddr, TM_ATYPE_DEFAULT);
                } else {
                    markTransAborted(writer, pid, caddr, TM_ATYPE_NONTM);
                }
                // but don't invalidate line
                line->makeClean();

                fwdGetSConflictMsg.inc();
            } else if(!line->isTransactional() && line->isDirty()) {
                line->makeClean();

                fwdGetSMsg.inc();
            }
        }
    } // End foreach(cache)
}


void TMLECoherence::removeTransaction(Pid_t pid) {
    Cache* cache = getCache(pid);
    cache->clearTransactional();
    overflow[pid].clear();
    removeTrans(pid);
}

/////////////////////////////////////////////////////////////////////////////////////////
// Eager-eager coherence. Tries to mimic LogTM as closely as possible
/////////////////////////////////////////////////////////////////////////////////////////
TMEECoherence::TMEECoherence(const char tmStyle[], int32_t nProcs, int32_t line):
        TMCoherence(tmStyle, nProcs, line) {

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
TMEECoherence::~TMEECoherence() {
    while(caches.size() > 0) {
        Cache* cache = caches.back();
        caches.pop_back();
        delete cache;
    }
}

uint32_t TMEECoherence::getNackRetryStallCycles(ThreadContext* context) {
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

size_t TMEECoherence::numWriters(VAddr caddr) const {
    auto i_line = wBits.find(caddr);
    if(i_line == wBits.end()) {
        return 0;
    } else {
        return 1;
    }
}
size_t TMEECoherence::numReaders(VAddr caddr) const {
    auto i_line = rBits.find(caddr);
    if(i_line == rBits.end()) {
        return 0;
    } else {
        return i_line->second.size();
    }
}

///
// Return true if pid is higher or equal priority than conflictPid
bool TMEECoherence::isHigherOrEqualPriority(Pid_t pid, Pid_t conflictPid) {
    Time_t otherTimestamp = getStartTime(conflictPid);
    Time_t myTimestamp = getStartTime(pid);

    return myTimestamp <= otherTimestamp;
}

///
// We have a conflict, so either NACK pid (the requester), or if there is a circular NACK, abort
TMRWStatus TMEECoherence::handleConflict(Pid_t pid, std::set<Pid_t>& conflicting, VAddr caddr) {
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
        nackCount[pid]++;
        nackedBy[pid] = highestPid;
        return TMRW_NACKED;
    }
}

TMEECoherence::Line* TMEECoherence::lookupLine(Pid_t pid, VAddr raddr, MemOpStatus* p_opStatus) {
    Cache* cache = getCache(pid);
	VAddr  caddr = addrToCacheLine(raddr);
    VAddr  myTag = cache->calcTag(raddr);

    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        line  = cache->findLine2Replace(raddr);

        // Invalidate old line
        if(line->isValid()) {
            line->invalidate();
        }

        // Replace the line
        line->validate(myTag, caddr);
    } else {
        p_opStatus->wasHit = true;
    }
    return line;
}

///
// Do a transactional read.
TMRWStatus TMEECoherence::TMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);

    std::set<Pid_t> conflicting;
    if(wBits.find(caddr) != wBits.end() && wBits[caddr] != pid) {
        conflicting.insert(wBits.at(caddr));
    }
    if(conflicting.size() > 0) {
        return handleConflict(pid, conflicting, caddr);
    }

    // Clear nackCount if we go through
    nackCount[pid] = 0;
    nackedBy.erase(pid);

    // Do the read
    Line*   line  = lookupLine(pid, raddr, p_opStatus);
    line->markTransactional();
    if(find(rBits[caddr].begin(), rBits[caddr].end(), pid) == rBits[caddr].end()) {
        rBits[caddr].push_back(pid);
    }
    readTrans(pid, raddr, caddr);

    return TMRW_SUCCESS;
}

///
// Do a transactional write.
TMRWStatus TMEECoherence::TMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);

    std::set<Pid_t> conflicting;
    if(wBits.find(caddr) != wBits.end() && wBits[caddr] != pid) {
        conflicting.insert(wBits.at(caddr));
    }
    if(rBits.find(caddr) != rBits.end()) {
        conflicting.insert(rBits.at(caddr).begin(), rBits.at(caddr).end());
        conflicting.erase(pid);
    }

    if(conflicting.size() > 0) {
        return handleConflict(pid, conflicting, caddr);
    }

    // Clear nackCount if we go through
    nackCount[pid] = 0;
    nackedBy.erase(pid);

    // Do the write
    Line*   line  = lookupLine(pid, raddr, p_opStatus);
    line->markTransactional();
    line->makeTransactionalDirty(pid);

    wBits[caddr] = pid;
    if(find(rBits[caddr].begin(), rBits[caddr].end(), pid) == rBits[caddr].end()) {
        rBits[caddr].push_back(pid);
    }
    writeTrans(pid, raddr, caddr);

    return TMRW_SUCCESS;
}

///
// Do a non-transactional read, i.e. when a thread not inside a transaction.
void TMEECoherence::nonTMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= getCache(pid);

    if(numWriters(caddr) != 0) {
        markTransAborted(wBits.at(caddr), pid, caddr, TM_ATYPE_NONTM);
    }

    // Update line
    Line*   line  = lookupLine(pid, raddr, p_opStatus);
}

///
// Do a non-transactional write, i.e. when a thread not inside a transaction.
void TMEECoherence::nonTMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);

    set<Pid_t> aborted;
    if(numWriters(caddr) != 0) {
        aborted.insert(wBits.at(caddr));
    }
    if(numReaders(caddr) != 0) {
        aborted.insert(rBits.at(caddr).begin(), rBits.at(caddr).end());
    }
    markTransAborted(aborted, pid, caddr, TM_ATYPE_NONTM);

    // Update line
    Line*   line  = lookupLine(pid, raddr, p_opStatus);
    line->makeDirty();
}

///
// TM begin that also initializes firstStartTime
TMBCStatus TMEECoherence::myBegin(Pid_t pid, InstDesc* inst) {
    if(startTime.find(pid) == startTime.end()) {
        startTime[pid] = globalClock;
    }
    beginTrans(pid, inst);
    return TMBC_SUCCESS;
}
///
// TM commit also clears firstStartTime
TMBCStatus TMEECoherence::myCommit(Pid_t pid) {
    startTime.erase(pid);
    commitTrans(pid);
    return TMBC_SUCCESS;
}
///
// Clear firstStartTime when completing fallback, too
void TMEECoherence::completeFallback(Pid_t pid) {
    startTime.erase(pid);
}

void TMEECoherence::removeTransaction(Pid_t pid) {
    Cache* cache = getCache(pid);
    cache->clearTransactional();

    std::set<VAddr> accessed;
    accessed.insert(linesWritten[pid].begin(), linesWritten[pid].end());
    accessed.insert(linesRead[pid].begin(), linesRead[pid].end());

    for(VAddr caddr:  accessed) {
        if(wBits.find(caddr) != wBits.end() && wBits[caddr] == pid) {
            wBits.erase(caddr);
        }
        auto i_readers = rBits.find(caddr);
        if(i_readers == rBits.end()) {
            fail("[%d] RBit not set for 0x%x\n", pid, caddr);
        }

        list<Pid_t>& myReaders = i_readers->second;
        myReaders.remove(pid);
        if(myReaders.empty()) {
            rBits.erase(i_readers);
        }
    }

	cycleFlags[pid] = false;
    nackCount[pid] = 0;
    nackedBy.erase(pid);
    removeTrans(pid);
}

/////////////////////////////////////////////////////////////////////////////////////////
// Versino of EE Coherence that sets priority based on number of transactional reads
/////////////////////////////////////////////////////////////////////////////////////////
bool TMEENumReadsCoherence::isHigherOrEqualPriority(Pid_t pid, Pid_t conflictPid) {
    size_t otherNumReads = getNumReads(conflictPid);
    size_t myNumReads = getNumReads(pid);

    return myNumReads >= otherNumReads;
}

/////////////////////////////////////////////////////////////////////////////////////////
// PleaseTM with conflict resolution done with plea bits, using ideal cache
/////////////////////////////////////////////////////////////////////////////////////////
IdealPleaseTM::IdealPleaseTM(const char tmStyle[], int32_t nProcs, int32_t line):
        TMCoherence(tmStyle, nProcs, line) {

    int totalSize = SescConf->getInt("TransactionalMemory", "totalSize");
    int assoc = SescConf->getInt("TransactionalMemory", "assoc");

    for(Pid_t pid = 0; pid < nProcs; pid++) {
        caches.push_back(new CacheAssocTM(totalSize, assoc, lineSize, 1));
    }
}

///
// Destructor for PrivateCache. Delete all allocated members
IdealPleaseTM::~IdealPleaseTM() {
    while(caches.size() > 0) {
        Cache* cache = caches.back();
        caches.pop_back();
        delete cache;
    }
}

void IdealPleaseTM::abortOthers(Pid_t pid, VAddr raddr, set<Pid_t>& conflicting) {
	VAddr caddr = addrToCacheLine(raddr);

    // Collect transactions that would be aborted and remove from conflicting
    set<Pid_t>::iterator i_m = conflicting.begin();
    while(i_m != conflicting.end()) {
        if(shouldAbort(pid, raddr, *i_m)) {
            markTransAborted(*i_m, pid, caddr, TM_ATYPE_DEFAULT);
            conflicting.erase(i_m++);
        } else {
            ++i_m;
        }
    }
}

IdealPleaseTM::Line* IdealPleaseTM::lookupLine(Pid_t pid, VAddr raddr, MemOpStatus* p_opStatus) {
    Cache* cache = getCache(pid);
	VAddr  caddr = addrToCacheLine(raddr);
    VAddr  myTag = cache->calcTag(raddr);

    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        line  = cache->findLine2Replace(raddr);

        // Invalidate old line
        if(line->isValid()) {
            line->invalidate();
        }

        // Replace the line
        line->validate(myTag, caddr);
    } else {
        p_opStatus->wasHit = true;
    }
    return line;
}

///
// Do a transactional read.
TMRWStatus IdealPleaseTM::TMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);

    set<Pid_t> conflicting;
    for(Pid_t writer: writers[caddr]) {
        conflicting.insert(writer);
    }
    conflicting.erase(pid);
    abortOthers(pid, raddr, conflicting);

    if(conflicting.size() > 0) {
        markTransAborted(pid, (*conflicting.begin()), caddr, TM_ATYPE_DEFAULT);
        return TMRW_ABORT;
    }

    // Do the read
    Line*   line  = lookupLine(pid, raddr, p_opStatus);
    line->markTransactional();

    readTrans(pid, raddr, caddr);

    return TMRW_SUCCESS;
}

///
// Do a transactional write.
TMRWStatus IdealPleaseTM::TMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);

    set<Pid_t> conflicting;
    for(Pid_t writer: writers[caddr]) {
        conflicting.insert(writer);
    }
    for(Pid_t reader: readers[caddr]) {
        conflicting.insert(reader);
    }
    conflicting.erase(pid);
    abortOthers(pid, raddr, conflicting);

    if(conflicting.size() > 0) {
        markTransAborted(pid, (*conflicting.begin()), caddr, TM_ATYPE_DEFAULT);
        return TMRW_ABORT;
    }

    // Do the write
    Line*   line  = lookupLine(pid, raddr, p_opStatus);
    line->markTransactional();
    line->makeTransactionalDirty(pid);

    writeTrans(pid, raddr, caddr);

    return TMRW_SUCCESS;
}

///
// Do a non-transactional read, i.e. when a thread not inside a transaction.
void IdealPleaseTM::nonTMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= getCache(pid);

    set<Pid_t> conflicting;
    for(Pid_t writer: writers[caddr]) {
        conflicting.insert(writer);
    }
    markTransAborted(conflicting, pid, caddr, TM_ATYPE_NONTM);

    // Update line
    Line*   line  = lookupLine(pid, raddr, p_opStatus);
}

///
// Do a non-transactional write, i.e. when a thread not inside a transaction.
void IdealPleaseTM::nonTMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);

    set<Pid_t> conflicting;
    for(Pid_t writer: writers[caddr]) {
        conflicting.insert(writer);
    }
    for(Pid_t reader: readers[caddr]) {
        conflicting.insert(reader);
    }
    markTransAborted(conflicting, pid, caddr, TM_ATYPE_NONTM);

    // Update line
    Line*   line  = lookupLine(pid, raddr, p_opStatus);
    line->makeDirty();
}

void IdealPleaseTM::removeTransaction(Pid_t pid) {
    Cache* cache = getCache(pid);
    cache->clearTransactional();

    removeTrans(pid);
}

/////////////////////////////////////////////////////////////////////////////////////////
// IdealPleaseTM with requester always losing
/////////////////////////////////////////////////////////////////////////////////////////
TMIdealRequesterLoses::TMIdealRequesterLoses(const char tmStyle[], int32_t nProcs, int32_t line):
        IdealPleaseTM(tmStyle, nProcs, line) {
}

bool TMIdealRequesterLoses::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    return false;
}

/////////////////////////////////////////////////////////////////////////////////////////
// IdealPleaseTM with more reads wins
/////////////////////////////////////////////////////////////////////////////////////////
TMIdealMoreReadsWins::TMIdealMoreReadsWins(const char tmStyle[], int32_t nProcs, int32_t line):
        IdealPleaseTM(tmStyle, nProcs, line) {
}

bool TMIdealMoreReadsWins::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    return linesRead[other].size() < linesRead[pid].size();
}

/////////////////////////////////////////////////////////////////////////////////////////
// PleaseTM with conflict resolution done with plea bits
/////////////////////////////////////////////////////////////////////////////////////////
PleaseTM::PleaseTM(const char tmStyle[], int32_t nProcs, int32_t line):
        TMCoherence(tmStyle, nProcs, line) {

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
PleaseTM::~PleaseTM() {
    while(caches.size() > 0) {
        Cache* cache = caches.back();
        caches.pop_back();
        delete cache;
    }
}

PleaseTM::Line* PleaseTM::replaceLine(Pid_t pid, VAddr raddr) {
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);
    VAddr myTag = cache->calcTag(raddr);
    Line* line = nullptr;

    line = cache->findLine2Replace(raddr);
    if(line == nullptr) {
        fail("Replacement policy failed");
    }

    if(line->isValid() && line->isTransactional()) {
        abortReplaced(line, pid, caddr, TM_ATYPE_NONTM);
    }

    // Replace the line
    line->invalidate();
    line->validate(myTag, caddr);
    return line;
}

PleaseTM::Line* PleaseTM::replaceLineTM(Pid_t pid, VAddr raddr) {
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
void PleaseTM::abortReplaced(Line* replaced, Pid_t byPid, VAddr byCaddr, TMAbortType_e abortType) {
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
void PleaseTM::sendGetM(Pid_t pid, VAddr raddr, bool isTM) {
    //getMMsg.inc();
    //getMAck.inc();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* myCache = getCache(pid);

    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        Line* line = cache->findLine(raddr);
        if(line) {
            set<Pid_t> lineSharers;
            line->getAccessors(lineSharers);

            for(Pid_t s: lineSharers) {
                if(s != pid) {
                    if(isTM == false || shouldAbort(pid, raddr, s)) {
                        line->clearTransactional(s);
                        if(isTM) {
                            markTransAborted(s, pid, caddr, TM_ATYPE_DEFAULT);
                        } else {
                            markTransAborted(s, pid, caddr, TM_ATYPE_NONTM);
                        }
                    } else {
                        if(isTM) {
                            markTransAborted(pid, s, caddr, TM_ATYPE_DEFAULT);
                        } else {
                            markTransAborted(pid, s, caddr, TM_ATYPE_NONTM);
                        }
                    }
                }
            }
            if(myCache != cache) {
                // "Other" cache, so invalidate
                line->invalidate();
                //invMsg.inc();
                //invAck.inc();
            }
        }
    }

    // Look at everyone's overflow set to see if the line is in there
    for(Pid_t p = 0; p < (Pid_t)nProcs; ++p) {
        if(p != pid) {
            if(overflow[p].find(caddr) != overflow[p].end()) {
                if(isTM == false || shouldAbort(pid, raddr, p)) {
                    if(isTM) {
                        markTransAborted(p, pid, caddr, TM_ATYPE_DEFAULT);
                    } else {
                        markTransAborted(p, pid, caddr, TM_ATYPE_NONTM);
                    }
                } else {
                    if(isTM) {
                        markTransAborted(pid, p, caddr, TM_ATYPE_DEFAULT);
                    } else {
                        markTransAborted(pid, p, caddr, TM_ATYPE_NONTM);
                    }
                }
            }
        }
    } // End foreach(pid)
}

///
// Helper function that looks at all private caches and makes clean writers, while aborting
// transactional writers.
void PleaseTM::sendGetS(Pid_t pid, VAddr raddr, bool isTM) {
	VAddr caddr = addrToCacheLine(raddr);

    //getSMsg.inc();
    //getSAck.inc();

    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        Line* line = cache->findLine(raddr);
        if(line && line->isDirty()) {
            if(line->isTransactional()) {
                Pid_t writer = line->getWriter();
                if(isTM == false || shouldAbort(pid, raddr, writer)) {
                    if(isTM) {
                        markTransAborted(writer, pid, caddr, TM_ATYPE_DEFAULT);
                    } else {
                        markTransAborted(writer, pid, caddr, TM_ATYPE_NONTM);
                    }
                    // but don't invalidate line
                    line->makeClean();

                    //fwdGetSMsg.inc();
                    //fwdGetSAck.inc();
                } else {
                    if(isTM) {
                        markTransAborted(pid, writer, caddr, TM_ATYPE_DEFAULT);
                    } else {
                        markTransAborted(pid, writer, caddr, TM_ATYPE_NONTM);
                    }
                }
            } else {
                line->makeClean();

                //fwdGetSMsg.inc();
                //fwdGetSAck.inc();
            }
        }
    } // End foreach(cache)
}

///
// Do a transactional read.
TMRWStatus PleaseTM::TMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache = getCache(pid);

    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;

        sendGetS(pid, raddr, true);

        line  = replaceLineTM(pid, raddr);
    } else {
        p_opStatus->wasHit = true;
        if(line->isTransactional() == false && line->isDirty()) {
            // If we were the previous writer, make clean and start anew
            line->makeClean();
        }
    }

    // Return abort
    if(getState(pid) == TM_MARKABORT) {
        return TMRW_ABORT;
    }

    // Do the read
    line->markTransactional();
    line->addReader(pid);

    readTrans(pid, raddr, caddr);

    return TMRW_SUCCESS;
}

///
// Do a transactional write.
TMRWStatus PleaseTM::TMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache = getCache(pid);

    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;

        sendGetM(pid, raddr, true);

        if(getState(pid) == TM_MARKABORT) {
            return TMRW_ABORT;
        }

        line  = replaceLineTM(pid, raddr);
    } else if(line->isDirty() == false) {
        p_opStatus->wasHit = false;

        sendGetM(pid, raddr, true);
    } else {
        p_opStatus->wasHit = true;
    }

    // Return abort
    if(getState(pid) == TM_MARKABORT) {
        return TMRW_ABORT;
    }

    // Do the write
    line->markTransactional();
    line->makeTransactionalDirty(pid);

    writeTrans(pid, raddr, caddr);

    return TMRW_SUCCESS;
}

///
// Do a non-transactional read, i.e. when a thread not inside a transaction.
void PleaseTM::nonTMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= getCache(pid);

    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;

        sendGetS(pid, raddr, false);

        line  = replaceLine(pid, raddr);
    } else {
        p_opStatus->wasHit = true;
    }
}

///
// Do a non-transactional write, i.e. when a thread not inside a transaction.
void PleaseTM::nonTMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= getCache(pid);

    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;

        sendGetM(pid, raddr, false);

        line  = replaceLine(pid, raddr);
    } else if(line->isDirty() == false) {
        p_opStatus->wasHit = false;

        sendGetM(pid, raddr, false);
    } else {
        p_opStatus->wasHit = true;
    }

    // Update line
    line->makeDirty();
}

void PleaseTM::removeTransaction(Pid_t pid) {
    Cache* cache = getCache(pid);
    cache->clearTransactional();
    overflow[pid].clear();
    removeTrans(pid);
}

/////////////////////////////////////////////////////////////////////////////////////////
// PleaseTM with requester always losing
/////////////////////////////////////////////////////////////////////////////////////////
TMRequesterLoses::TMRequesterLoses(const char tmStyle[], int32_t nProcs, int32_t line):
        PleaseTM(tmStyle, nProcs, line) {
}

bool TMRequesterLoses::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    return false;
}

/////////////////////////////////////////////////////////////////////////////////////////
// PleaseTM with more reads wins
/////////////////////////////////////////////////////////////////////////////////////////
TMMoreReadsWinsCoherence::TMMoreReadsWinsCoherence(const char tmStyle[], int32_t nProcs, int32_t line):
        PleaseTM(tmStyle, nProcs, line) {
}

bool TMMoreReadsWinsCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    return linesRead[other].size() < linesRead[pid].size();
}
