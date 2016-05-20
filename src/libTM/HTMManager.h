#ifndef HTM_MANAGER_H
#define HTM_MANAGER_H

#include <map>
#include <set>
#include <list>
#include <vector>

#include "Snippets.h"
#include "GStats.h"
#include "libemul/InstDesc.h"
#include "TMState.h"
#include "RWSetManager.h"

// Forward defs instead of ThreadContext.h
class ThreadContext;
class InstContext;

class HTMManager {
public:
    virtual ~HTMManager() { }
    
    // Factory method
    static HTMManager *create(int32_t nCores);

    // Entry point functions for TM operations
    TMBCStatus begin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    TMBCStatus commit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    TMRWStatus read(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    TMRWStatus write(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);

    // Entry point for TM abort operations
    void startAborting(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    void markSyscallAbort(InstDesc* inst, const ThreadContext* context);
    void markUserAbort(InstDesc* inst, const ThreadContext* context, uint32_t abortArg);
    TMBCStatus completeAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);

    // Functions about the fallback path for statistics that run across multiple retries
    virtual void beginFallback(Pid_t pid, uint32_t arg);
    virtual void completeFallback(Pid_t pid);

    // Query functions
    VAddr addrToCacheLine(VAddr raddr) {
        while(raddr % lineSize != 0) {
            raddr = raddr-1;
        }
        return raddr;
    }
    const TMAbortState& getAbortState(Pid_t pid) const { return abortStates.at(pid); }
    TMStateEngine::State_e getTMState(Pid_t pid)   const { return tmStates.at(pid).getState(); }
    uint64_t getUtid(Pid_t pid)     const { return utids.at(pid); }

    virtual uint32_t getNackRetryStallCycles(ThreadContext* context) { return 0; }

protected:
    HTMManager(const char* tmStyle, int procs, int line);

    // Mark a transaction (or set of transactions) as aborted.
    void markTransAborted(Pid_t victimPid, Pid_t aborterPid, VAddr caddr, TMAbortType_e abortType);
    void markTransAborted(std::set<Pid_t>& aborted, Pid_t aborterPid, VAddr caddr, TMAbortType_e abortType);

    // Interface for child classes to override and actually implement the TM OP
    virtual TMBCStatus myBegin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual void myStartAborting(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual void       myCompleteAbort(Pid_t pid);

    virtual TMRWStatus TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) = 0;
    virtual TMRWStatus TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) = 0;
    virtual void       nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) = 0;
    virtual void       nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) = 0;

    // Common member variables
    int             nCores;
    size_t          nSMTWays;
    size_t          nThreads;
    int             lineSize;

    RWSetManager    rwSetManager;
    std::vector<struct TMStateEngine> tmStates;
    std::vector<TMAbortState>       abortStates;
    // The unique identifier for each tnx instance
    std::vector<uint64_t>           utids;
    // Mono-increasing UTID
    static uint64_t nextUtid;
    std::map<Pid_t, uint32_t> fallbackArg;

    // Statistics
    GStatsCntr      numCommits;
    GStatsCntr      numAborts;
    GStatsHist      abortTypes;
    GStatsHist      userAbortArgs;
    GStatsHist      fallbackArgHist;
    GStatsCntr      numFutileAborts;
    GStatsHist      numAbortsBeforeCommit;

    std::map<Pid_t, size_t>   abortsSoFar;
    std::map<Pid_t, size_t>   abortsCaused;
};

extern HTMManager *htmManager;

#endif
