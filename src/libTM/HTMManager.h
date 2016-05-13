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
    static HTMManager *create(int32_t nCores);

    // Entry point functions for TM operations
    TMRWStatus read(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    TMRWStatus write(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    TMBCStatus abort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    TMBCStatus commit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    TMBCStatus begin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);

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

    virtual uint32_t getNackRetryStallCycles() const { return 0; }

protected:
    HTMManager(const char* tmStyle, int procs, int line);

    // Common functionality that all HTMManager styles would use
    void beginTrans(Pid_t pid, InstDesc* inst);
    void commitTrans(Pid_t pid);
    void abortTrans(Pid_t pid);
    void completeAbortTrans(Pid_t pid);
    void readTrans(Pid_t pid, VAddr raddr, VAddr caddr);
    void writeTrans(Pid_t pid, VAddr raddr, VAddr caddr);
    void removeTrans(Pid_t pid);
    void markTransAborted(Pid_t victimPid, Pid_t aborterPid, VAddr caddr, TMAbortType_e abortType);
    void markTransAborted(std::set<Pid_t>& aborted, Pid_t aborterPid, VAddr caddr, TMAbortType_e abortType);

    // Interface for child classes to override and actually implement the TM OP
    virtual TMRWStatus TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) = 0;
    virtual TMRWStatus TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) = 0;
    virtual void       nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) = 0;
    virtual void       nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) = 0;
    virtual TMBCStatus myAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myBegin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual void       myCompleteAbort(Pid_t pid);
    virtual void       removeTransaction(Pid_t pid);

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
};

extern HTMManager *htmManager;
typedef std::set<Pid_t> PidSet;

#endif
