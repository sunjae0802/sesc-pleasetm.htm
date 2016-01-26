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

enum TMBCStatus { TMBC_INVALID, TMBC_SUCCESS, TMBC_NACK, TMBC_ABORT };
enum TMRWStatus { TMRW_INVALID, TMRW_NONTM, TMRW_SUCCESS, TMRW_NACKED, TMRW_ABORT };

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
    virtual void beginFallback(Pid_t pid) {}
    virtual void completeFallback(Pid_t pid) {}

    // Query functions
    VAddr addrToCacheLine(VAddr raddr) {
        while(raddr % lineSize != 0) {
            raddr = raddr-1;
        }
        return raddr;
    }
    const TransState& getTransState(Pid_t pid) const { return transStates.at(pid); }
    const TMAbortState& getAbortState(Pid_t pid) const { return abortStates.at(pid); }
    TMState_e getState(Pid_t pid)   const { return transStates.at(pid).getState(); }
    uint64_t getUtid(Pid_t pid)     const { return transStates.at(pid).getUtid(); }

    virtual uint32_t getNackRetryStallCycles() const { return 0; }
    size_t getNumReads(Pid_t pid)   const { return linesRead.at(pid).size(); }
    size_t getNumWrites(Pid_t pid)  const { return linesWritten.at(pid).size(); }

    bool hadWrote(Pid_t pid, VAddr caddr) const {
        return linesWritten.at(pid).find(caddr) != linesWritten.at(pid).end();
    }

    bool hadRead(Pid_t pid, VAddr caddr) const {
        return linesRead.at(pid).find(caddr) != linesRead.at(pid).end();
    }

    size_t numWriters(VAddr caddr) const {
        auto i_line = writers.find(caddr);
        if(i_line == writers.end()) {
            return 0;
        } else {
            return i_line->second.size();
        }
    }
    size_t numReaders(VAddr caddr) const {
        auto i_line = readers.find(caddr);
        if(i_line == readers.end()) {
            return 0;
        } else {
            return i_line->second.size();
        }
    }

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

    std::vector<struct TransState>  transStates;
    std::vector<TMAbortState>       abortStates;

    // Statistics
    GStatsCntr      numCommits;
    GStatsCntr      numAborts;
    GStatsHist      abortTypes;
    GStatsHist      userAbortArgs;

    std::map<Pid_t, std::set<VAddr> >   linesRead;
    std::map<Pid_t, std::set<VAddr> >   linesWritten;
    std::map<VAddr, std::set<Pid_t> >   writers;
    std::map<VAddr, std::set<Pid_t> >   readers;
};

class TSXManager: public HTMManager {
public:
    TSXManager(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~TSXManager();

    typedef CacheAssocTM    Cache;
    typedef TMLine          Line;
protected:
    virtual TMRWStatus TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual TMRWStatus TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual TMBCStatus myAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);

    // Helper functions
    Cache* getCache(Pid_t pid) { return caches.at(pid/nSMTWays); }
    void getPeers(Pid_t pid, std::set<Pid_t>& peers);
    void updateOverflow(Pid_t pid, VAddr newCaddr);
    Line* replaceLine(Pid_t pid, VAddr raddr);
    void cleanDirtyLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except);
    void invalidateLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except);
    void abortTMWriters(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except);
    void abortTMSharers(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except);

    // Configurable member variables
    int             totalSize;
    int             assoc;
    size_t          maxOverflowSize;

    // State member variables
    std::vector<Cache*>         caches;
    std::map<Pid_t, std::set<VAddr> >   overflow;
};

extern HTMManager *htmManager;

#endif
