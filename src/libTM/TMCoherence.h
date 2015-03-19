#ifndef TM_COHERENCE
#define TM_COHERENCE

#include <map>
#include <set>
#include <list>
#include <vector>

#include "Snippets.h"
#include "GStats.h"
#include "libemul/InstDesc.h"
#include "TMState.h"
#include "PrivateCache.h"
#include "HWGate.h"

#define MAX_CPU_COUNT 512

typedef unsigned long long ID; 
typedef unsigned long long INSTCOUNT;

enum TMBCStatus { TMBC_INVALID, TMBC_SUCCESS, TMBC_NACK, TMBC_ABORT, TMBC_IGNORE };
enum TMRWStatus { TMRW_NONTM, TMRW_SUCCESS, TMRW_NACKED, TMRW_ABORT };

class TMCoherence {
public:
    virtual ~TMCoherence() { }
    static TMCoherence *create(int32_t nProcs);

    // Entry point functions for TM operations
    TMRWStatus read(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    TMRWStatus write(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    TMBCStatus abort(Pid_t pid, int tid, TMAbortType_e abortType);
    TMBCStatus commit(Pid_t pid, int tid);
    TMBCStatus begin(Pid_t pid, InstDesc *inst);

    TMBCStatus completeAbort(Pid_t pid);
    void completeFallback(Pid_t pid);

    // Query functions
    const TransState& getTransState(Pid_t pid) const { return transStates.at(pid); }
    TMState_e getState(Pid_t pid)   const { return transStates.at(pid).getState(); }
    uint64_t getUtid(Pid_t pid)     const { return transStates.at(pid).getUtid(); }
    size_t  getDepth(Pid_t pid)     const { return transStates.at(pid).getDepth(); }
    int     getReturnArgType()      const { return returnArgType; }

    uint32_t getNackStallCycles(size_t numNacks)   const {
        uint32_t stallCycles = nackStallBaseCycles * numNacks;
        if(stallCycles > nackStallCap) {
            stallCycles = nackStallCap;
        }
        return stallCycles;
    }
    size_t getNumReads(Pid_t pid)   const { return linesRead.at(pid).size(); }
    size_t getNumWrites(Pid_t pid)  const { return linesWritten.at(pid).size(); }

protected:
    TMCoherence(const char* tmStyle, int procs, int size, int a, int line, int argType);
    VAddr addrToCacheLine(VAddr raddr) {
        while(raddr % lineSize != 0) {
            raddr = raddr-1;
        }
        return raddr;
    }

    // Common functionality that all TMCoherence styles would use
    void beginTrans(Pid_t pid, InstDesc* inst);
    void commitTrans(Pid_t pid);
    void abortTrans(Pid_t pid);
    void completeAbortTrans(Pid_t pid);
    void nackTrans(Pid_t victimPid, Pid_t byPid);
    void resumeAllTrans(Pid_t pid);
    void readTrans(Pid_t pid, VAddr raddr, VAddr caddr);
    void writeTrans(Pid_t pid, VAddr raddr, VAddr caddr);
    void removeTrans(Pid_t pid);
    void markTransAborted(Pid_t victimPid, Pid_t aborterPid, VAddr caddr, TMAbortType_e abortType);
    void markTransAborted(std::set<Pid_t>& aborted, Pid_t aborterPid, VAddr caddr, TMAbortType_e abortType);

    // Interface for child classes to override and actually implement the TM OP
    virtual TMRWStatus TMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) = 0;
    virtual TMRWStatus TMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) = 0;
    virtual void       nonTMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) = 0;
    virtual void       nonTMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) = 0;
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual void       myCompleteAbort(Pid_t pid);
    virtual void       removeTransaction(Pid_t pid);

    // Common member variables
    static uint64_t nextUtid;
    int             nProcs;
    int             totalSize;
    int             assoc;
    int             lineSize;
    int             returnArgType;
    uint32_t        nackStallBaseCycles;
    uint32_t        nackStallCap;

    std::vector<struct TransState>  transStates;

    // Statistics
    GStatsCntr      numCommits;
    GStatsCntr      numAborts;
    GStatsHist      abortTypes;
    GStatsCntr      tmLoads;
    GStatsCntr      tmStores;
    GStatsCntr      tmLoadMisses;
    GStatsCntr      tmStoreMisses;
    GStatsCntr      numAbortsCausedBeforeAbort;
    GStatsCntr      numAbortsCausedBeforeCommit;
    GStatsHist      linesReadHist;
    GStatsHist      linesWrittenHist;

    std::map<VAddr, size_t>             numAbortsCaused;
    std::map<Pid_t, std::set<VAddr> >   linesRead;
    std::map<Pid_t, std::set<VAddr> >   linesWritten;
    std::map<Pid_t, std::set<Pid_t> >   nacking;
    std::map<Pid_t, Pid_t>              nackedBy;
};

class TMLECoherence: public TMCoherence {
public:
    TMLECoherence(int32_t nProcs, int size, int a, int line, int argType);
    virtual ~TMLECoherence();

    typedef CacheAssocTM    Cache;
    typedef TMLine          Line;
protected:
    virtual TMRWStatus TMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    virtual TMRWStatus TMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    virtual void       nonTMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    virtual void       nonTMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual void       myCompleteAbort(Pid_t pid);
    virtual void       removeTransaction(Pid_t pid);

    Cache* getCache(Pid_t pid) { return caches.at(pid); }

    void handleTMSetConflict(Pid_t pid, Line* line);
    void updateOverflow(Pid_t pid, VAddr newCaddr);

    Line* lookupLine(Pid_t pid, bool isInTM, VAddr raddr, MemOpStatus* p_opStatus);
    void invalidateSharers(Pid_t pid, VAddr raddr);
    void cleanWriters(Pid_t pid, VAddr raddr);

    std::vector<Cache*>         caches;
    std::map<Pid_t, std::set<VAddr> >   overflow;
    size_t                              maxOverflowSize;
};

extern TMCoherence *tmCohManager;

#endif
