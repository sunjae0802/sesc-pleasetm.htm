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
enum TMRWStatus { TMRW_SUCCESS, TMRW_NACKED, TMRW_ABORT };

class TMCoherence {
public:
    virtual ~TMCoherence() {}
    static TMCoherence *create(int32_t nProcs);

    // Entry point functions for TM operations
    TMRWStatus read(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    TMRWStatus write(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    TMBCStatus abort(Pid_t pid, int tid, TMAbortType_e abortType);
    TMBCStatus commit(Pid_t pid, int tid);
    TMBCStatus begin(Pid_t pid, InstDesc *inst);

    TMBCStatus completeAbort(Pid_t pid);
    TMRWStatus nonTMread(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    TMRWStatus nonTMwrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    void completeFallback(Pid_t pid);

    // Query functions
    const TransState& getTransState(Pid_t pid) { return transStates.at(pid); }
    int getReturnArgType()          const { return returnArgType; }
    uint64_t getUtid(Pid_t pid)     const { return transStates.at(pid).getUtid(); }
    size_t getDepth(Pid_t pid)      const { return transStates.at(pid).getDepth(); }
    bool checkNacked(Pid_t pid)    const { return transStates.at(pid).getState() == TM_NACKED; }
    bool checkAborting(Pid_t pid)   const { return transStates.at(pid).getState() == TM_ABORTING; }
    bool markedForAbort(Pid_t pid)  const { return transStates.at(pid).getState() == TM_MARKABORT; }

    uint32_t getNackStallCycles(size_t numNacks)   const {
        uint32_t stallCycles = nackStallBaseCycles * numNacks;
        if(stallCycles > nackStallCap) {
            stallCycles = nackStallCap;
        }
        return stallCycles;
    }
    size_t getNumReads(Pid_t pid) const {
        return linesRead.at(pid).size();
    }
    size_t getNumWrites(Pid_t pid) const {
        return linesWritten.at(pid).size();
    }

protected:
    TMCoherence(const char* tmStyle, int32_t nProcs, int lineSize, int lines, int returnArgType);
    VAddr addrToCacheLine(VAddr raddr) {
        while(raddr % cacheLineSize != 0) {
            raddr = raddr-1;
        }
        return raddr;
    }

    // Common functionality that all TMCoherence styles would use
    void beginTrans(Pid_t pid, InstDesc* inst);
    void commitTrans(Pid_t pid);
    void abortTrans(Pid_t pid);
    void completeAbortTrans(Pid_t pid);
    void nackTrans(Pid_t pid);
    void readTrans(Pid_t pid, int tid, VAddr raddr, VAddr caddr);
    void writeTrans(Pid_t pid, int tid, VAddr raddr, VAddr caddr);
    void markTransAborted(Pid_t victimPid, Pid_t aborterPid, VAddr caddr, TMAbortType_e abortType);
    void markTransAborted(std::set<Pid_t>& aborted, Pid_t aborterPid, VAddr caddr, TMAbortType_e abortType);
    void invalidateSharers(InstDesc* inst, ThreadContext* context, VAddr raddr);

    // Interface for child classes to override and actually implement the TM OP
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr) = 0;
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr) = 0;
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual void       myCompleteAbort(Pid_t pid);

    // Functions for managing reads/writes
    void removeTransaction(Pid_t pid);
    void removeFromList(std::list<Pid_t>& list, Pid_t pid);
    bool hadWrote(VAddr caddr, Pid_t pid);
    bool hadRead(VAddr caddr, Pid_t pid);
    void getWritersExcept(VAddr caddr, Pid_t pid, std::set<Pid_t>& w);
    void getReadersExcept(VAddr caddr, Pid_t pid, std::set<Pid_t>& r);

    // Common member variables
    static uint64_t nextUtid;
    int             nProcs;
    int             cacheLineSize;
    size_t          numLines;
    int             returnArgType;
    uint32_t        nackStallBaseCycles;
    uint32_t        nackStallCap;

    std::vector<struct TransState>  transStates;
    std::vector<PrivateCache*>      caches;

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
    std::map<VAddr, std::list<Pid_t> >  writers;
    std::map<VAddr, std::list<Pid_t> >  readers;
};

class TMEECoherence: public TMCoherence {
public:
    TMEECoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMEECoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
private:
    std::vector<bool> cycleFlags;
    int     abortVarStallCycles;
    int     commitVarStallCycles;
};

class TMLLCoherence: public TMCoherence {
public:
    TMLLCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLLCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
private:
    Pid_t   currentCommitter; // PID of the currently committing processor
    int     abortVarStallCycles;
    int     commitVarStallCycles;
};

class TMLECoherence: public TMCoherence {
public:
    TMLECoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLECoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
private:
};

extern TMCoherence *tmCohManager;

#endif
