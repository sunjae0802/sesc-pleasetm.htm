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
    TMRWStatus read(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    TMRWStatus write(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    TMBCStatus abort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    TMBCStatus commit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    TMBCStatus begin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);

    void markAbort(InstDesc* inst, const ThreadContext* context, TMAbortType_e abortType);
    TMBCStatus completeAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);

    // Functions about the fallback path for statistics that run across multiple retries
    virtual void beginFallback(Pid_t pid);
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

    virtual uint32_t getNackRetryStallCycles(ThreadContext* context) { return 0; }
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
    int             nProcs;
    int             lineSize;

    std::vector<struct TransState>  transStates;
    std::vector<TMAbortState>       abortStates;

    // Statistics
    GStatsCntr      numCommits;
    GStatsCntr      numAborts;
    GStatsHist      abortTypes;
    GStatsCntr      numFutileAborts;
    GStatsHist      numAbortsBeforeCommit;
    std::map<Pid_t, size_t>   abortsSoFar;
    std::map<Pid_t, size_t>   abortsCaused;

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
    virtual TMRWStatus TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual TMRWStatus TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual TMBCStatus myAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);

    // Helper functions
    Cache* getCache(Pid_t pid) { return caches.at(pid); }
    Line* replaceLine(Pid_t pid, VAddr raddr);
    void cleanDirtyLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except);
    void invalidateLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except);
    void abortTMWriters(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except);
    void abortTMSharers(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except);

    // Configurable member variables
    int             totalSize;
    int             assoc;

    // Statistics
    GStatsCntr      getSMsg;
    GStatsCntr      fwdGetSMsg;
    GStatsCntr      getMMsg;
    GStatsCntr      invMsg;
    GStatsCntr      flushMsg;
    GStatsCntr      fwdGetSConflictMsg;
    GStatsCntr      invConflictMsg;

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
    virtual TMRWStatus TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual TMRWStatus TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual TMBCStatus myAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    Line* replaceLine(Pid_t pid, VAddr raddr);
    Line* replaceLineTM(Pid_t pid, VAddr raddr);

    // Helper functions for handling Cache lines
    Cache* getCache(Pid_t pid) { return caches.at(pid); }
    Line* lookupLine(Pid_t pid, bool isInTM, VAddr raddr, InstContext* p_opStatus);
    void abortReplaced(Line* replaced, Pid_t byPid, VAddr byCaddr, TMAbortType_e abortType);
    void invalidateSharers(Pid_t pid, VAddr raddr, bool isTM);
    void cleanWriters(Pid_t pid, VAddr raddr, bool isTM);

    // Configurable member variables
    int             totalSize;
    int             assoc;
    size_t          maxOverflowSize;

    // Statistics
    GStatsCntr      getSMsg;
    GStatsCntr      fwdGetSMsg;
    GStatsCntr      getMMsg;
    GStatsCntr      invMsg;
    GStatsCntr      flushMsg;
    GStatsCntr      fwdGetSConflictMsg;
    GStatsCntr      invConflictMsg;

    // State member variables
    std::vector<Cache*>         caches;
    std::map<Pid_t, std::set<VAddr> >   overflow;
};

class IdealLogTM: public TMCoherence {
public:
    IdealLogTM(const char tmStyle[], int32_t nProcs, int32_t line);
    virtual ~IdealLogTM();
    virtual uint32_t getNackRetryStallCycles(ThreadContext* context);

    typedef CacheAssocTM    Cache;
    typedef TMLine          Line;
protected:
    virtual TMRWStatus TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual TMRWStatus TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       removeTransaction(Pid_t pid);
    virtual TMBCStatus myAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myBegin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual void completeFallback(Pid_t pid);

    Cache* getCache(Pid_t pid) { return caches.at(pid); }
    TMRWStatus handleConflicts(Pid_t pid, VAddr caddr, std::set<Pid_t>& conflicting);
    virtual bool isHigherOrEqualPriority(Pid_t pid, Pid_t conflictPid);
    Time_t getStartTime(Pid_t pid)   const { return startTime.at(pid); }

    Line* replaceLine(Pid_t pid, VAddr raddr);
    void cleanDirtyLines(VAddr raddr, std::set<Cache*>& except);
    void invalidateLines(VAddr raddr, std::set<Cache*>& except);
    TMRWStatus abortTMWriters(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except);
    TMRWStatus abortTMSharers(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except);

