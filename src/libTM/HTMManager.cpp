#include <cmath>
#include <algorithm>
#include <iostream>
#include "nanassert.h"
#include "libemul/EmulInit.h"
#include "SescConf.h"
#include "libll/ThreadContext.h"
#include "libll/Instruction.h"
#include "HTMManager.h"
#include "TSXManager.h"
#include "IdealTSXManager.h"
#include "LogTMManager.h"
#include "FasTMManager.h"
#include "PleaseTMManager.h"

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
    } else if(method == "PTM-RequesterLoses") {
        newCohManager = new PTMRequesterLoses("PleaseTM Requester Loses", nCores, lineSize);
    } else if(method == "PTM-MoreReadsWins") {
        newCohManager = new PTMMoreReadsWins("PleaseTM More Reads Wins", nCores, lineSize);
    } else if(method == "PTM-OlderWins") {
        newCohManager = new PTMOlderWins("PleaseTM Older Wins", nCores, lineSize);
    } else if(method == "PTM-OlderAllWins") {
        newCohManager = new PTMOlderAllWins("PleaseTM Older All Wins", nCores, lineSize);
    } else if(method == "PTM-MoreAbortsWins") {
        newCohManager = new PTMMoreAbortsWins("PleaseTM MoreAborts Wins", nCores, lineSize);
    } else if(method == "PTM-Log2MoreReadsWins") {
        newCohManager = new PTMLog2MoreCoherence("PleaseTM MoreReads Wins (log2)", nCores, lineSize);
    } else if(method == "PTM-CappedMoreReadsWins") {
        newCohManager = new PTMCappedMoreCoherence("PleaseTM MoreReads Wins (capped)", nCores, lineSize);
    } else if(method == "IdealLogTM") {
        newCohManager = new IdealLogTM("Ideal LogTM", nCores, lineSize);
    } else if(method == "FasTM-Abort-Reads") {
        newCohManager = new FasTMAbortMoreReadsWins("FasTM-Abort (More reads wins)", nCores, lineSize);
    } else if(method == "FasTM-Abort") {
        newCohManager = new FasTMAbortOlderWins("FasTM-Abort (Older wins)", nCores, lineSize);
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
        userAbortArgs("tm:userAbortArgs"),
        fallbackArgHist("tm:fallbackArgHist"),
        numFutileAborts("tm:numFutileAborts"),
        numAbortsBeforeCommit("tm:numAbortsBeforeCommit") {

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
    }
    rwSetManager.initialize(nThreads);
}
///
// Entry point for TM begin operation. Check for nesting and then call the real begin.
TMBCStatus HTMManager::begin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    TMBCStatus status = myBegin(inst, context, p_opStatus);

    if(status == TMBC_SUCCESS) {
        // Do the begin
        utids.at(pid) = HTMManager::nextUtid;
        HTMManager::nextUtid += 1;

        tmStates[pid].begin();
        abortStates.at(pid).clear();
        abortsCaused[pid] = 0;
    }
    return status;
}

///
// Entry point for TM begin operation. Check for nesting and then call the real begin.
TMBCStatus HTMManager::commit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    TMBCStatus status = TMBC_INVALID;

	if(getTMState(pid) == TMStateEngine::TM_MARKABORT) {
        p_opStatus->tmCommitSubtype=TM_COMMIT_ABORTED;
		status = TMBC_ABORT;
	} else {
		status = myCommit(inst, context, p_opStatus);
	}

    if(status == TMBC_SUCCESS) {
        // Do the commit
        numCommits.inc();
        numAbortsBeforeCommit.sample(abortsSoFar[pid]);

        abortsSoFar[pid] = 0;

        tmStates.at(pid).clear();
        utids.at(pid) = INVALID_UTID;
        rwSetManager.clear(pid);
    }
    return status;
}
///
// Entry point for TM read operation. Checks transaction state and then calls the real read.
TMRWStatus HTMManager::read(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    TMRWStatus status = TMRW_INVALID;

	if(getTMState(pid) == TMStateEngine::TM_MARKABORT) {
		status = TMRW_ABORT;
	} else if(getTMState(pid) == TMStateEngine::TM_INVALID) {
        nonTMRead(inst, context, raddr, p_opStatus);
        status = TMRW_NONTM;
	} else {
        status = TMRead(inst, context, raddr, p_opStatus);
    }

    if(status == TMRW_SUCCESS) {
        if(getTMState(pid) != TMStateEngine::TM_RUNNING) {
            fail("%d in invalid state to do tm.load: %d", pid, getTMState(pid));
        }
        rwSetManager.read(pid, caddr);
    }
    return status;
}

