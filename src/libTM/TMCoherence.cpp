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

TMCoherence *TMCoherence::create(int32_t nProcs) {
    TMCoherence* newCohManager;

    string method = SescConf->getCharPtr("TransactionalMemory","method");
    int cacheLineSize = SescConf->getInt("TransactionalMemory","cacheLineSize");
    int numLines = SescConf->getInt("TransactionalMemory","numLines");
	int returnArgType = SescConf->getInt("TransactionalMemory","returnArgType");
    if(method == "EE") {
        newCohManager = new TMEECoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LL") {
        newCohManager = new TMLLCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE") {
        newCohManager = new TMLECoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-Hourglass") {
        newCohManager = new TMLEHourglassCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-SOK") {
        newCohManager = new TMLESOKCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-SOK-Queue") {
        newCohManager = new TMLESOKQueueCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-SOA-Original") {
        newCohManager = new TMLESOA0Coherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-SOA2") {
        newCohManager = new TMLESOA2Coherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-Lock") {
        newCohManager = new TMLELockCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-Lock0") {
        newCohManager = new TMLELock0Coherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-WAR") {
        newCohManager = new TMLEWARCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-ATS") {
        newCohManager = new TMLEATSCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-ASet") {
        newCohManager = new TMLEAsetCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-Snoop") {
        newCohManager = new TMLESnoopCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "First") {
        newCohManager = new TMFirstWinsCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "Older") {
        newCohManager = new TMOlderCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "OlderAll") {
        newCohManager = new TMOlderAllCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "More") {
        newCohManager = new TMMoreCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "Log2More") {
        newCohManager = new TMLog2MoreCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "CappedMore") {
        newCohManager = new TMCappedMoreCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "NumAborts") {
        newCohManager = new TMNumAbortsCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "FirstNotify") {
        newCohManager = new TMFirstNotifyCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "MoreNotify") {
        newCohManager = new TMMoreNotifyCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "FirstNackRetry") {
        newCohManager = new TMFirstRetryCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "MoreNackRetry") {
        newCohManager = new TMMoreRetryCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "OlderNackRetry") {
        newCohManager = new TMOlderRetryCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "OlderAllRetry") {
        newCohManager = new TMOlderAllRetryCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "MoreLog2Retry") {
        newCohManager = new TMLog2MoreRetryCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else {
        MSG("unknown TM method, using EE");
        newCohManager = new TMEECoherence(nProcs, cacheLineSize, numLines, returnArgType);
    }

    return newCohManager;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Abstract super-class of all TM policies. Contains the external interface and common
// implementations
/////////////////////////////////////////////////////////////////////////////////////////
TMCoherence::TMCoherence(int32_t procs, int lineSize, int lines, int argType):
        nProcs(procs), cacheLineSize(lineSize), numLines(lines), returnArgType(argType),
        nackStallBaseCycles(1), nackStallCap(1), maxNacks(0),
        numCommits("tm:numCommits"), numAborts("tm:numAborts"), abortTypes("tm:abortTypes"),
        numAbortsCausedBeforeAbort("tm:numAbortsCausedBeforeAbort"),
        numAbortsCausedBeforeCommit("tm:numAbortsCausedBeforeCommit"),
        avgLinesRead("tm:avgLinesRead"), avgLinesWritten("tm:avgLinesWritten"),
        linesReadHist("tm:linesReadHist"), linesWrittenHist("tm:linesWrittenHist") {
    for(Pid_t pid = 0; pid < nProcs; ++pid) {
        transStates.push_back(TransState(pid));
        cacheLines[pid].clear();        // Initialize map to enable at() use
    }
}

void TMCoherence::addWrite(VAddr caddr, Pid_t pid) {
    if(!hadWrote(caddr, pid)) {
        linesWritten[pid].insert(caddr);
        writers2[caddr].push_back(pid);
    } else {
        if(find(writers2[caddr].begin(), writers2[caddr].end(), pid)
                == writers2[caddr].end()) {
            fail("writers2 and linesWritten mistmatch in add\n");
        }
    }
}
void TMCoherence::addRead(VAddr caddr, Pid_t pid) {
    if(!hadRead(caddr, pid)) {
        linesRead[pid].insert(caddr);
        readers2[caddr].push_back(pid);
    } else {
        if(find(readers2[caddr].begin(), readers2[caddr].end(), pid)
                == readers2[caddr].end()) {
            fail("readers2 and linesRead mistmatch in add\n");
        }
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
    i_line = writers2.find(caddr);
    if(i_line != writers2.end()) {
        w.insert(i_line->second.begin(), i_line->second.end());
        w.erase(pid);
    }
}
void TMCoherence::getReadersExcept(VAddr caddr, Pid_t pid, std::set<Pid_t>& r) {
    std::map<VAddr, std::list<Pid_t> >::iterator i_line;
    i_line = readers2.find(caddr);
    if(i_line != readers2.end()) {
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
        i_line = writers2.find(*i_wroteTo);
        if(i_line == writers2.end()) {
            fail("linesWritten and writers2 mismatch\n");
        }
        removeFromList(i_line->second, pid);
        if(i_line->second.empty()) {
            writers2.erase(i_line);
        }
        if(std::find(i_line->second.begin(), i_line->second.end(), pid) != i_line->second.end()) {
            fail("Remove fail?");
        }
    }
    std::set<VAddr>::iterator i_readFrom;
    for(i_readFrom = linesRead[pid].begin(); i_readFrom != linesRead[pid].end(); ++i_readFrom) {
        i_line = readers2.find(*i_readFrom);
        if(i_line == readers2.end()) {
            fail("linesRead and readers2 mismatch\n");
        }
        removeFromList(i_line->second, pid);
        if(i_line->second.empty()) {
            readers2.erase(i_line);
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
	cacheLines[pid].clear();
    numAbortsCaused[pid] = 0;
    removeTransaction(pid);
	transStates[pid].begin(TMCoherence::nextUtid++);
}
void TMCoherence::commitTrans(Pid_t pid) {
    numCommits.inc();
    numAbortsCausedBeforeCommit.add(numAbortsCaused[pid]);
    avgLinesRead.sample(linesRead[pid].size());
    avgLinesWritten.sample(linesWritten[pid].size());
    linesReadHist.sample(linesRead[pid].size());
    linesWrittenHist.sample(linesWritten[pid].size());
    transStates[pid].commit();
    removeTransaction(pid);
}
void TMCoherence::abortTrans(Pid_t pid) {
	transStates[pid].startAborting();
}
void TMCoherence::markTransAborted(Pid_t victimPid, Pid_t aborterPid, uint64_t aborterUtid, VAddr caddr, TMAbortType_e abortType) {
    if(transStates[victimPid].getState() != TM_ABORTING) {
        transStates[victimPid].markAbort(aborterPid, aborterUtid, caddr, abortType);
        removeTransaction(victimPid);
        if(victimPid != aborterPid && transStates[aborterPid].getState() == TM_RUNNING) {
            numAbortsCaused[aborterPid]++;
        }
    } // Else victim is already aborting, so leave it alone
}

void TMCoherence::markTransAborted(std::set<Pid_t>& aborted, Pid_t aborterPid, uint64_t aborterUtid, VAddr caddr, TMAbortType_e abortType) {
	set<Pid_t>::iterator i_aborted;
    for(i_aborted = aborted.begin(); i_aborted != aborted.end(); ++i_aborted) {
		if(*i_aborted == aborterPid) {
            fail("Aborter is also the aborted?");
        }
        markTransAborted(*i_aborted, aborterPid, aborterUtid, caddr, abortType);
	}
}
void TMCoherence::readTrans(Pid_t pid, int tid, VAddr raddr, VAddr caddr) {
    cacheLines[pid].insert(caddr);
    addRead(caddr, pid);
	I(transStates[pid].getState() == TM_RUNNING);
}
void TMCoherence::writeTrans(Pid_t pid, int tid, VAddr raddr, VAddr caddr) {
    cacheLines[pid].insert(caddr);
    addWrite(caddr, pid);

	I(transStates[pid].getState() == TM_RUNNING);
}
void TMCoherence::nackTrans(Pid_t pid) {
    transStates[pid].startNacking();
}

TMBCStatus TMCoherence::begin(Pid_t pid, InstDesc* inst) {
    if(transStates[pid].getDepth() > 0) {
        fail("Nested transactions not tested\n");
		transStates[pid].beginNested();
		return TMBC_IGNORE;
	} else {
		return myBegin(pid, inst);
	}
}

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

TMBCStatus TMCoherence::abort(Pid_t pid, int tid, TMAbortType_e abortType) {
    if(abortType == TM_ATYPE_SYSCALL || abortType == TM_ATYPE_USER) {
        transStates[pid].markAbort(pid, transStates[pid].getUtid(), 0, abortType);
    } else if(abortType != 0) {
        // Abort type internal, so should not be set
        fail("Unknown abort type");
    }
    return myAbort(pid, tid);
}

TMBCStatus TMCoherence::completeAbort(Pid_t pid) {
    if(transStates[pid].getState() == TM_ABORTING) {
        numAborts.inc();
        numAbortsCausedBeforeAbort.add(numAbortsCaused[pid]);
        abortTypes.sample(transStates[pid].getAbortType());
        avgLinesRead.sample(linesRead[pid].size());
        avgLinesWritten.sample(linesWritten[pid].size());
        linesReadHist.sample(linesRead[pid].size());
        linesWrittenHist.sample(linesWritten[pid].size());

        transStates[pid].completeAbort();
        removeTransaction(pid);

        Pid_t aborter = transStates[pid].getAborterPid();
        myCompleteAbort(pid);
    }
    return TMBC_SUCCESS;
}
void TMCoherence::completeFallback(Pid_t pid) {
    transStates[pid].completeFallback();
    removeTransaction(pid);
}

TMRWStatus TMCoherence::read(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
	if(transStates[pid].getState() == TM_MARKABORT) {
		return TMRW_ABORT;
	} else if(cacheOverflowed(pid, caddr)) {
		markTransAborted(pid, pid, transStates[pid].getUtid(), caddr, TM_ATYPE_CAPACITY);
		return TMRW_ABORT;
	} else {
        return myRead(pid, tid, raddr);
    }
}
TMRWStatus TMCoherence::write(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
	if(transStates[pid].getState() == TM_MARKABORT) {
		return TMRW_ABORT;
	} else if(cacheOverflowed(pid, caddr)) {
		markTransAborted(pid, pid, transStates[pid].getUtid(), caddr, TM_ATYPE_CAPACITY);
		return TMRW_ABORT;
	} else {
        return myWrite(pid, tid, raddr);
    }
}

///
// When a thread not inside a transaction conflicts with data read as part of
//  a transaction, abort the transaction.
TMRWStatus TMCoherence::nonTMread(Pid_t pid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);

    I(!hadRead(caddr, pid));
    I(!hadWrote(caddr, pid));

    // Abort writers once we try to read
    set<Pid_t> aborted;
    getWritersExcept(caddr, pid, aborted);

    markTransAborted(aborted, pid, INVALID_UTID, caddr, TM_ATYPE_NONTM);

    return TMRW_SUCCESS;
}

///
// When a thread not inside a transaction conflicts with data is accessed as part of
//  a transaction, abort the transaction.
TMRWStatus TMCoherence::nonTMwrite(Pid_t pid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);

    I(!hadRead(caddr, pid));
    I(!hadWrote(caddr, pid));

    // Abort everyone once we try to write
    set<Pid_t> aborted;
    getReadersExcept(caddr, pid, aborted);
    getWritersExcept(caddr, pid, aborted);

    markTransAborted(aborted, pid, INVALID_UTID, caddr, TM_ATYPE_NONTM);

    return TMRW_SUCCESS;
}

TMBCStatus TMCoherence::myBegin(Pid_t pid, InstDesc* inst) {
    beginTrans(pid, inst);
    return TMBC_SUCCESS;
}

TMBCStatus TMCoherence::myAbort(Pid_t pid, int tid) {
	abortTrans(pid);
	return TMBC_SUCCESS;
}

TMBCStatus TMCoherence::myCommit(Pid_t pid, int tid) {
    commitTrans(pid);
    return TMBC_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Eager-eager coherence. TMs update shared memory as soon as they write, and only
// on conflict do they try to roll back any updates that the transaction had made.
// Follows LogTM TM policy.
/////////////////////////////////////////////////////////////////////////////////////////
TMEECoherence::TMEECoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMCoherence(nProcs, lineSize, lines, argType), cycleFlags(nProcs) {
	cout<<"[TM] Eager/Eager Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
	abortVarStallCycles     = SescConf->getInt("TransactionalMemory","secondaryVarStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitVarStallCycles    = SescConf->getInt("TransactionalMemory","primaryVarStallCycles");
}
TMRWStatus TMEECoherence::myRead(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);

	// If we had been NACKed, we can now be released
	if(transStates[pid].getState() == TM_NACKED) {
		transStates[pid].resumeAfterNack();
	}

    if(writers2[caddr].size() >= 1 && !hadWrote(caddr, pid)) {
        list<Pid_t>::iterator i_writer = writers2[caddr].begin();
        Pid_t aborterPid = *i_writer;

        if(aborterPid == pid) {
            // Grab the first reader than isn't us
            ++i_writer;
            aborterPid = *i_writer;
        }

        Time_t nackTimestamp = transStates[aborterPid].getTimestamp();
        Time_t myTimestamp = transStates[pid].getTimestamp();

        if(nackTimestamp <= myTimestamp && cycleFlags[pid]) {
            uint64_t aborterUtid = transStates[aborterPid].getUtid();
            markTransAborted(pid, aborterPid, aborterUtid, caddr, TM_ATYPE_DEFAULT);
            return TMRW_ABORT;
        } else {
            if(nackTimestamp >= myTimestamp) {
                cycleFlags[aborterPid] = true;
            }

            nackTrans(pid);
            return TMRW_NACKED;
        }
    } else {
        readTrans(pid, tid, raddr, caddr);
        return TMRW_SUCCESS;
    }
}

TMRWStatus TMEECoherence::myWrite(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);

	// If we had been NACKed, we can now be released
	if(transStates[pid].getState() == TM_NACKED) {
		transStates[pid].resumeAfterNack();
	}

    if(readers2[caddr].size() > 1 || ((readers2[caddr].size() == 1) && !hadRead(caddr, pid))) {
        // If there is more than one reader, or there is a single reader who happens not to be us
        list<Pid_t>::iterator i_reader = readers2[caddr].begin();
        Pid_t aborterPid = *i_reader;

        if(aborterPid == pid) {
            // Grab the first reader than isn't us
            ++i_reader;
            aborterPid = *i_reader;
        }

        Time_t nackTimestamp = transStates[aborterPid].getTimestamp();
        Time_t myTimestamp = transStates[pid].getTimestamp();

        if(nackTimestamp <= myTimestamp && cycleFlags[pid]) {
            uint64_t aborterUtid = transStates[aborterPid].getUtid();
            markTransAborted(pid, aborterPid, aborterUtid, caddr, TM_ATYPE_DEFAULT);
            return TMRW_ABORT;
        } else {
            if(nackTimestamp >= myTimestamp) {
                cycleFlags[aborterPid] = true;
            }

            nackTrans(pid);
            return TMRW_NACKED;
        }
    } else if(writers2[caddr].size() > 1 || ((writers2[caddr].size() == 1) && !hadWrote(caddr, pid))) {
        list<Pid_t>::iterator i_writer = writers2[caddr].begin();
        Pid_t aborterPid = *i_writer;

        if(aborterPid == pid) {
            // Grab the first reader than isn't us
            ++i_writer;
            aborterPid = *i_writer;
        }

        Time_t nackTimestamp = transStates[aborterPid].getTimestamp();
        Time_t myTimestamp = transStates[pid].getTimestamp();

        if(nackTimestamp <= myTimestamp && cycleFlags[pid]) {
            uint64_t aborterUtid = transStates[aborterPid].getUtid();
            markTransAborted(pid, aborterPid, aborterUtid, caddr, TM_ATYPE_DEFAULT);
            return TMRW_ABORT;
        } else {
            if(nackTimestamp >= myTimestamp) {
                cycleFlags[aborterPid] = true;
            }

            nackTrans(pid);
            return TMRW_NACKED;
        }
    } else {
        writeTrans(pid, tid, raddr, caddr);
        return TMRW_SUCCESS;
    }
}

TMBCStatus TMEECoherence::myBegin(Pid_t pid, InstDesc* inst) {
    cycleFlags[pid] = false;
    beginTrans(pid, inst);
    return TMBC_SUCCESS;
}

TMBCStatus TMEECoherence::myAbort(Pid_t pid, int tid) {
	cycleFlags[pid] = false;
	abortTrans(pid);

	return TMBC_SUCCESS;
}

TMBCStatus TMEECoherence::myCommit(Pid_t pid, int tid) {
    commitTrans(pid);
    cycleFlags[pid] = false;

    return TMBC_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-lazy coherence. TMs are allowed to run until commit, which then they are checked
// for any memory conflicts. Follows Josep's group's TM policy
/////////////////////////////////////////////////////////////////////////////////////////
TMLLCoherence::TMLLCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Lazy Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	abortVarStallCycles     = SescConf->getInt("TransactionalMemory","primaryVarStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
	commitVarStallCycles    = SescConf->getInt("TransactionalMemory","secondaryVarStallCycles");

	currentCommitter = INVALID_PID; 
}
TMRWStatus TMLLCoherence::myRead(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);

    readTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMRWStatus TMLLCoherence::myWrite(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);

    writeTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMBCStatus TMLLCoherence::myCommit(Pid_t pid, int tid) {
    if(currentCommitter == INVALID_PID) {
        // Stop other transactions from being able to commit
        currentCommitter = pid;

        // "Lazily" check the read and write sets and abort anyone who conflicts with me
        uint64_t utid = transStates[pid].getUtid();
        set<VAddr>::iterator i_line;
        set<Pid_t> aborted;
        for(i_line = linesWritten[pid].begin(); i_line != linesWritten[pid].end(); ++i_line) {
            VAddr caddr = *i_line;

            aborted.clear();
            getReadersExcept(caddr, pid, aborted);
            getWritersExcept(caddr, pid, aborted);
            markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);
        }

        // Now do the "commit"
        commitTrans(pid);

        // Allow other transaction to commit again
        currentCommitter = INVALID_PID;
        return TMBC_SUCCESS;
    } else {
        nackTrans(pid);

        return TMBC_NACK;
    }
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence. This is the most simple style of TM, and used in TSX
/////////////////////////////////////////////////////////////////////////////////////////
TMLECoherence::TMLECoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}
TMRWStatus TMLECoherence::myRead(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort writers once we try to read
    set<Pid_t> aborted;
    getWritersExcept(caddr, pid, aborted);
    markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);

    readTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMRWStatus TMLECoherence::myWrite(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort everyone once we try to write
    set<Pid_t> aborted;
    getReadersExcept(caddr, pid, aborted);
    getWritersExcept(caddr, pid, aborted);
    markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);

    writeTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with Hourglass. If a transaction gets aborted more than a
// threshold number of times, the hourglass is triggered.
/////////////////////////////////////////////////////////////////////////////////////////
TMLEHourglassCoherence::TMLEHourglassCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMCoherence(nProcs, lineSize, lines, argType), abortThreshold(2), hourglassOwner(INVALID_HOURGLASS) {
	cout<<"[TM] Lazy/Eager with Hourglass Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}

TMRWStatus TMLEHourglassCoherence::myRead(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort writers once we try to read
    set<Pid_t> aborted;
    getWritersExcept(caddr, pid, aborted);
    markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);

    readTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMRWStatus TMLEHourglassCoherence::myWrite(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort everyone once we try to write
    set<Pid_t> aborted;
    getReadersExcept(caddr, pid, aborted);
    getWritersExcept(caddr, pid, aborted);
    markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);

    writeTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMBCStatus TMLEHourglassCoherence::myBegin(Pid_t pid, InstDesc* inst) {
    if(hourglassOwner != INVALID_HOURGLASS && hourglassOwner != pid) {
        return TMBC_NACK;
    } else {
        beginTrans(pid, inst);
        return TMBC_SUCCESS;
    }
}

void TMLEHourglassCoherence::myCompleteAbort(Pid_t pid) {
    abortCount[pid]++;

    if(abortCount[pid] > abortThreshold && hourglassOwner == INVALID_HOURGLASS) {
        hourglassOwner = pid;
    }
}

TMBCStatus TMLEHourglassCoherence::myCommit(Pid_t pid, int tid) {
    if(hourglassOwner == pid) {
        hourglassOwner = INVALID_HOURGLASS;
    }
    abortCount[pid] = 0;
    commitTrans(pid);

    return TMBC_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with Serialize-on-Killer. Each aborted transaction will block
// until the killer transaction commits.
/////////////////////////////////////////////////////////////////////////////////////////
TMLESOKCoherence::TMLESOKCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with SOK Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}

TMRWStatus TMLESOKCoherence::myRead(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort writers once we try to read
    set<Pid_t> aborted;
    getWritersExcept(caddr, pid, aborted);
    markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);

    readTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMRWStatus TMLESOKCoherence::myWrite(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort everyone once we try to write
    set<Pid_t> aborted;
    getReadersExcept(caddr, pid, aborted);
    getWritersExcept(caddr, pid, aborted);
    markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);

    writeTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

bool TMLESOKCoherence::inRunQueue(Pid_t pid) {
    std::map<Pid_t, std::list<Pid_t> >::iterator i_runQueue;
    for(i_runQueue = runQueues.begin(); i_runQueue != runQueues.end(); ++i_runQueue) {
        std::list<Pid_t>& runQueue = i_runQueue->second;
        if(find(runQueue.begin(), runQueue.end(), pid) != runQueue.end()) {
            return true;
        }
    }
    return false;
}
TMBCStatus TMLESOKCoherence::myBegin(Pid_t pid, InstDesc* inst) {
    if(inRunQueue(pid)) {
        return TMBC_NACK;
    } else {
        beginTrans(pid, inst);
        return TMBC_SUCCESS;
    }
}

void TMLESOKCoherence::myCompleteAbort(Pid_t pid) {
    Pid_t aborter = transStates[pid].getAborterPid();

    if(transStates[aborter].getState() != TM_INVALID) {
        runQueues[aborter].push_back(pid);
    }
}

TMBCStatus TMLESOKCoherence::myCommit(Pid_t pid, int tid) {
    runQueues[pid].clear();
    commitTrans(pid);

    return TMBC_SUCCESS;
}
/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with Serialize-on-Killer. Each aborted transaction will block
// until the killer transaction commits.
/////////////////////////////////////////////////////////////////////////////////////////
TMLESOKQueueCoherence::TMLESOKQueueCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with SOK with Queue Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}

TMRWStatus TMLESOKQueueCoherence::myRead(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort writers once we try to read
    set<Pid_t> aborted;
    getWritersExcept(caddr, pid, aborted);
    markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);

    readTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMRWStatus TMLESOKQueueCoherence::myWrite(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort everyone once we try to write
    set<Pid_t> aborted;
    getReadersExcept(caddr, pid, aborted);
    getWritersExcept(caddr, pid, aborted);
    markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);

    writeTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

bool TMLESOKQueueCoherence::inRunQueue(Pid_t pid) {
    std::map<Pid_t, std::list<Pid_t> >::iterator i_runQueue;
    for(i_runQueue = runQueues.begin(); i_runQueue != runQueues.end(); ++i_runQueue) {
        std::list<Pid_t>& runQueue = i_runQueue->second;
        if(find(runQueue.begin(), runQueue.end(), pid) != runQueue.end()) {
            return true;
        }
    }
    return false;
}
TMBCStatus TMLESOKQueueCoherence::myBegin(Pid_t pid, InstDesc* inst) {
    if(inRunQueue(pid)) {
        return TMBC_NACK;
    } else {
        abortCause[pid] = 0;
        beginTrans(pid, inst);
        return TMBC_SUCCESS;
    }
}

void TMLESOKQueueCoherence::myCompleteAbort(Pid_t pid) {
    abortCount[pid]++;
    Pid_t aborter = transStates[pid].getAborterPid();

    if(transStates[aborter].getState() != TM_INVALID) {
        runQueues[aborter].push_back(pid);
        abortCause[pid] = transStates[pid].getAbortBy();
    }
}

TMBCStatus TMLESOKQueueCoherence::myCommit(Pid_t pid, int tid) {
    if(runQueues[pid].size() > 0) {
        std::list<Pid_t>::iterator i_runQueueMember;
        i_runQueueMember = runQueues[pid].begin();

        Pid_t front = *i_runQueueMember;
        VAddr firstAddr = abortCause[*i_runQueueMember];
        runQueues[pid].erase(i_runQueueMember++);

        while(i_runQueueMember != runQueues[pid].end()) {
            if(abortCause[*i_runQueueMember] == firstAddr) {
                ++i_runQueueMember;
            } else {
                runQueues[pid].erase(i_runQueueMember++);
            }
        }
        if(runQueues[pid].size() > 0) {
            runQueues[front].insert(runQueues[front].begin(),
                runQueues[pid].begin(), runQueues[pid].end());
            runQueues[pid].clear();
        }
    }
    commitTrans(pid);

    return TMBC_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with Serialize-on-Address. Each cache line which was "hot" will
// act as a reader-writer lock
/////////////////////////////////////////////////////////////////////////////////////////
TMLESOA0Coherence::TMLESOA0Coherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with SOA Original Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}
TMRWStatus TMLESOA0Coherence::myRead(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort writers once we try to read
    set<Pid_t> aborted;
    getWritersExcept(caddr, pid, aborted);
    markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);

    readTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMRWStatus TMLESOA0Coherence::myWrite(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort everyone once we try to write
    set<Pid_t> aborted;
    getReadersExcept(caddr, pid, aborted);
    getWritersExcept(caddr, pid, aborted);
    markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);

    writeTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

bool TMLESOA0Coherence::inRunQueue(Pid_t pid) {
    std::map<VAddr, std::list<Pid_t> >::iterator i_runQueue;
    for(i_runQueue = runQueues.begin(); i_runQueue != runQueues.end(); ++i_runQueue) {
        std::list<Pid_t>& runQueue = i_runQueue->second;
        if(find(runQueue.begin(), runQueue.end(), pid) != runQueue.end()) {
            return true;
        }
    }
    return false;
}

TMBCStatus TMLESOA0Coherence::myBegin(Pid_t pid, InstDesc* inst) {
    if(inRunQueue(pid)) {
        return TMBC_NACK;
    } else {
        beginTrans(pid, inst);
        return TMBC_SUCCESS;
    }
}

void TMLESOA0Coherence::myCompleteAbort(Pid_t pid) {
    Pid_t aborter = transStates[pid].getAborterPid();
    VAddr abortAddr = transStates[pid].getAbortBy();

    if(transStates[aborter].getState() != TM_INVALID) {
        runQueues[abortAddr].push_back(pid);
        lockList[aborter].insert(abortAddr);
    }
}

TMBCStatus TMLESOA0Coherence::myCommit(Pid_t pid, int tid) {
    std::set<VAddr>::iterator i_lockAddr;
    for(i_lockAddr = lockList[pid].begin(); i_lockAddr != lockList[pid].end(); ++i_lockAddr) {
        runQueues[*i_lockAddr].clear();
    }
    lockList[pid].clear();
    commitTrans(pid);

    return TMBC_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with Serialize-on-Address. Each cache line which was "hot" will
// act as a reader-writer lock
/////////////////////////////////////////////////////////////////////////////////////////
TMLESOA2Coherence::TMLESOA2Coherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with SOA 2 Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}
TMRWStatus TMLESOA2Coherence::myRead(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort writers once we try to read
    set<Pid_t> aborted;
    getWritersExcept(caddr, pid, aborted);

    set<Pid_t>::iterator i_aborted;
    for(i_aborted = aborted.begin(); i_aborted != aborted.end(); ++i_aborted) {
        runQueues[caddr].push_back(*i_aborted);
        lockList[pid].insert(caddr);
        markTransAborted(*i_aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);
    }

    readTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMRWStatus TMLESOA2Coherence::myWrite(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort everyone once we try to write
    set<Pid_t> aborted;
    getReadersExcept(caddr, pid, aborted);
    getWritersExcept(caddr, pid, aborted);

    set<Pid_t>::iterator i_aborted;
    for(i_aborted = aborted.begin(); i_aborted != aborted.end(); ++i_aborted) {
        runQueues[caddr].push_back(*i_aborted);
        lockList[pid].insert(caddr);
        markTransAborted(*i_aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);
    }

    writeTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

bool TMLESOA2Coherence::inRunQueue(Pid_t pid) {
    std::map<VAddr, std::list<Pid_t> >::iterator i_runQueue;
    for(i_runQueue = runQueues.begin(); i_runQueue != runQueues.end(); ++i_runQueue) {
        std::list<Pid_t>& runQueue = i_runQueue->second;
        if(find(runQueue.begin(), runQueue.end(), pid) != runQueue.end()) {
            return true;
        }
    }
    return false;
}

TMBCStatus TMLESOA2Coherence::myBegin(Pid_t pid, InstDesc* inst) {
    if(inRunQueue(pid)) {
        return TMBC_NACK;
    } else {
        beginTrans(pid, inst);
        return TMBC_SUCCESS;
    }
}

TMBCStatus TMLESOA2Coherence::myCommit(Pid_t pid, int tid) {
    std::set<VAddr>::iterator i_lockAddr;
    for(i_lockAddr = lockList[pid].begin(); i_lockAddr != lockList[pid].end(); ++i_lockAddr) {
        runQueues[*i_lockAddr].clear();
    }
    lockList[pid].clear();
    commitTrans(pid);

    return TMBC_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with WAR chest. The war chest collects read lines that were
// aborted (written) by someone else, so that if we don't ever touch it any more we
// avoid getting aborted (NEED PROOF).
/////////////////////////////////////////////////////////////////////////////////////////
TMLEWARCoherence::TMLEWARCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with WAR chest Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}

TMRWStatus TMLEWARCoherence::myRead(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    if(warChest[pid].find(caddr) != warChest[pid].end()) {
        markTransAborted(pid, pid, utid, caddr, TM_ATYPE_DEFAULT);
        return TMRW_ABORT;
    }
    if(warChest[pid].size() > 0 && hadRead(caddr, pid) == false) {
        markTransAborted(pid, pid, utid, caddr, TM_ATYPE_DEFAULT);
        return TMRW_ABORT;
    }

    // Abort writers once we try to read
    set<Pid_t> aborted;
    getWritersExcept(caddr, pid, aborted);
    markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);

    readTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMRWStatus TMLEWARCoherence::myWrite(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    if(warChest[pid].find(caddr) != warChest[pid].end()) {
        markTransAborted(pid, pid, utid, caddr, TM_ATYPE_DEFAULT);
        return TMRW_ABORT;
    }
    if(warChest[pid].size() > 0) {
        markTransAborted(pid, pid, utid, caddr, TM_ATYPE_DEFAULT);
        return TMRW_ABORT;
    }

    // Abort everyone once we try to write
    set<Pid_t> aborted;
    getWritersExcept(caddr, pid, aborted);
    markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);

    set<Pid_t> readers;
    getReadersExcept(caddr, pid, readers);
    set<Pid_t>::iterator i_reader;
    for(i_reader = readers.begin(); i_reader != readers.end(); ++i_reader) {
        if(hadWrote(caddr, *i_reader)) {
            markTransAborted(*i_reader, pid, utid, caddr, TM_ATYPE_DEFAULT);
        } else {
            warChest[*i_reader].insert(caddr);
            std::remove(readers2[caddr].begin(), readers2[caddr].end(), pid);
            linesRead[pid].erase(caddr);
        }
	}

    writeTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMBCStatus TMLEWARCoherence::myBegin(Pid_t pid, InstDesc* inst) {
    beginTrans(pid, inst);
    warChest[pid].clear();
    return TMBC_SUCCESS;
}

TMBCStatus TMLEWARCoherence::myAbort(Pid_t pid, int tid) {
    warChest[pid].clear();
	abortTrans(pid);

	return TMBC_SUCCESS;
}

TMBCStatus TMLEWARCoherence::myCommit(Pid_t pid, int tid) {
    warChest[pid].clear();
    commitTrans(pid);

    return TMBC_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with Adaptive Transaction Scheduling.
/////////////////////////////////////////////////////////////////////////////////////////
TMLEATSCoherence::TMLEATSCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMCoherence(nProcs, lineSize, lines, argType), alpha(0.3) {
	cout<<"[TM] Lazy/Eager with ATS Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}

TMRWStatus TMLEATSCoherence::myRead(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort writers once we try to read
    set<Pid_t> aborted;
    getWritersExcept(caddr, pid, aborted);
    markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);

    readTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMRWStatus TMLEATSCoherence::myWrite(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort everyone once we try to write
    set<Pid_t> aborted;
    getReadersExcept(caddr, pid, aborted);
    getWritersExcept(caddr, pid, aborted);
    markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);

    writeTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMBCStatus TMLEATSCoherence::myBegin(Pid_t pid, InstDesc* inst) {
    if(runQueue.size() > 0 && runQueue.front() != pid) {
        return TMBC_NACK;
    } else {
        beginTrans(pid, inst);
        return TMBC_SUCCESS;
    }
}
void TMLEATSCoherence::myCompleteAbort(Pid_t pid) {
    abortCount[pid] = alpha * abortCount[pid] + (1 - alpha);
    if(abortCount[pid] > 0.5) {
        runQueue.push_back(pid);
    }
}

TMBCStatus TMLEATSCoherence::myCommit(Pid_t pid, int tid) {
    commitTrans(pid);
    list<Pid_t>::iterator i_pid = find(runQueue.begin(), runQueue.end(), pid);
    if(i_pid != runQueue.end()) {
        runQueue.erase(i_pid);
    }
    abortCount[pid] = alpha * abortCount[pid];

    return TMBC_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with locked cache lines. Each cache line which was "hot" will
// act as a reader-writer lock
/////////////////////////////////////////////////////////////////////////////////////////
TMLELockCoherence::TMLELockCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with Locked Lines Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}

void TMLELockCoherence::addMember(HWGate& gate, Pid_t pid) {
    if(gate.getOwner() == pid) {
        fail("Owner cannot be member, too\n");
    }
    if(gate.isReader(pid)) {
        // Cannot be both a member and a reader
        if(gate.getReadOnly() == false) {
            fail("Reader when gate is not read-only\n");
        }
        gate.removeReader(pid);
    }
    if(gate.isMember(pid) == false) {
        gate.addMember(pid);
    }
}
HWGate& TMLELockCoherence::newGate(Pid_t pid, VAddr caddr, bool readOnly) {
    HWGate& newGate = gates.insert(std::make_pair(caddr, HWGate(pid))).first->second;

    addrGateCount[caddr]++;
    if(readOnly) {
        newGate.setReadOnly();
    }

    return newGate;
}

size_t TMLELockCoherence::getAbortAddrCount(VAddr caddr) {
    map<VAddr, size_t>::iterator i_abortAddrCount = abortAddrCount.find(caddr);

    return i_abortAddrCount != abortAddrCount.end() ? i_abortAddrCount->second : 0;
}

void TMLELockCoherence::markAbort(VAddr caddr, Pid_t pid, HWGate& gate, TMAbortType_e abortType) {
    if(gate.getOwner() == pid) {
        fail("Aborting gate owner?");
    }

    updateAbortAddr(caddr, 1);

    Pid_t ownerPid = gate.getOwner();
    accessed[pid].insert(caddr);
    markTransAborted(pid, ownerPid, transStates[ownerPid].getUtid(), caddr, abortType);
}

TMRWStatus TMLELockCoherence::myRead(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);

    if(getAbortAddrCount(caddr) > 0) {
        // This is a HOT address
        std::map<VAddr, HWGate>::iterator i_gate = gates.find(caddr);
        if(i_gate != gates.end()) {
            // The address is locked
            HWGate& gate = i_gate->second;
            Pid_t ownerPid = gate.getOwner();
            if(ownerPid == pid || gate.isReader(pid)) {
                // Do nothing
            } else if(ownerPid != pid && gate.getReadOnly()) {
                // A read-only lock owned by someone else
                if(gate.isReader(pid) == false) {
                    gate.addReader(pid);
                }
            } else {
                // I'm not the owner of an exclusive lock, so abort
                markAbort(caddr, pid, gate, TM_ATYPE_DEFAULT);
                return TMRW_ABORT;
            }
        } else {
            // A free hot address, so lock it as read-only
            newGate(pid, caddr, true);
        }
    } else {
        // An idle address, so handle like LE-coherence
        set<Pid_t> aborted;
        getWritersExcept(caddr, pid, aborted);

        if(aborted.size() > 0) {
            HWGate& gate = newGate(pid, caddr, true);
            set<Pid_t>::iterator i_aborted;
            for(i_aborted = aborted.begin(); i_aborted != aborted.end(); ++i_aborted) {
                markAbort(caddr, *i_aborted, gate, TM_ATYPE_DEFAULT);
            }
        }
    }

    accessed[pid].insert(caddr);

    readTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMRWStatus TMLELockCoherence::myWrite(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);

    if(getAbortAddrCount(caddr) > 0) {
        // This is a HOT address
        std::map<VAddr, HWGate>::iterator i_gate = gates.find(caddr);
        if(i_gate != gates.end()) {
            HWGate& gate = i_gate->second;
            Pid_t ownerPid = gate.getOwner();
            if(ownerPid == pid) {
                // I'm the owner, check if this was a read-only lock and abort others.
                if(gate.getReadOnly()) {
                    set<Pid_t> aborted;
                    getWritersExcept(caddr, pid, aborted);

                    if(aborted.size() > 0) {
                        set<Pid_t>::iterator i_aborted;
                        for(i_aborted = aborted.begin(); i_aborted != aborted.end(); ++i_aborted) {
                            markAbort(caddr, *i_aborted, gate, TM_ATYPE_DEFAULT);
                        }
                    }
                    gate.stopReads();
                }
            } else {
                // I'm not the owner of the lock, so abort
                markAbort(caddr, pid, gate, TM_ATYPE_DEFAULT);
                return TMRW_ABORT;
            }
        } else {
            // A free hot address, so lock it
            newGate(pid, caddr, false);
        }
    } else {
        // An idle address, so handle like LE-coherence
        set<Pid_t> aborted;
        getReadersExcept(caddr, pid, aborted);
        getWritersExcept(caddr, pid, aborted);

        if(aborted.size() > 0) {
            HWGate& gate = newGate(pid, caddr, false);
            set<Pid_t>::iterator i_aborted;
            for(i_aborted = aborted.begin(); i_aborted != aborted.end(); ++i_aborted) {
                markAbort(caddr, *i_aborted, gate, TM_ATYPE_DEFAULT);
            }
        }
    }

    accessed[pid].insert(caddr);
    writeTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMBCStatus TMLELockCoherence::myBegin(Pid_t pid, InstDesc* inst) {
    Pid_t nacker = 10240;
    std::map<VAddr, HWGate>::iterator i_gate;
    for(i_gate = gates.begin(); i_gate != gates.end(); ++i_gate) {
        if(i_gate->second.isMember(pid)) {
            nacker = i_gate->second.getOwner();
        }
    }
    if(nacker != 10240) {
        nackOwner[pid] = nacker;
        return TMBC_NACK;
	} else {
        nackOwner.erase(pid);
        beginTrans(pid, inst);
        return TMBC_SUCCESS;
    }
}

void TMLELockCoherence::updateAbortAddr(VAddr abortAddr, size_t count) {
    abortAddrCount[abortAddr] += count;
    lruList.remove(abortAddr);
    lruList.push_back(abortAddr);
    if(lruList.size() > 1024) {
        VAddr droppedAddr = lruList.front();
        abortAddrCount.erase(droppedAddr);
        addrGateCount.erase(droppedAddr);
        lruList.pop_front();
    }
}

void TMLELockCoherence::myCompleteAbort(Pid_t pid) {
    Pid_t aborter = transStates[pid].getAborterPid();
    VAddr abortAddr = transStates[pid].getAbortBy();

    // Step through the set of lines we accessed, and drop any HWGates I own and
    // add myself to any that others own
    std::map<VAddr, HWGate>::iterator i_gate;
    i_gate = gates.begin();
    while(i_gate != gates.end()) {
        VAddr caddr = i_gate->first;
        HWGate& gate = i_gate->second;
        if(gate.isOwner(pid)) {
            // I'm the owner, so drop the gate
            gates.erase(i_gate++);
        } else if(gate.isReader(pid) && caddr == abortAddr) {
            // Read but got aborted, so stall for writer to exit
            gate.remove(pid);
            addMember(gate, pid);
            ++i_gate;
        } else if(gate.isReader(pid)) {
            // Former readers do not need to be notified by gate owner anymore
            gate.removeReader(pid);
            ++i_gate;
        } else if(gate.isMember(pid)) {
            // Was stalled, so add to the END of the waiting list
            gate.remove(pid);
            addMember(gate, pid);
            ++i_gate;
        } else if(accessed[pid].find(caddr) != accessed[pid].end()) {
            // I'm not the owner, add to the waiting list
            addMember(gate, pid);
            ++i_gate;
        } else {
            // I have nothing to do with this gate, so skip
            ++i_gate;
        }
    }

    // Any gates not found above but were in accessed set are already removed
    // by the owner (owner committed/aborted already)
    accessed[pid].clear();
}

TMBCStatus TMLELockCoherence::myCommit(Pid_t pid, int tid) {
    // Excuse ourselves from the gates
    std::map<VAddr, HWGate>::iterator i_gate;
    i_gate = gates.begin();
    while(i_gate != gates.end()) {
        HWGate& gate = i_gate->second;
        if(gate.isOwner(pid)) {
            // I'm the owner, so drop the gate
            gates.erase(i_gate++);
        } else if(gate.isReader(pid) || gate.isMember(pid)) {
            gate.removeReader(pid);
            ++i_gate;
        } else {
            // I have nothing to do with this gate, so skip
            ++i_gate;
        }
    }
    accessed[pid].clear();

    // Finally we commit
    commitTrans(pid);

    return TMBC_SUCCESS;
}
/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with locked cache lines. Each cache line which was "hot" will
// act as a reader-writer lock
/////////////////////////////////////////////////////////////////////////////////////////
TMLELock0Coherence::TMLELock0Coherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with Locked Lines 0 Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}

void TMLELock0Coherence::addMember(HWGate& gate, Pid_t pid) {
    if(gate.getOwner() == pid) {
        fail("Owner cannot be member, too\n");
    }
    if(gate.isReader(pid)) {
        // Cannot be both a member and a reader
        if(gate.getReadOnly() == false) {
            fail("Reader when gate is not read-only\n");
        }
        gate.removeReader(pid);
    }
    if(gate.isMember(pid) == false) {
        gate.addMember(pid);
    }
}
HWGate& TMLELock0Coherence::newGate(Pid_t pid, VAddr caddr, bool readOnly) {
    HWGate& newGate = gates.insert(std::make_pair(caddr, HWGate(pid))).first->second;

    addrGateCount[caddr]++;
    if(readOnly) {
        newGate.setReadOnly();
    }

    return newGate;
}

size_t TMLELock0Coherence::getAbortAddrCount(VAddr caddr) {
    map<VAddr, size_t>::iterator i_abortAddrCount = abortAddrCount.find(caddr);

    return i_abortAddrCount != abortAddrCount.end() ? i_abortAddrCount->second : 0;
}

void TMLELock0Coherence::markAbort(VAddr caddr, Pid_t pid, HWGate& gate, TMAbortType_e abortType) {
    if(gate.getOwner() == pid) {
        fail("Aborting gate owner?");
    }

    updateAbortAddr(caddr, 1);

    Pid_t ownerPid = gate.getOwner();
    markTransAborted(pid, ownerPid, transStates[ownerPid].getUtid(), caddr, abortType);
}

TMRWStatus TMLELock0Coherence::myRead(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort writers once we try to read
    set<Pid_t> aborted;
    getWritersExcept(caddr, pid, aborted);

    if(aborted.size() > 0) {
        std::map<VAddr, HWGate>::iterator i_gate = gates.find(caddr);
        if(i_gate != gates.end() && i_gate->second.getOwner() != pid) {
            Pid_t oldOwner = i_gate->second.getOwner();
            i_gate->second.setOwner(pid);
            i_gate->second.addMember(oldOwner);
        } else {
            newGate(pid, caddr, false);
            i_gate = gates.find(caddr);
        }
        set<Pid_t>::iterator i_aborted;
        for(i_aborted = aborted.begin(); i_aborted != aborted.end(); ++i_aborted) {
            markAbort(caddr, *i_aborted, i_gate->second, TM_ATYPE_DEFAULT);
        }
    }

    readTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMRWStatus TMLELock0Coherence::myWrite(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);

    // An idle address, so handle like LE-coherence
    set<Pid_t> aborted;
    getReadersExcept(caddr, pid, aborted);
    getWritersExcept(caddr, pid, aborted);
    if(aborted.size() > 0) {
        std::map<VAddr, HWGate>::iterator i_gate = gates.find(caddr);
        if(i_gate != gates.end() && i_gate->second.getOwner() != pid) {
            Pid_t oldOwner = i_gate->second.getOwner();
            i_gate->second.setOwner(pid);
            i_gate->second.addMember(oldOwner);
        } else {
            newGate(pid, caddr, false);
            i_gate = gates.find(caddr);
        }
        set<Pid_t>::iterator i_aborted;
        for(i_aborted = aborted.begin(); i_aborted != aborted.end(); ++i_aborted) {
            markAbort(caddr, *i_aborted, i_gate->second, TM_ATYPE_DEFAULT);
        }
    }


    writeTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMBCStatus TMLELock0Coherence::myBegin(Pid_t pid, InstDesc* inst) {
    Pid_t nacker = 10240;
    std::map<VAddr, HWGate>::iterator i_gate;
    for(i_gate = gates.begin(); i_gate != gates.end(); ++i_gate) {
        if(i_gate->second.isMember(pid)) {
            nacker = i_gate->second.getOwner();
        }
    }
    if(nacker != 10240) {
        nackOwner[pid] = nacker;
        return TMBC_NACK;
	} else {
        nackOwner.erase(pid);
        beginTrans(pid, inst);
        return TMBC_SUCCESS;
    }
}

void TMLELock0Coherence::updateAbortAddr(VAddr abortAddr, size_t count) {
    abortAddrCount[abortAddr] += count;
    lruList.remove(abortAddr);
    lruList.push_back(abortAddr);
    if(lruList.size() > 1024) {
        VAddr droppedAddr = lruList.front();
        abortAddrCount.erase(droppedAddr);
        addrGateCount.erase(droppedAddr);
        lruList.pop_front();
    }
}

void TMLELock0Coherence::myCompleteAbort(Pid_t pid) {
}

TMBCStatus TMLELock0Coherence::myCommit(Pid_t pid, int tid) {
    // Excuse ourselves from the gates
    std::map<VAddr, HWGate>::iterator i_gate;
    i_gate = gates.begin();
    while(i_gate != gates.end()) {
        HWGate& gate = i_gate->second;
        if(gate.isOwner(pid)) {
            // I'm the owner, so drop the gate
            gates.erase(i_gate++);
        } else {
            // I have nothing to do with this gate, so skip
            ++i_gate;
        }
    }

    // Finally we commit
    commitTrans(pid);

    return TMBC_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with accessed sets. Threads will be organized into a runqueue,
// which is then checked one by one to determine which to let proceed afterwards.
/////////////////////////////////////////////////////////////////////////////////////////
TMLEAsetCoherence::TMLEAsetCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with Accessed Sets Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}

TMRWStatus TMLEAsetCoherence::myRead(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort writers once we try to read
    set<Pid_t> aborted;
    getWritersExcept(caddr, pid, aborted);
    markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);

    readTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMRWStatus TMLEAsetCoherence::myWrite(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort everyone once we try to write
    set<Pid_t> aborted;
    getReadersExcept(caddr, pid, aborted);
    getWritersExcept(caddr, pid, aborted);
    markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);

    accessed[pid].insert(caddr);
    writeTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

bool TMLEAsetCoherence::inRunQueue(Pid_t pid) {
    std::map<Pid_t, std::list<Pid_t> >::iterator i_runQueue;
    for(i_runQueue = runQueues.begin(); i_runQueue != runQueues.end(); ++i_runQueue) {
        std::list<Pid_t>& runQueue = i_runQueue->second;
        if(find(runQueue.begin(), runQueue.end(), pid) != runQueue.end()) {
            return true;
        }
    }
    return false;
}
TMBCStatus TMLEAsetCoherence::myBegin(Pid_t pid, InstDesc* inst) {
    if(inRunQueue(pid)) {
        return TMBC_NACK;
    } else {
        abortCause[pid] = 0;
        accessed[pid].clear();
        beginTrans(pid, inst);
        return TMBC_SUCCESS;
    }
}

void TMLEAsetCoherence::myCompleteAbort(Pid_t pid) {
    abortCount[pid]++;
    Pid_t aborter = transStates[pid].getAborterPid();

    if(transStates[aborter].getState() != TM_INVALID) {
        runQueues[aborter].push_back(pid);
        abortCause[pid] = transStates[pid].getAbortBy();
    }
}

TMBCStatus TMLEAsetCoherence::myCommit(Pid_t pid, int tid) {
    vector<VAddr> aset_common;
    if(runQueues[pid].size() > 0) {
        std::list<Pid_t>::iterator i_runQueueMember;
        i_runQueueMember = runQueues[pid].begin();

        Pid_t front = *i_runQueueMember;
        VAddr firstAddr = abortCause[*i_runQueueMember];
        runQueues[pid].erase(i_runQueueMember++);

        while(i_runQueueMember != runQueues[pid].end()) {
            aset_common.clear();
            set_intersection(accessed[pid].begin(), accessed[pid].end(),
                        accessed[*i_runQueueMember].begin(), accessed[*i_runQueueMember].end(),
                        back_inserter(aset_common));
            if(aset_common.size() > 0) {
                ++i_runQueueMember;
            } else {
                runQueues[pid].erase(i_runQueueMember++);
            }
        }
        if(runQueues[pid].size() > 0) {
            runQueues[front].insert(runQueues[front].begin(),
                runQueues[pid].begin(), runQueues[pid].end());
            runQueues[pid].clear();
        }
    }
    commitTrans(pid);

    return TMBC_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with activity snooping
/////////////////////////////////////////////////////////////////////////////////////////
TMLESnoopCoherence::TMLESnoopCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with Snooping Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
    for(Pid_t pid = 0; pid < nProcs; ++pid) {
        readLines[pid].clear();
        wroteLines[pid].clear();
    }
}
TMRWStatus TMLESnoopCoherence::myRead(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    for(Pid_t oPid = 0; oPid < nProcs; ++oPid) {
        if(oPid != pid && wroteLines.at(oPid).find(caddr) != wroteLines.at(oPid).end()) {
            markTransAborted(pid, oPid, utid, caddr, TM_ATYPE_DEFAULT);
            return TMRW_ABORT;
        }
    }

    // Abort writers once we try to read
    set<Pid_t> aborted;
    getWritersExcept(caddr, pid, aborted);
    markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);

    readLines.at(pid).insert(caddr);
    readTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMRWStatus TMLESnoopCoherence::myWrite(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    for(Pid_t oPid = 0; oPid < nProcs; ++oPid) {
        if(oPid != pid && wroteLines.at(oPid).find(caddr) != wroteLines.at(oPid).end()) {
            markTransAborted(pid, oPid, utid, caddr, TM_ATYPE_DEFAULT);
            return TMRW_ABORT;
        }
    }

    // Abort everyone once we try to write
    set<Pid_t> aborted;
    getReadersExcept(caddr, pid, aborted);
    getWritersExcept(caddr, pid, aborted);
    markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);

    wroteLines.at(pid).insert(caddr);
    writeTrans(pid, tid, raddr, caddr);
    return TMRW_SUCCESS;
}

TMBCStatus TMLESnoopCoherence::myBegin(Pid_t pid, InstDesc* inst) {
    if(aborters.find(pid) != aborters.end()) {
        return TMBC_NACK;
    } else {
        beginTrans(pid, inst);
        readLines[pid].clear();
        wroteLines[pid].clear();
        return TMBC_SUCCESS;
    }
}

TMBCStatus TMLESnoopCoherence::myAbort(Pid_t pid, int tid) {
	abortTrans(pid);
    readLines[pid].clear();
    wroteLines[pid].clear();

	return TMBC_SUCCESS;
}
void TMLESnoopCoherence::myCompleteAbort(Pid_t pid) {
    Pid_t aborter = transStates[pid].getAborterPid();

    if(transStates[aborter].getState() != TM_INVALID) {
        aborters[pid] = aborter;
    }
}

TMBCStatus TMLESnoopCoherence::myCommit(Pid_t pid, int tid) {
    commitTrans(pid);
    readLines[pid].clear();
    wroteLines[pid].clear();

    map<Pid_t, Pid_t>::iterator i_aborter = aborters.begin();
    while(i_aborter != aborters.end()) {
        if(i_aborter->second == pid) {
            aborters.erase(i_aborter++);
        } else {
            ++i_aborter;
        }
    }

    return TMBC_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with first wins
/////////////////////////////////////////////////////////////////////////////////////////
TMFirstWinsCoherence::TMFirstWinsCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with first wins Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}

bool TMFirstWinsCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    return false;
}

void TMFirstWinsCoherence::abortOthers(Pid_t pid, VAddr raddr, set<Pid_t>& conflicting) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Collect transactions that would be aborted and remove from conflicting
    set<Pid_t>::iterator i_m = conflicting.begin();
    while(i_m != conflicting.end()) {
        if(shouldAbort(pid, raddr, *i_m)) {
            markTransAborted(*i_m, pid, utid, caddr, TM_ATYPE_DEFAULT);
            conflicting.erase(i_m++);
        } else {
            ++i_m;
        }
    }
}

TMRWStatus TMFirstWinsCoherence::myRead(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort writers once we try to read
    set<Pid_t> m;
    getWritersExcept(caddr, pid, m);

    abortOthers(pid, raddr, m);
    if(m.size() > 0) {
        // If there are any still alive, I need to self abort
        Pid_t aborter = *(m.begin());
        markTransAborted(pid, aborter, transStates[aborter].getUtid(), caddr, TM_ATYPE_DEFAULT);
    } else {
        readTrans(pid, tid, raddr, caddr);
    }
    return TMRW_SUCCESS;
}

TMRWStatus TMFirstWinsCoherence::myWrite(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Abort everyone once we try to write
    set<Pid_t> m;
    getReadersExcept(caddr, pid, m);
    getWritersExcept(caddr, pid, m);

    abortOthers(pid, raddr, m);
    if(m.size() > 0) {
        // If there are any still alive, I need to self abort
        Pid_t aborter = *(m.begin());
        markTransAborted(pid, aborter, transStates[aborter].getUtid(), caddr, TM_ATYPE_DEFAULT);
    } else {
        writeTrans(pid, tid, raddr, caddr);
    }
    return TMRW_SUCCESS;
}
TMRWStatus TMFirstWinsCoherence::nonTMread(Pid_t pid, VAddr raddr) {
    // If anyone refuses to abort, they'll refetch the line later

    return TMRW_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with older wins
/////////////////////////////////////////////////////////////////////////////////////////
TMOlderCoherence::TMOlderCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMFirstWinsCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with older wins Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}

bool TMOlderCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    return (transStates[other].getTimestamp() / 16) > (transStates[pid].getTimestamp() / 16);
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with older instance wins
/////////////////////////////////////////////////////////////////////////////////////////
TMOlderAllCoherence::TMOlderAllCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMFirstWinsCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with older wins Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}

bool TMOlderAllCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    return startedAt[other] > startedAt[pid];
}

TMBCStatus TMOlderAllCoherence::myBegin(Pid_t pid, InstDesc* inst) {
    if(startedAt[pid] == 0) {
        startedAt[pid] = globalClock;
    }
    beginTrans(pid, inst);
    return TMBC_SUCCESS;
}
TMBCStatus TMOlderAllCoherence::myCommit(Pid_t pid, int tid) {
    startedAt[pid] = 0;
    commitTrans(pid);
    return TMBC_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with more writes wins
/////////////////////////////////////////////////////////////////////////////////////////
TMMoreCoherence::TMMoreCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMFirstWinsCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with more reads wins Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}

bool TMMoreCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    return linesRead[other].size() <= linesRead[pid].size();
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with more writes wins
/////////////////////////////////////////////////////////////////////////////////////////
TMLog2MoreCoherence::TMLog2MoreCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMFirstWinsCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with log2(more reads) wins Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}

bool TMLog2MoreCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    uint32_t log2Num = log2(linesRead[pid].size());
    uint32_t log2OtherNum = log2(linesRead[other].size());
    return log2OtherNum <= log2Num;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with more writes wins
/////////////////////////////////////////////////////////////////////////////////////////
TMCappedMoreCoherence::TMCappedMoreCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMFirstWinsCoherence(nProcs, lineSize, lines, argType), m_cap(64) {
	cout<<"[TM] Lazy/Eager with more reads capped wins Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}

bool TMCappedMoreCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    uint32_t cappedMyNum = linesRead[pid].size();
    uint32_t cappedOtherNum = linesRead[other].size();
    if(cappedMyNum > m_cap) {
        cappedMyNum = m_cap;
    }
    if(cappedOtherNum > m_cap) {
        cappedOtherNum = m_cap;
    }
    return cappedOtherNum <= cappedMyNum;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with more aborts wins
/////////////////////////////////////////////////////////////////////////////////////////
TMNumAbortsCoherence::TMNumAbortsCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMFirstWinsCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with more aborts wins Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}

bool TMNumAbortsCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    return numAbortsSeen[other] < numAbortsSeen[pid];
}
TMBCStatus TMNumAbortsCoherence::myBegin(Pid_t pid, InstDesc *inst) {
    VAddr currentIAddr = inst->getSescInst()->getAddr();
    if(lastBegin[pid] != currentIAddr) {
        lastBegin[pid] = currentIAddr;
        numAbortsSeen[pid] = 0;
    }

    beginTrans(pid, inst);
    return TMBC_SUCCESS;
}
TMBCStatus TMNumAbortsCoherence::myCommit(Pid_t pid, int tid) {
    numAbortsSeen[pid] = 0;
    lastBegin[pid] = 0;
    commitTrans(pid);
    return TMBC_SUCCESS;
}
void TMNumAbortsCoherence::myCompleteAbort(Pid_t pid) {
    numAbortsSeen[pid]++;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with first wins with nacked transactions resume after notify
/////////////////////////////////////////////////////////////////////////////////////////
TMFirstNotifyCoherence::TMFirstNotifyCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMCoherence(nProcs, lineSize, lines, argType),
        usefulNacks("tm:usefulNacks"), futileNacks("tm:futileNacks") {
	cout<<"[TM] Lazy/Eager with first wins with nacked transactions resume after notify Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
	nackStallBaseCycles     = SescConf->getInt("TransactionalMemory","nackStallBaseCycles");
	nackStallCap            = SescConf->getInt("TransactionalMemory","nackStallCap");
	maxNacks                = SescConf->getInt("TransactionalMemory","maxNacks");
}

bool TMFirstNotifyCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    return transStates[other].getState() == TM_NACKED;
}
void TMFirstNotifyCoherence::abortOthers(Pid_t pid, VAddr raddr, set<Pid_t>& conflicting) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Collect transactions that are to be aborted and remove from conflicting
    set<Pid_t> toAbort;
    set<Pid_t>::iterator i_m = conflicting.begin();
    while(i_m != conflicting.end()) {
        if(shouldAbort(pid, raddr, *i_m)) {
            toAbort.insert(*i_m);
            conflicting.erase(i_m++);
        } else {
            ++i_m;
        }
    }

    // Abort them
    for(i_m = toAbort.begin(); i_m != toAbort.end(); ++i_m) {
        Pid_t aborted = *i_m;
        if(transStates[aborted].getState() == TM_NACKED) {
            if(nackedBy.find(aborted) == nackedBy.end()) { fail("Who was I nacked by...?\n"); }

            releaseNacker(aborted);
        }
        markTransAborted(aborted, pid, utid, caddr, TM_ATYPE_DEFAULT);
    }
}
void TMFirstNotifyCoherence::getNacked(Pid_t pid, Pid_t nacker) {
    nackTrans(pid);
    if(nackedBy.find(nacker) != nackedBy.end()) { fail("Duplicate nack\n"); }

    nacking[nacker].insert(pid);
    nackedBy[pid] = nacker;
}
void TMFirstNotifyCoherence::releaseNacker(Pid_t pid) {
    Pid_t nacker = nackedBy.at(pid);
    nackedBy.erase(pid);
    nacking[nacker].erase(pid);
}

