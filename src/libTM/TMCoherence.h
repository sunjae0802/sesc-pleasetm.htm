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

class TMCoherence {
public:
    virtual ~TMCoherence() { }
    static TMCoherence *create(int32_t nProcs);

    // Entry point functions for TM operations
    TMRWStatus read(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    TMRWStatus write(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    TMBCStatus abort(InstDesc* inst, ThreadContext* context);
    TMBCStatus commit(InstDesc* inst, ThreadContext* context);
    TMBCStatus begin(InstDesc* inst, ThreadContext* context);

    void markAbort(InstDesc* inst, ThreadContext* context, TMAbortType_e abortType);
    TMBCStatus completeAbort(Pid_t pid);

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
    TMCoherence(const char* tmStyle, int procs, int line);

    // Common functionality that all TMCoherence styles would use
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
    virtual TMRWStatus TMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) = 0;
    virtual TMRWStatus TMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) = 0;
    virtual void       nonTMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) = 0;
    virtual void       nonTMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus) = 0;
    virtual TMBCStatus myAbort(Pid_t pid);
    virtual TMBCStatus myCommit(Pid_t pid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual void       myCompleteAbort(Pid_t pid);
    virtual void       removeTransaction(Pid_t pid);

    // Common member variables
    int             nProcs;
    int             lineSize;

    std::vector<struct TransState>  transStates;
    std::vector<TMAbortState>       abortStates;

    // Statistics
    GStatsCntr      numCommits;
    GStatsCntr      numAborts;
    GStatsHist      abortTypes;

    std::map<Pid_t, std::set<VAddr> >   linesRead;
    std::map<Pid_t, std::set<VAddr> >   linesWritten;
    std::map<VAddr, std::set<Pid_t> >   writers;
    std::map<VAddr, std::set<Pid_t> >   readers;
};

class TMIdealLECoherence: public TMCoherence {
public:
    TMIdealLECoherence(const char tmStyle[], int32_t nProcs, int32_t line);
    virtual ~TMIdealLECoherence();

    typedef CacheAssocTM    Cache;
    typedef TMLine          Line;
protected:
    virtual TMRWStatus TMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    virtual TMRWStatus TMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    virtual void       nonTMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    virtual void       nonTMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    virtual void       removeTransaction(Pid_t pid);

    // Helper functions
    Cache* getCache(Pid_t pid) { return caches.at(pid); }
    Line* replaceLine(Pid_t pid, VAddr raddr);
    void cleanDirtyLines(Pid_t pid, VAddr raddr);
    void invalidateLines(Pid_t pid, VAddr raddr);
    void abortTMWriters(Pid_t pid, VAddr caddr, TMAbortType_e abortType);
    void abortTMSharers(Pid_t pid, VAddr caddr, TMAbortType_e abortType);

    // Configurable member variables
    int             totalSize;
    int             assoc;

    // State member variables
    std::vector<Cache*>         caches;
};

class TMLECoherence: public TMCoherence {
public:
    TMLECoherence(const char tmStyle[], int32_t nProcs, int32_t line);
    virtual ~TMLECoherence();

    typedef CacheAssocTM    Cache;
    typedef TMLine          Line;
protected:
    virtual TMRWStatus TMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    virtual TMRWStatus TMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    virtual void       nonTMRead(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    virtual void       nonTMWrite(InstDesc* inst, ThreadContext* context, VAddr raddr, MemOpStatus* p_opStatus);
    virtual void       removeTransaction(Pid_t pid);
    Line* replaceLine(Pid_t pid, VAddr raddr);
    Line* replaceLineTM(Pid_t pid, VAddr raddr);

    // Helper functions for handling Cache lines
    Cache* getCache(Pid_t pid) { return caches.at(pid); }
    Line* lookupLine(Pid_t pid, bool isInTM, VAddr raddr, MemOpStatus* p_opStatus);
    void abortReplaced(Line* replaced, Pid_t byPid, VAddr byCaddr, TMAbortType_e abortType);
    void invalidateSharers(Pid_t pid, VAddr raddr, bool isTM);
    void cleanWriters(Pid_t pid, VAddr raddr, bool isTM);

    // Configurable member variables
    int             totalSize;
    int             assoc;
    size_t          maxOverflowSize;

    // Statistics
    GStatsCntr      getSMsg;
    GStatsCntr      getSAck;
    GStatsCntr      fwdGetSMsg;
    GStatsCntr      fwdGetSAck;
    GStatsCntr      getMMsg;
    GStatsCntr      getMAck;
    GStatsCntr      invMsg;
    GStatsCntr      invAck;

    // State member variables
    std::vector<Cache*>         caches;
    std::map<Pid_t, std::set<VAddr> >   overflow;
};

extern TMCoherence *tmCohManager;

#endif
