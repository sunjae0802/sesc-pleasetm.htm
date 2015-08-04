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
    linesRead[pid].insert(caddr);
}
void TMCoherence::writeTrans(Pid_t pid, VAddr raddr, VAddr caddr) {
    linesWritten[pid].insert(caddr);
}
void TMCoherence::removeTrans(Pid_t pid) {
    transStates.at(pid).clear();
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
// Function that tells the TM engine that a fallback path for this transaction has been used,
// so reset any statistics. Used for statistics that run across multiple retires.
void TMCoherence::beginFallback(Pid_t pid, uint32_t pFallbackMutex) {
	VAddr caddr = addrToCacheLine(pFallbackMutex);
    fallbackMutexCAddrs.insert(pFallbackMutex);
}

///
// Function that tells the TM engine that a fallback path for this transaction has been completed.
void TMCoherence::completeFallback(Pid_t pid) {
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

size_t TMIdealLECoherence::numWriters(VAddr caddr) const {
    auto i_line = writers.find(caddr);
    if(i_line == writers.end()) {
        return 0;
    } else {
        return i_line->second.size();
    }
}
size_t TMIdealLECoherence::numReaders(VAddr caddr) const {
    auto i_line = readers.find(caddr);
    if(i_line == readers.end()) {
        return 0;
    } else {
        return i_line->second.size();
    }
}

///
// Helper function that replaces a line in the Cache
TMIdealLECoherence::Line* TMIdealLECoherence::replaceLine(Pid_t pid, VAddr raddr) {
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);
    VAddr myTag = cache->calcTag(raddr);

    Line* line  = cache->findLine2Replace(raddr);
    if(line->isValid()) {
        line->invalidate();
    }
    line->validate(myTag, caddr);

    return line;
}

///
// Helper function that aborts all transactional readers
void TMIdealLECoherence::abortTMReaders(Pid_t pid, VAddr caddr, TMAbortType_e abortType) {
    // Collect readers
    set<Pid_t> aborted;
    if(numWriters(caddr) != 0) {
        aborted.insert(writers.at(caddr).begin(), writers.at(caddr).end());
    }
    aborted.erase(pid);

    // Do the abort
    markTransAborted(aborted, pid, caddr, abortType);
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

    // Do the abort
    markTransAborted(aborted, pid, caddr, abortType);
}

///
// Do a transactional read.
TMRWStatus TMIdealLECoherence::TMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    // Lookup line
    Line*   line  = cache->lookupLine(raddr);
    if(line == NULL) {
        p_opStatus->wasHit = false;

        abortTMReaders(pid, caddr, TM_ATYPE_DEFAULT);

        line  = replaceLine(pid, raddr);
    } else {
        p_opStatus->wasHit = true;
    }

    // Do the read
    line->markTransactional();
    readers[caddr].insert(pid);

    readTrans(pid, raddr, caddr);

    return TMRW_SUCCESS;
}


///
// Do a transactional write.
TMRWStatus TMIdealLECoherence::TMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    // Lookup line
    Line*   line  = cache->lookupLine(raddr);
    if(line == NULL) {
        p_opStatus->wasHit = false;

        abortTMSharers(pid, caddr, TM_ATYPE_DEFAULT);

        line  = replaceLine(pid, raddr);
    } else if(line->isDirty() == false) {
        p_opStatus->wasHit = false;

        abortTMSharers(pid, caddr, TM_ATYPE_DEFAULT);

        // Do NOT replace line, though. We just need to mark dirty below
    } else {
        p_opStatus->wasHit = true;
    }

    // Do the write
    line->markTransactional();
    line->makeTransactionalDirty(pid);

    writers[caddr].insert(pid);
    writeTrans(pid, raddr, caddr);

    return TMRW_SUCCESS;
}