TMRWStatus TMFirstNotifyCoherence::myRead(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();
	if(transStates[pid].getState() == TM_NACKED) {
        return TMRW_NACKED;
	}

    // Abort writers once we try to read
    set<Pid_t> m;
    getWritersExcept(caddr, pid, m);

    abortOthers(pid, raddr, m);

    if(m.size() > 0) {
        // If anyone refuses to abort, I need to nack instead
        getNacked(pid, *(m.begin()));
        return TMRW_NACKED;
    } else {
        readTrans(pid, tid, raddr, caddr);
        return TMRW_SUCCESS;
    }
}

TMRWStatus TMFirstNotifyCoherence::myWrite(Pid_t pid, int tid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();
	if(transStates[pid].getState() == TM_NACKED) {
        return TMRW_NACKED;
	}

    // Abort everyone once we try to write
    set<Pid_t> m;
    getReadersExcept(caddr, pid, m);
    getWritersExcept(caddr, pid, m);

    abortOthers(pid, raddr, m);

    if(m.size() > 0) {
        // If anyone refuses to abort, I need to nack instead
        getNacked(pid, *(m.begin()));
        return TMRW_NACKED;
    } else {
        writeTrans(pid, tid, raddr, caddr);
        return TMRW_SUCCESS;
    }
}

