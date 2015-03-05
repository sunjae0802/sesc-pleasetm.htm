#include <cmath>
#include <algorithm>
#include <iostream>
#include "libsuc/nanassert.h"
#include "SescConf.h"
#include "libll/Instruction.h"
#include "TMCoherence.h"

using namespace std;

TMCoherence *tmCohManager = 0;
uint64_t TMCoherence::nextUtid = 0;
/////////////////////////////////////////////////////////////////////////////////////////
// Factory function for all TM Coherence objects. Use different concrete classes
// depending on SescConf
/////////////////////////////////////////////////////////////////////////////////////////
TMCoherence *TMCoherence::create(int32_t nProcs) {
    TMCoherence* newCohManager;

    string method = SescConf->getCharPtr("TransactionalMemory","method");
    int totalSize = SescConf->getInt("TransactionalMemory", "totalSize");
    int assoc = SescConf->getInt("TransactionalMemory", "assoc");
    int lineSize = SescConf->getInt("TransactionalMemory","lineSize");
	int returnArgType = SescConf->getInt("TransactionalMemory","returnArgType");

    if(method == "LE") {
        newCohManager = new TMLECoherence(nProcs, totalSize, assoc, lineSize, returnArgType);
    } else {
        MSG("unknown TM method, using LE");
        newCohManager = new TMLECoherence(nProcs, totalSize, assoc, lineSize, returnArgType);
    }

    return newCohManager;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Abstract super-class of all TM policies. Contains the external interface and common
// implementations
/////////////////////////////////////////////////////////////////////////////////////////
TMCoherence::TMCoherence(const char tmStyle[], int32_t procs, int size, int a, int line, int argType):
        nProcs(procs), totalSize(size), assoc(a), lineSize(line), returnArgType(argType),
        nackStallBaseCycles(1), nackStallCap(1),
        numCommits("tm:numCommits"),
        numAborts("tm:numAborts"),
        abortTypes("tm:abortTypes"),
        tmLoads("tm:loads"),
        tmStores("tm:stores"),
        tmLoadMisses("tm:loadMisses"),
        tmStoreMisses("tm:storeMisses"),
        numAbortsCausedBeforeAbort("tm:numAbortsCausedBeforeAbort"),
        numAbortsCausedBeforeCommit("tm:numAbortsCausedBeforeCommit"),
        linesReadHist("tm:linesReadHist"),
        linesWrittenHist("tm:linesWrittenHist") {

    MSG("Using %s TM", tmStyle);

    for(Pid_t pid = 0; pid < nProcs; ++pid) {
        transStates.push_back(TransState(pid));
        // Initialize maps to enable at() use
        linesRead[pid].clear();
        linesWritten[pid].clear();
    }
}

bool TMCoherence::hadWrote(VAddr caddr, Pid_t pid) {
    return linesWritten[pid].find(caddr) != linesWritten[pid].end();
}

bool TMCoherence::hadRead(VAddr caddr, Pid_t pid) {
    return linesRead[pid].find(caddr) != linesRead[pid].end();
}
void TMCoherence::getWritersExcept(VAddr caddr, Pid_t pid, std::set<Pid_t>& w) {
    std::map<VAddr, std::list<Pid_t> >::iterator i_line;
    i_line = writers.find(caddr);
    if(i_line != writers.end()) {
        w.insert(i_line->second.begin(), i_line->second.end());
        w.erase(pid);
    }
}
void TMCoherence::getReadersExcept(VAddr caddr, Pid_t pid, std::set<Pid_t>& r) {
    std::map<VAddr, std::list<Pid_t> >::iterator i_line;
    i_line = readers.find(caddr);
    if(i_line != readers.end()) {
        r.insert(i_line->second.begin(), i_line->second.end());
        r.erase(pid);
    }
}
void TMCoherence::removeFromList(std::list<Pid_t>& list, Pid_t pid) {
    std::list<Pid_t>::iterator i_list = list.begin();
    while(i_list != list.end()) {
        if(*i_list == pid) {
            list.erase(i_list++);
        } else {
            ++i_list;
        }
    }
}

void TMCoherence::removeTransaction(Pid_t pid) {
    std::map<VAddr, std::list<Pid_t> >::iterator i_line;
    std::set<VAddr>::iterator i_wroteTo;
    for(i_wroteTo = linesWritten[pid].begin(); i_wroteTo != linesWritten[pid].end(); ++i_wroteTo) {
        i_line = writers.find(*i_wroteTo);
        if(i_line == writers.end()) {
            fail("linesWritten and writers mismatch\n");
        }
        removeFromList(i_line->second, pid);
        if(i_line->second.empty()) {
            writers.erase(i_line);
        }
        if(std::find(i_line->second.begin(), i_line->second.end(), pid) != i_line->second.end()) {
            fail("Remove fail?");
        }
    }
    std::set<VAddr>::iterator i_readFrom;
    for(i_readFrom = linesRead[pid].begin(); i_readFrom != linesRead[pid].end(); ++i_readFrom) {
        i_line = readers.find(*i_readFrom);
        if(i_line == readers.end()) {
            fail("linesRead and readers mismatch\n");
        }
        removeFromList(i_line->second, pid);
        if(i_line->second.empty()) {
            readers.erase(i_line);
        }
        if(std::find(i_line->second.begin(), i_line->second.end(), pid) != i_line->second.end()) {
            fail("Remove fail?");
        }
    }
    linesRead[pid].clear();
    linesWritten[pid].clear();
}

void TMCoherence::beginTrans(Pid_t pid, InstDesc* inst) {
	if(!transStates[pid].getRestartPending()) {
        // This is a new transaction instance
    } // Else a restarted transaction

    // Reset Statistics
    numAbortsCaused[pid] = 0;

    // Do the begin
    removeTransaction(pid);
	transStates[pid].begin(TMCoherence::nextUtid++);
}

void TMCoherence::commitTrans(Pid_t pid) {
    // Update Statistics
    numCommits.inc();
    numAbortsCausedBeforeCommit.add(numAbortsCaused[pid]);
    linesReadHist.sample(getNumReads(pid));
    linesWrittenHist.sample(getNumWrites(pid));

    // Do the commit
    removeTransaction(pid);
    transStates[pid].commit();
}
void TMCoherence::abortTrans(Pid_t pid) {
	transStates[pid].startAborting();
}
void TMCoherence::completeAbortTrans(Pid_t pid) {
    // Update Statistics
    numAborts.inc();
    numAbortsCausedBeforeAbort.add(numAbortsCaused[pid]);
    abortTypes.sample(transStates[pid].getAbortType());
    linesReadHist.sample(getNumReads(pid));
    linesWrittenHist.sample(getNumWrites(pid));

    // Do the completeAbort
    removeTransaction(pid);
    transStates[pid].completeAbort();
}

void TMCoherence::markTransAborted(Pid_t victimPid, Pid_t aborterPid, VAddr caddr, TMAbortType_e abortType) {
    uint64_t aborterUtid = transStates[aborterPid].getUtid();

    if(transStates[victimPid].getState() != TM_ABORTING && transStates[victimPid].getState() != TM_MARKABORT) {
        transStates[victimPid].markAbort(aborterPid, aborterUtid, caddr, abortType);
        removeTransaction(victimPid);
        if(victimPid != aborterPid && transStates[aborterPid].getState() == TM_RUNNING) {
            numAbortsCaused[aborterPid]++;
        }
    } // Else victim is already aborting, so leave it alone
}

void TMCoherence::markTransAborted(std::set<Pid_t>& aborted, Pid_t aborterPid, VAddr caddr, TMAbortType_e abortType) {
	set<Pid_t>::iterator i_aborted;
    for(i_aborted = aborted.begin(); i_aborted != aborted.end(); ++i_aborted) {
		if(*i_aborted == aborterPid) {
            fail("Aborter is also the aborted?");
        }
        markTransAborted(*i_aborted, aborterPid, caddr, abortType);
	}
}
void TMCoherence::readTrans(Pid_t pid, VAddr raddr, VAddr caddr) {
    I(transStates[pid].getState() == TM_RUNNING);

    if(!hadRead(caddr, pid)) {
        linesRead[pid].insert(caddr);
        readers[caddr].push_back(pid);
    } else {
        if(find(readers[caddr].begin(), readers[caddr].end(), pid)
                == readers[caddr].end()) {
            fail("readers and linesRead mistmatch in add\n");
        }
    }
}
void TMCoherence::writeTrans(Pid_t pid, VAddr raddr, VAddr caddr) {
    I(transStates[pid].getState() == TM_RUNNING);

    if(!hadWrote(caddr, pid)) {
        linesWritten[pid].insert(caddr);
        writers[caddr].push_back(pid);
    } else {
        if(find(writers[caddr].begin(), writers[caddr].end(), pid)
                == writers[caddr].end()) {
            fail("writers and linesWritten mistmatch in add\n");
        }
    }
}
void TMCoherence::nackTrans(Pid_t pid) {
    transStates[pid].startNacking();
}

///
// Entry point for TM begin operation. Check for nesting and then call the real begin.
TMBCStatus TMCoherence::begin(Pid_t pid, InstDesc* inst) {
    if(transStates[pid].getDepth() > 0) {
        fail("Nested transactions not tested\n");
		transStates[pid].beginNested();
		return TMBC_IGNORE;
	} else {
		return myBegin(pid, inst);
	}
}

///
// Entry point for TM begin operation. Check for nesting and then call the real begin.
TMBCStatus TMCoherence::commit(Pid_t pid, int tid) {
	if(transStates[pid].getState() == TM_MARKABORT) {
		return TMBC_ABORT;
	} else if(transStates[pid].getDepth() > 1) {
		transStates[pid].commitNested();
		return TMBC_IGNORE;
	} else {
		return myCommit(pid, tid);
	}
}

///
// Entry point for TM abort operation. If the abort type is driven externally (syscall/user),
// then mark the transaction as aborted, else 
TMBCStatus TMCoherence::abort(Pid_t pid, int tid, TMAbortType_e abortType) {
    if(abortType == TM_ATYPE_SYSCALL || abortType == TM_ATYPE_USER) {
        transStates[pid].markAbort(pid, transStates[pid].getUtid(), 0, abortType);
    } else if(abortType != 0) {
        // Abort type internal, so should not be set
        fail("Unknown abort type");
    }
    return myAbort(pid, tid);
}

///
// Entry point for TM complete abort operation (to be called after an aborted TM returns to
// tm.begin).
TMBCStatus TMCoherence::completeAbort(Pid_t pid) {
    if(transStates[pid].getState() == TM_ABORTING) {
        myCompleteAbort(pid);
    }
    return TMBC_SUCCESS;
}

///
// Function that tells the TM engine that a fallback path for this transaction has been used,
// so reset any statistics. Used for statistics that run across multiple retires.
void TMCoherence::completeFallback(Pid_t pid) {
    transStates[pid].completeFallback();
    removeTransaction(pid);
}

///
// Entry point for TM read operation. Checks transaction state and then calls the real read.
TMRWStatus TMCoherence::read(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
	if(transStates[pid].getState() == TM_MARKABORT) {
		return TMRW_ABORT;
	} else if(transStates[pid].getState() == TM_INVALID) {
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
	if(transStates[pid].getState() == TM_MARKABORT) {
		return TMRW_ABORT;
	} else if(transStates[pid].getState() == TM_INVALID) {
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
TMBCStatus TMCoherence::myAbort(Pid_t pid, int tid) {
	abortTrans(pid);
	return TMBC_SUCCESS;
}

///
// A basic type of TM commit if child does not override
TMBCStatus TMCoherence::myCommit(Pid_t pid, int tid) {
    commitTrans(pid);
    return TMBC_SUCCESS;
}

///
// A basic type of TM complete abort if child does not override
void TMCoherence::myCompleteAbort(Pid_t pid) {
    completeAbortTrans(pid);
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence. This is the most simple style of TM, and used in TSX
/////////////////////////////////////////////////////////////////////////////////////////
TMLECoherence::TMLECoherence(int32_t nProcs, int size, int a, int line, int argType):
        TMCoherence("Lazy/Eager", nProcs, size, a, line, argType) {
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
    for(size_t cid = caches.size() - 1; cid >= 0; cid--) {
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
    Cache* cache= caches.at(pid);
    VAddr myTag = cache->calcTag(raddr);
    bool setConflict = false;

    // Lookup line
    abortWriters(pid, raddr);

    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        line  = cache->findLine2Replace(true, raddr);

        // Invalidate old line
        if(line->isValid()) {
            if(line->isTransactional()) {
                if(line->isDirty()) {
                    markTransAborted(pid, pid, line->getCaddr(), TM_ATYPE_SETCONFLICT);
                    setConflict = true;
                } else {
                    overflow[pid].insert(line->getCaddr());
                    if(overflow[pid].size() > maxOverflowSize) {
                        markTransAborted(pid, pid, line->getCaddr(), TM_ATYPE_SETCONFLICT);
                        setConflict = true;
                    }
                }
            }

            // Invalidate line
            line->invalidate();
        }

        if(overflow[pid].find(caddr) != overflow[pid].end()) {
            overflow[pid].erase(caddr);
        }

        // Replace the line
        line->setTag(myTag);
        line->setCaddr(caddr);
    } else {
        p_opStatus->wasHit = true;
        if(!line->isTransactional()) {
            line->makeClean();
        }
    }
    line->markTransactional();

    if(setConflict) {
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
    Cache* cache= caches.at(pid);
    VAddr myTag = cache->calcTag(raddr);
    bool setConflict = false;

    // Lookup line
    invalidateSharers(pid, raddr);

    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        line  = cache->findLine2Replace(true, raddr);

        // Invalidate old line
        if(line->isValid()) {
            if(line->isTransactional()) {
                if(line->isDirty()) {
                    markTransAborted(pid, pid, line->getCaddr(), TM_ATYPE_SETCONFLICT);
                    setConflict = true;
                } else {
                    overflow[pid].insert(line->getCaddr());
                    if(overflow[pid].size() > maxOverflowSize) {
                        markTransAborted(pid, pid, line->getCaddr(), TM_ATYPE_SETCONFLICT);
                        setConflict = true;
                    }
                }
            }

            // Invalidate line
            line->invalidate();
        }
        if(overflow[pid].find(caddr) != overflow[pid].end()) {
            overflow[pid].erase(caddr);
        }

        // Replace the line
        line->setTag(myTag);
        line->setCaddr(caddr);
    } else {
        p_opStatus->wasHit = true;
    }
    line->markTransactional();
    line->makeDirty();

    if(setConflict) {
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
    Cache* cache= caches.at(pid);
    VAddr myTag = cache->calcTag(raddr);

    // Lookup line
    abortWriters(pid, raddr);

    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        line  = cache->findLine2Replace(false, raddr);

        // Invalidate old line
        if(line->isValid()) {
            line->invalidate();
        }

        // Replace the line
        line->setTag(myTag);
        line->setCaddr(caddr);
    } else {
        p_opStatus->wasHit = true;
    }
}

///
// Do a non-transactional write, i.e. when a thread not inside a transaction.
void TMLECoherence::nonTMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    Cache* cache= caches.at(pid);
    VAddr myTag = cache->calcTag(raddr);

    // Lookup line
    invalidateSharers(pid, raddr);

    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        line  = cache->findLine2Replace(false, raddr);

        // Invalidate old line
        if(line->isValid()) {
            line->invalidate();
        }

        // Replace the line
        line->setTag(myTag);
        line->setCaddr(caddr);
    } else {
        p_opStatus->wasHit = true;
    }
    line->makeDirty();
}

///
// Helper function that looks at all private caches and invalidates every active transaction.
void TMLECoherence::invalidateSharers(Pid_t pid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);

    for(Pid_t p = 0; p < (Pid_t)nProcs; ++p) {
        if(p != pid) {
            Line* line = findLine(p, raddr);
            if(line) {
                if(line->isTransactional()) {
                    markTransAborted(p, pid, caddr, TM_ATYPE_EVICTION);
                }
                line->invalidate();
            } else if(overflow[p].find(caddr) != overflow[p].end()) {
                markTransAborted(p, pid, caddr, TM_ATYPE_EVICTION);
            }
        }
    } // End foreach(pid)
}

///
// Helper function that looks at all private caches and invalidates every active transaction.
void TMLECoherence::abortWriters(Pid_t pid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);

    for(Pid_t p = 0; p < (Pid_t)nProcs; ++p) {
        if(p != pid) {
            Line* line = findLine(p, raddr);
            if(line) {
                if(line->isTransactional() && line->isDirty()) {
                    markTransAborted(p, pid, caddr, TM_ATYPE_EVICTION);
                }
                // but don't invalidate line
            }
        }
    } // End foreach(pid)
}

TMLECoherence::Line* TMLECoherence::findLine(Pid_t pid, VAddr raddr) {
    Cache* cache = caches.at(pid);
    return cache->findLine(raddr);
}

TMBCStatus TMLECoherence::myBegin(Pid_t pid, InstDesc* inst) {
    beginTrans(pid, inst);
    return TMBC_SUCCESS;
}

TMBCStatus TMLECoherence::myCommit(Pid_t pid, int tid) {
    Cache* cache = caches.at(pid);
    cache->clearTransactional();
    overflow[pid].clear();
    commitTrans(pid);
    return TMBC_SUCCESS;
}

void TMLECoherence::myCompleteAbort(Pid_t pid) {
    Cache* cache = caches.at(pid);
    cache->clearTransactional();
    overflow[pid].clear();
    completeAbortTrans(pid);
}