    // Configurable member variables
    int             totalSize;
    int             assoc;
    uint32_t        nackBase;
    uint32_t        nackCap;

    // RNG-related data
    struct random_data randBuf;
    static const int   RBUF_SIZE = 32;
    char               rbuf[RBUF_SIZE];

    // Statistics
    GStatsCntr      getSMsg;
    GStatsCntr      fwdGetSMsg;
    GStatsCntr      getMMsg;
    GStatsCntr      invMsg;
    GStatsCntr      flushMsg;
    GStatsCntr      fwdGetSConflictMsg;
    GStatsCntr      invConflictMsg;
    GStatsCntr      nackMsg;

    // State member variables
    std::vector<Cache*>         caches;

    std::map<Pid_t, bool>               cycleFlags;
    std::map<Pid_t, Time_t>             startTime;
    std::map<Pid_t, size_t>             nackCount;
    std::map<Pid_t, Pid_t>              nackedBy;
};

class FasTMAbort: public TMCoherence {
public:
    FasTMAbort(const char tmStyle[], int32_t nProcs, int32_t line);
    virtual ~FasTMAbort();

    typedef CacheAssocTM    Cache;
    typedef TMLine          Line;
protected:
    virtual TMRWStatus TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual TMRWStatus TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual TMBCStatus myAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);

    Cache* getCache(Pid_t pid) { return caches.at(pid); }
    TMRWStatus handleConflicts(Pid_t pid, VAddr caddr, std::set<Pid_t>& conflicting);
    virtual bool isHigherOrEqualPriority(Pid_t pid, Pid_t conflictPid) = 0;

    Line* replaceLine(Pid_t pid, VAddr raddr);
    void cleanDirtyLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except);
    void invalidateLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except);
    TMRWStatus abortTMWriters(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except);
    TMRWStatus abortTMSharers(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except);

    // Configurable member variables
    int             totalSize;
    int             assoc;

    // Statistics
    GStatsCntr      getSMsg;
    GStatsCntr      fwdGetSMsg;
    GStatsCntr      getMMsg;
    GStatsCntr      invMsg;
    GStatsCntr      flushMsg;
    GStatsCntr      fwdGetSConflictMsg;
    GStatsCntr      invConflictMsg;
    GStatsCntr      nackMsg;

    // State member variables
    std::vector<Cache*>         caches;
};

class FasTMAbortMoreReadsWins: public FasTMAbort {
public:
    FasTMAbortMoreReadsWins(const char tmStyle[], int32_t nProcs, int32_t line);
    virtual ~FasTMAbortMoreReadsWins() { }

private:
    virtual bool isHigherOrEqualPriority(Pid_t pid, Pid_t conflictPid);
};

class FasTMAbortOlderWins: public FasTMAbort {
public:
    FasTMAbortOlderWins(const char tmStyle[], int32_t nProcs, int32_t line);
    virtual ~FasTMAbortOlderWins() { }

    virtual TMBCStatus myBegin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual void completeFallback(Pid_t pid);
private:
    virtual bool isHigherOrEqualPriority(Pid_t pid, Pid_t conflictPid);
    Time_t getStartTime(Pid_t pid)   const { return startTime.at(pid); }

    std::map<Pid_t, Time_t>             startTime;
};

class TMEECoherence: public TMCoherence {
public:
    TMEECoherence(const char tmStyle[], int32_t nProcs, int32_t line);
    virtual ~TMEECoherence();
    virtual uint32_t getNackRetryStallCycles(ThreadContext* context);

    typedef CacheAssocTM    Cache;
    typedef TMLine          Line;
protected:
    virtual TMRWStatus TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual TMRWStatus TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       removeTransaction(Pid_t pid);
    virtual TMBCStatus myAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myBegin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual void completeFallback(Pid_t pid);

    Cache* getCache(Pid_t pid) { return caches.at(pid); }
    TMRWStatus handleConflict(Pid_t pid, std::set<Pid_t>& conflicting, VAddr caddr);
    virtual bool isHigherOrEqualPriority(Pid_t pid, Pid_t conflictPid);
    Time_t getStartTime(Pid_t pid)   const { return startTime.at(pid); }

    Line* lookupLine(Pid_t pid, VAddr raddr, InstContext* p_opStatus);

    // Configurable member variables
    int             totalSize;
    int             assoc;
    uint32_t        nackBase;
    uint32_t        nackCap;