TMBCStatus TMFirstNotifyCoherence::myCommit(Pid_t pid, int tid) {
    set<Pid_t>::iterator i_nacked;
    for(i_nacked = nacking[pid].begin(); i_nacked != nacking[pid].end(); ++i_nacked) {
        Pid_t nacked = *i_nacked;
        if(nackedBy.find(nacked) == nackedBy.end()) { fail("NackedBy resume mismatch?\n"); }
        if(nackedBy.at(nacked) != pid) { fail("NackedBy mismatch pid\n"); }

        // Note: The supposedly nacked transaction might not be nacked anymore,
        // because of nonTM aborts
        if(transStates.at(nacked).getState() == TM_NACKED) {
            transStates[nacked].resumeAfterNack();
        }
        nackedBy.erase(nacked);
    }
    nacking[pid].clear();
    commitTrans(pid);
    return TMBC_SUCCESS;
}

void TMFirstNotifyCoherence::myCompleteAbort(Pid_t pid) {
    if(nackedBy.find(pid) != nackedBy.end()) {
        // I can be aborted while being nacked, so let my nacker know it no longer
        // needs to abort me
        releaseNacker(pid);
    }
}


/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with first wins with nacked transactions retrying
/////////////////////////////////////////////////////////////////////////////////////////
TMFirstRetryCoherence::TMFirstRetryCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMCoherence(nProcs, lineSize, lines, argType), maxNacks(10),
        usefulNacks("tm:usefulNacks"), futileNacks("tm:futileNacks"), timedOutNacks("tm:timedOutNacks"),
        nackRefetches("tm:nackRefetches") {
	cout<<"[TM] Lazy/Eager with first wins with nacked transactions retrying Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
	nackStallBaseCycles     = SescConf->getInt("TransactionalMemory","nackStallBaseCycles");
	nackStallCap            = SescConf->getInt("TransactionalMemory","nackStallCap");
	maxNacks                = SescConf->getInt("TransactionalMemory","maxNacks");
}