///
// Do a non-transactional read, i.e. when a thread not inside a transaction.
void TMIdealLECoherence::nonTMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    // Lookup line
    Line*   line  = cache->lookupLine(raddr);
    if(line == NULL) {
        p_opStatus->wasHit = false;

        abortTMReaders(pid, caddr, TM_ATYPE_NONTM);

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

    // Lookup line
    Line*   line  = cache->lookupLine(raddr);
    if(line == NULL) {
        p_opStatus->wasHit = false;

        abortTMSharers(pid, caddr, TM_ATYPE_NONTM);

        line  = replaceLine(pid, raddr);
    } else if(line->isDirty() == false) {
        p_opStatus->wasHit = false;

        abortTMSharers(pid, caddr, TM_ATYPE_NONTM);

        // Do NOT replace line, though. We just need to mark dirty below
    } else {
        p_opStatus->wasHit = true;
    }

    // Update line
    line->makeDirty();
}

void TMIdealLECoherence::removeTransaction(Pid_t pid) {
    Cache* cache = getCache(pid);
    cache->clearTransactional();

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

    removeTrans(pid);
}


/////////////////////////////////////////////////////////////////////////////////////////
// TSX-style coherence using Lazy-eager coherence with cache overflow aborts.
/////////////////////////////////////////////////////////////////////////////////////////
TMLECoherence::TMLECoherence(const char tmStyle[], int32_t nProcs, int32_t line):
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
    VAddr myTag = cache->calcTag(raddr);

    // Handle any sharers
    cleanWriters(pid, raddr, true);

    // Lookup line
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        line  = findLine2ReplaceTM(pid, raddr);

        if(getState(pid) == TM_MARKABORT) {
            return TMRW_ABORT;
        }

        updateOverflow(pid, caddr);

        // Replace the line
        line->invalidate();
        line->validate(myTag, caddr);
    } else {
        p_opStatus->wasHit = true;
        if(!line->isTransactional()) {
            // If we were the previous writer, make clean and start anew
            line->makeClean();
        }
    }

    // Update line
    line->markTransactional();
    line->addReader(pid);

    if(getState(pid) == TM_MARKABORT) {
        return TMRW_ABORT;
    } else {
        readTrans(pid, raddr, caddr);
        return TMRW_SUCCESS;
    }
}

///
// Do a transactional write.
TMRWStatus TMLECoherence::TMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= getCache(pid);
    VAddr myTag = cache->calcTag(raddr);

    // Handle any sharers
    invalidateSharers(pid, raddr, true);

    // Lookup line
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        line  = findLine2ReplaceTM(pid, raddr);

        if(getState(pid) == TM_MARKABORT) {
            return TMRW_ABORT;
        }

        updateOverflow(pid, caddr);

        // Replace the line
        line->invalidate();
        line->validate(myTag, caddr);
    } else {
        p_opStatus->wasHit = true;
    }

    // Update line
    line->markTransactional();
    line->makeTransactionalDirty(pid);

    if(getState(pid) == TM_MARKABORT) {
        return TMRW_ABORT;
    } else {
        writeTrans(pid, raddr, caddr);
        return TMRW_SUCCESS;
    }
}

///
// Do a non-transactional read, i.e. when a thread not inside a transaction.
void TMLECoherence::nonTMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= getCache(pid);
    VAddr myTag = cache->calcTag(raddr);

    // Handle any sharers
    cleanWriters(pid, raddr, false);

    // Lookup line
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        line  = findLine2Replace(pid, raddr);

        // Replace the line
        line->invalidate();
        line->validate(myTag, caddr);
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
    VAddr myTag = cache->calcTag(raddr);

    // Handle any sharers
    invalidateSharers(pid, raddr, false);

    // Lookup line
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        line  = findLine2Replace(pid, raddr);

        // Replace the line
        line->invalidate();
        line->validate(myTag, caddr);
    } else {
        p_opStatus->wasHit = true;
    }

    // Update line
    line->makeDirty();
}

TMLECoherence::Line* TMLECoherence::findLine2Replace(Pid_t pid, VAddr raddr) {
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    // Find line to replace
    LineNonTMComparator nonTMCmp;
    Line* line = cache->findOldestLine2Replace(raddr, nonTMCmp);
    if(line == nullptr) {
        LineNonTMOrCleanComparator nonTMCleanCmp;
        line = cache->findOldestLine2Replace(raddr, nonTMCleanCmp);
        if(line == nullptr) {
            line = cache->findOldestLine2Replace(raddr);
        }
    }

    if(line == nullptr) {
        fail("Replacement policy failed");
    }

    // Invalidate old line
    if(line->isValid() && line->isTransactional()) {
        abortReplaced(line, pid, caddr, TM_ATYPE_NONTM);
    }
    return line;
}