    // RNG-related data
    struct random_data randBuf;
    static const int   RBUF_SIZE = 32;
    char               rbuf[RBUF_SIZE];

    // State member variables
    std::vector<Cache*>         caches;

    std::map<Pid_t, bool>               cycleFlags;
    std::map<Pid_t, Time_t>             startTime;
    std::map<Pid_t, size_t>             nackCount;
    std::map<Pid_t, Pid_t>              nackedBy;
};
class TMEENumReadsCoherence: public TMEECoherence {
public:
    TMEENumReadsCoherence(const char tmStyle[], int32_t nProcs, int32_t line):
        TMEECoherence(tmStyle, nProcs, line) {}
    virtual ~TMEENumReadsCoherence() {}
protected:
    virtual bool isHigherOrEqualPriority(Pid_t pid, Pid_t conflictPid);
};

class IdealPleaseTM: public TMCoherence {
public:
    IdealPleaseTM(const char tmStyle[], int32_t nProcs, int32_t line);
    virtual ~IdealPleaseTM();

    typedef CacheAssocTM    Cache;
    typedef TMLine          Line;
protected:
    virtual TMRWStatus TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual TMRWStatus TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual TMBCStatus myAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);

    Cache* getCache(Pid_t pid) { return caches.at(pid); }
    Line* replaceLine(Pid_t pid, VAddr raddr);
    void cleanDirtyLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except);
    void invalidateLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except);
    void abortTMWriters(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except, InstContext* p_opStatus);
    void abortTMSharers(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except, InstContext* p_opStatus);
    void handleConflicts(Pid_t pid, VAddr caddr, bool isTM, std::set<Pid_t>& conflicting);
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) = 0;

    // Configurable member variables
    int             totalSize;
    int             assoc;

    // Statistics
    GStatsCntr      getSMsg;
    GStatsCntr      fwdGetSMsg;
    GStatsCntr      getMMsg;
    GStatsCntr      invMsg;
    GStatsCntr      flushMsg;
    GStatsCntr      fwdGetSConflictMsg;
    GStatsCntr      invConflictMsg;
    GStatsCntr      rfchSuccMsg;
    GStatsCntr      rfchFailMsg;

    // State member variables
    std::vector<Cache*>         caches;
};

class TMIdealRequesterLoses: public IdealPleaseTM {
public:
    TMIdealRequesterLoses(const char tmStyle[], int32_t nProcs, int32_t line);
    virtual ~TMIdealRequesterLoses() { }
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

class TMIdealMoreReadsWins: public IdealPleaseTM {
public:
    TMIdealMoreReadsWins(const char tmStyle[], int32_t nProcs, int32_t line);
    virtual ~TMIdealMoreReadsWins() { }
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

class PleaseTM: public TMCoherence {
public:
    PleaseTM(const char tmStyle[], int32_t nProcs, int32_t line);
    virtual ~PleaseTM();

    typedef CacheAssocTM    Cache;
    typedef TMLine          Line;
protected:
    virtual TMRWStatus TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual TMRWStatus TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       removeTransaction(Pid_t pid);
    virtual TMBCStatus myAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    Line* replaceLine(Pid_t pid, VAddr raddr);
    Line* replaceLineTM(Pid_t pid, VAddr raddr);

    Cache* getCache(Pid_t pid) { return caches.at(pid); }
    void abortReplaced(Line* replaced, Pid_t byPid, VAddr byCaddr, TMAbortType_e abortType);
    void updateOverflow(Pid_t pid, VAddr newCaddr);
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) = 0;
    void sendGetM(Pid_t pid, VAddr raddr, bool isTM);
    void sendGetS(Pid_t pid, VAddr raddr, bool isTM);

    // Configurable member variables
    int             totalSize;
    int             assoc;
    size_t          maxOverflowSize;

    // State member variables
    std::vector<Cache*>         caches;
    std::map<Pid_t, std::set<VAddr> >   overflow;
};

class TMRequesterLoses: public PleaseTM {
public:
    TMRequesterLoses(const char tmStyle[], int32_t nProcs, int32_t line);
    virtual ~TMRequesterLoses() { }
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

class TMMoreReadsWinsCoherence: public PleaseTM {
public:
    TMMoreReadsWinsCoherence(const char tmStyle[], int32_t nProcs, int32_t line);
    virtual ~TMMoreReadsWinsCoherence() { }
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

extern TMCoherence *tmCohManager;

#endif