void TMFirstRetryCoherence::getNacked(Pid_t pid, set<Pid_t>& nackers) {
    nackTrans(pid);
    numNacked[pid]++;

    Pid_t n = *(nackers.begin());
    nackedBy[pid] = n;
    nackerUtid[pid] = transStates[n].getUtid();

    numRefetched[pid]++;
}
Pid_t TMFirstRetryCoherence::removeNack(Pid_t pid) {
    Pid_t by = INVALID_PID;
    if(nackedBy.find(pid) != nackedBy.end()) {
        by = nackedBy.at(pid);
    }
    nackedBy.erase(pid);
    nackerUtid.erase(pid);

    return by;
}
bool TMFirstRetryCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    return false;
}
void TMFirstRetryCoherence::abortOthers(Pid_t pid, VAddr raddr, set<Pid_t>& conflicting) {
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();

    // Collect transactions that are in NACKED state and remove from m
    set<Pid_t>::iterator i_m = conflicting.begin();
    while(i_m != conflicting.end()) {
        if(checkNacked(pid)) {
            futileNacks.inc();
            removeNack(*i_m);
            markTransAborted(*i_m, pid, utid, caddr, TM_ATYPE_CIRCULAR);

            conflicting.erase(i_m++);
        } else if(shouldAbort(pid, raddr, *i_m)) {
            markTransAborted(*i_m, pid, utid, caddr, TM_ATYPE_DEFAULT);

            conflicting.erase(i_m++);
        } else {
            ++i_m;
        }
    }
}
void TMFirstRetryCoherence::selfAbort(Pid_t pid, VAddr caddr) {
    Pid_t aborter = nackedBy.at(pid);
    uint64_t aborterUtid = nackerUtid.at(pid);
    removeNack(pid);

    markTransAborted(pid, aborter, aborterUtid, caddr, TM_ATYPE_NACKOVERFLOW);
}
void TMFirstRetryCoherence::selfResume(Pid_t pid) {
    transStates[pid].resumeAfterNack();
    removeNack(pid);
}