///
// Entry point for TM write operation. Checks transaction state and then calls the real write.
TMRWStatus HTMManager::write(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
	VAddr caddr = addrToCacheLine(raddr);
    TMRWStatus status = TMRW_INVALID;

	if(getTMState(pid) == TMStateEngine::TM_MARKABORT) {
		status = TMRW_ABORT;
	} else if(getTMState(pid) == TMStateEngine::TM_INVALID) {
        nonTMWrite(inst, context, raddr, p_opStatus);
        status = TMRW_NONTM;
	} else {
        status = TMWrite(inst, context, raddr, p_opStatus);
    }

    if(status == TMRW_SUCCESS) {
        if(getTMState(pid) != TMStateEngine::TM_RUNNING) {
            fail("%d in invalid state to do tm.store: %d", pid, getTMState(pid));
        }
        rwSetManager.write(pid, caddr);
    }
    return status;
}

void HTMManager::startAborting(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    if(getTMState(pid) != TMStateEngine::TM_MARKABORT) {
        fail("%d should be marked abort before starting abort: %d", pid, getTMState(pid));
    }

    myStartAborting(inst, context, p_opStatus);

    abortStates.at(pid).setAbortIAddr(context->getIAddr());
    tmStates[pid].startAborting();
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
    if(getTMState(pid) != TMStateEngine::TM_ABORTING) {
        fail("%d should be doing aborting before completing it: %d", pid, getTMState(pid));
    }
    const TMAbortState& abortState = abortStates.at(pid);

    myCompleteAbort(pid);

    // Update Statistics
    numAborts.inc();
    abortTypes.sample(abortState.getAbortType());

    abortsSoFar[pid]++;
    if(abortsCaused[pid] > 0) {
        numFutileAborts.inc();
        abortsCaused[pid] = 0;
    }

    p_opStatus->tmBeginSubtype=TM_COMPLETE_ABORT;
    p_opStatus->tmAbortType = abortStates.at(pid).getAbortType();
    tmStates.at(pid).clear();
    utids.at(pid) = INVALID_UTID;
    rwSetManager.clear(pid);

    return TMBC_SUCCESS;
}
///
// Function that tells the TM engine that a fallback path for this transaction has been used,
// so reset any statistics. Used for statistics that run across multiple retires.
void HTMManager::beginFallback(Pid_t pid, uint32_t arg) {
    fallbackArg[pid] = arg;
    fallbackArgHist.sample(arg);
    abortsSoFar[pid] = 0;
}
void HTMManager::completeFallback(Pid_t pid) {
    fallbackArg.erase(pid);
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

///
// A basic type of TM begin that will be used if child does not override
TMBCStatus HTMManager::myBegin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    p_opStatus->tmBeginSubtype=TM_BEGIN_REGULAR;

    return TMBC_SUCCESS;
}

///
// A basic type of TM abort if child does not override
void HTMManager::myStartAborting(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
}

///
// A basic type of TM commit if child does not override
TMBCStatus HTMManager::myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();

    p_opStatus->tmLat           = 4 + rwSetManager.getNumWrites(pid);
    p_opStatus->tmCommitSubtype =TM_COMMIT_REGULAR;

    return TMBC_SUCCESS;
}

///
// A basic type of TM complete abort if child does not override
void HTMManager::myCompleteAbort(Pid_t pid) {
}