TMLECoherence::Line* TMLECoherence::findLine2ReplaceTM(Pid_t pid, VAddr raddr) {
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    // Find line to replace
    LineNonTMComparator nonTMCmp;
    Line* line = cache->findOldestLine2Replace(raddr, nonTMCmp);
    if(line == nullptr) {
        LineNonTMOrCleanComparator nonTMCleanCmp;
        line = cache->findOldestLine2Replace(raddr, nonTMCleanCmp);
        if(line == nullptr) {
            line = cache->findOldestLine2Replace(raddr);
        }
    }

    if(line == nullptr) {
        fail("Replacement policy failed");
    }

    if(line->isValid() && line->isTransactional()) {
        LineTMDirtyComparator dirtyCmp;
        if(cache->countLines(caddr, dirtyCmp) == cache->getAssoc()) {
            // Too many transactional dirty lines, just give up
            markTransAborted(pid, pid, caddr, TM_ATYPE_SETCONFLICT);
            return nullptr;
        }
        abortReplaced(line, pid, caddr, TM_ATYPE_SETCONFLICT);
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
// If this line had been sent to the overflow set, bring it back.
void TMLECoherence::updateOverflow(Pid_t pid, VAddr newCaddr) {
    if(overflow[pid].find(newCaddr) != overflow[pid].end()) {
        overflow[pid].erase(newCaddr);
    }
}

///
// Helper function that looks at all private caches and invalidates all sharers, while aborting
// transactions.
void TMLECoherence::invalidateSharers(Pid_t pid, VAddr raddr, bool isTM) {
    set<Pid_t> sharers;

    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        Line* line = cache->findLine(raddr);
        if(line) {
            set<Pid_t> lineSharers;
            line->getAccessors(lineSharers);

            for(Pid_t s: lineSharers) {
                if(s != pid) {
                    line->clearTransactional();
                    sharers.insert(s);
                }
            }
            if(getCache(pid) != cache) {
                // "Other" cache, so invalidate
                line->invalidate();
            }
        }
    }

	VAddr caddr = addrToCacheLine(raddr);
    // Look at everyone's overflow set to see if the line is in there
    for(Pid_t p = 0; p < (Pid_t)nProcs; ++p) {
        if(p != pid) {
            if(overflow[p].find(caddr) != overflow[p].end()) {
                sharers.insert(p);
            }
        }
    } // End foreach(pid)

    if(isTM) {
        markTransAborted(sharers, pid, caddr, TM_ATYPE_DEFAULT);
    } else if(fallbackMutexCAddrs.find(caddr) == fallbackMutexCAddrs.end()) {
        markTransAborted(sharers, pid, caddr, TM_ATYPE_NONTM);
    } else {
        markTransAborted(sharers, pid, caddr, TM_ATYPE_FALLBACK);
    }
}

///
// Helper function that looks at all private caches and makes clean writers, while aborting
// transactional writers.
void TMLECoherence::cleanWriters(Pid_t pid, VAddr raddr, bool isTM) {
	VAddr caddr = addrToCacheLine(raddr);

    for(Cache* cache: caches) {
        Line* line = cache->findLine(raddr);
        if(line) {
            Pid_t writer = line->getWriter();
            if(writer != INVALID_PID && writer != pid) {
                if(isTM) {
                    markTransAborted(writer, pid, caddr, TM_ATYPE_DEFAULT);
                } else if(fallbackMutexCAddrs.find(caddr) == fallbackMutexCAddrs.end()) {
                    markTransAborted(writer, pid, caddr, TM_ATYPE_NONTM);
                } else {
                    markTransAborted(writer, pid, caddr, TM_ATYPE_FALLBACK);
                }
                // but don't invalidate line
                line->clearTransactional();
                line->makeClean();
            } else if(!line->isTransactional() && line->isDirty()) {
                line->makeClean();
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