TMRWStatus TMFirstRetryCoherence::myRead(Pid_t pid, int tid, VAddr raddr) {
    bool prevNacked = false;
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();
	if(transStates[pid].getState() == TM_NACKED && numNacked[pid] > maxNacks) {
        timedOutNacks.inc();
        selfAbort(pid, caddr);
        return TMRW_ABORT;
    }
	if(transStates[pid].getState() == TM_NACKED) {
        prevNacked = true;
        selfResume(pid);
	}

    // Abort writers once we try to read
    set<Pid_t> m;
    getWritersExcept(caddr, pid, m);

    abortOthers(pid, raddr, m);

    if(m.size() > 0) {
        // If anyone refuses to abort, I need to nack instead
        getNacked(pid, m);
        return TMRW_NACKED;
    } else {
        if(prevNacked) {
            usefulNacks.inc();
        }
        readTrans(pid, tid, raddr, caddr);
        return TMRW_SUCCESS;
    }
}

TMRWStatus TMFirstRetryCoherence::myWrite(Pid_t pid, int tid, VAddr raddr) {
    bool prevNacked = false;
	VAddr caddr = addrToCacheLine(raddr);
    uint64_t utid = transStates[pid].getUtid();
	if(transStates[pid].getState() == TM_NACKED && numNacked[pid] > maxNacks) {
        timedOutNacks.inc();
        selfAbort(pid, caddr);
        return TMRW_ABORT;
    }
	if(transStates[pid].getState() == TM_NACKED) {
        prevNacked = true;
        selfResume(pid);
	}

    // Abort everyone once we try to write
    set<Pid_t> m;
    getReadersExcept(caddr, pid, m);
    getWritersExcept(caddr, pid, m);

    abortOthers(pid, raddr, m);

    if(m.size() > 0) {
        // If anyone refuses to abort, I need to nack instead
        getNacked(pid, m);
        return TMRW_NACKED;
    } else {
        if(prevNacked) {
            usefulNacks.inc();
        }
        writeTrans(pid, tid, raddr, caddr);
        return TMRW_SUCCESS;
    }
}

void TMFirstRetryCoherence::nackOthers(Pid_t pid, set<Pid_t>& conflicting) {
    uint64_t utid = transStates[pid].getUtid();

    // Collect transactions that are in NACKED state and remove from m
    set<Pid_t> toAbort;
    set<Pid_t>::iterator i_m;
    for(i_m = conflicting.begin(); i_m != conflicting.end(); ++i_m) {
        nackTrans(*i_m);
        numNacked[*i_m]++;

        nackedBy[*i_m] = pid;
        nackerUtid[*i_m] = utid;
        numRefetched[*i_m]++;
    }
}

TMRWStatus TMFirstRetryCoherence::nonTMread(Pid_t pid, VAddr raddr) {
	VAddr caddr = addrToCacheLine(raddr);

    I(!hadRead(caddr, pid));
    I(!hadWrote(caddr, pid));

    // Abort writers once we try to read
    set<Pid_t> conflicting;
    getWritersExcept(caddr, pid, conflicting);

    abortOthers(pid, raddr, conflicting);

    if(conflicting.size() > 0) {
        // If anyone refuses to abort, they'll refetch the line later
    }

    return TMRW_SUCCESS;
}

TMBCStatus TMFirstRetryCoherence::myBegin(Pid_t pid, InstDesc* inst) {
    numNacked[pid] = 0;
    numRefetched[pid] = 0;

    removeNack(pid);
    beginTrans(pid, inst);
    return TMBC_SUCCESS;
}
TMBCStatus TMFirstRetryCoherence::myCommit(Pid_t pid, int tid) {
    nackRefetches.sample(numRefetched[pid]);
    numRefetched[pid] = 0;
    commitTrans(pid);
    return TMBC_SUCCESS;
}

void TMFirstRetryCoherence::myCompleteAbort(Pid_t pid) {
    nackRefetches.sample(numRefetched[pid]);
    numRefetched[pid] = 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with more reads wins, resume after notify
/////////////////////////////////////////////////////////////////////////////////////////
TMMoreNotifyCoherence::TMMoreNotifyCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMFirstNotifyCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with more reads wins, resume after notify Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}
bool TMMoreNotifyCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    size_t myNumReads = linesRead[pid].size();
    return transStates[other].getState() == TM_NACKED ||
        (transStates[other].getState() == TM_RUNNING && linesRead[other].size() < myNumReads);
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with more reads wins, with nacked transactions retrying
/////////////////////////////////////////////////////////////////////////////////////////
TMMoreRetryCoherence::TMMoreRetryCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMFirstRetryCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with more reads wins with nacked transactions retrying Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}
bool TMMoreRetryCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    size_t myNumReads = linesRead[pid].size();
    return linesRead[other].size() < myNumReads;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with more reads wins, with nacked transactions retrying
/////////////////////////////////////////////////////////////////////////////////////////
TMLog2MoreRetryCoherence::TMLog2MoreRetryCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMFirstRetryCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with log2(more reads) wins with nacked transactions retrying Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}
bool TMLog2MoreRetryCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    uint32_t log2Num = log2(linesRead[pid].size());
    uint32_t log2OtherNum = log2(linesRead[other].size());
    return log2OtherNum < log2Num;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with more reads wins, with nacked transactions retrying
/////////////////////////////////////////////////////////////////////////////////////////
TMCappedMoreRetryCoherence::TMCappedMoreRetryCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMFirstRetryCoherence(nProcs, lineSize, lines, argType), m_cap(64) {
	cout<<"[TM] Lazy/Eager with capped more reads wins with nacked transactions retrying Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}
bool TMCappedMoreRetryCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    uint32_t cappedMyNum = linesRead[pid].size();
    uint32_t cappedOtherNum = linesRead[other].size();
    if(cappedMyNum > m_cap) {
        cappedMyNum = m_cap;
    }
    if(cappedOtherNum > m_cap) {
        cappedOtherNum = m_cap;
    }
    return cappedOtherNum < cappedMyNum;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with more reads wins, with nacked transactions retrying
/////////////////////////////////////////////////////////////////////////////////////////
TMOlderRetryCoherence::TMOlderRetryCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMFirstRetryCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with older all wins with nacked transactions retrying Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}
bool TMOlderRetryCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    size_t myTimestamp = transStates[pid].getTimestamp();
    return transStates[other].getState() == TM_NACKED ||
        (transStates[other].getState() == TM_RUNNING && transStates[other].getTimestamp() > myTimestamp);
}

/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with more reads wins, with nacked transactions retrying
/////////////////////////////////////////////////////////////////////////////////////////
TMOlderAllRetryCoherence::TMOlderAllRetryCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMFirstRetryCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with older wins with nacked transactions retrying Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}
bool TMOlderAllRetryCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    size_t myTimestamp = startedAt[pid];
    return startedAt[other] > myTimestamp;
}
TMBCStatus TMOlderAllRetryCoherence::myBegin(Pid_t pid, InstDesc* inst) {
    if(startedAt[pid] == 0) {
        startedAt[pid] = globalClock;
    }
    beginTrans(pid, inst);
    return TMBC_SUCCESS;
}
TMBCStatus TMOlderAllRetryCoherence::myCommit(Pid_t pid, int tid) {
    startedAt[pid] = 0;
    commitTrans(pid);
    return TMBC_SUCCESS;
}


/////////////////////////////////////////////////////////////////////////////////////////
// Lazy-eager coherence with more aborts wins, with nacked transactions retrying
/////////////////////////////////////////////////////////////////////////////////////////
TMNumAbortsRetryCoherence::TMNumAbortsRetryCoherence(int32_t nProcs, int lineSize, int lines, int argType):
        TMFirstRetryCoherence(nProcs, lineSize, lines, argType) {
	cout<<"[TM] Lazy/Eager with more aborts wins Transactional Memory System" << endl;

	abortBaseStallCycles    = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
	commitBaseStallCycles   = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
}

bool TMNumAbortsRetryCoherence::shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) {
    return numAbortsSeen[other] < numAbortsSeen[pid];
}
TMBCStatus TMNumAbortsRetryCoherence::myBegin(Pid_t pid, InstDesc *inst) {
    VAddr currentIAddr = inst->getSescInst()->getAddr();
    if(lastBegin[pid] != currentIAddr) {
        lastBegin[pid] = currentIAddr;
        numAbortsSeen[pid] = 0;
    }

    beginTrans(pid, inst);
    return TMBC_SUCCESS;
}
TMBCStatus TMNumAbortsRetryCoherence::myCommit(Pid_t pid, int tid) {
    numAbortsSeen[pid] = 0;
    lastBegin[pid] = 0;
    commitTrans(pid);
    return TMBC_SUCCESS;
}
void TMNumAbortsRetryCoherence::myCompleteAbort(Pid_t pid) {
    numAbortsSeen[pid]++;
}

