#ifndef TM_COHERENCE
#define TM_COHERENCE

#include <map>
#include <set>
#include <list>
#include <vector>

#include "Snippets.h"
#include "GStats.h"
#include "libemul/InstDesc.h"
#include "libsuc/CacheCore.h"
#include "TMState.h"
#include "HWGate.h"

#define MAX_CPU_COUNT 512

typedef unsigned long long ID; 
typedef unsigned long long INSTCOUNT;
class MyPrefetcher;

enum TMBCStatus { TMBC_INVALID, TMBC_SUCCESS, TMBC_NACK, TMBC_ABORT, TMBC_IGNORE };
enum TMRWStatus { TMRW_SUCCESS, TMRW_NACKED, TMRW_ABORT };

enum EvictCause {
    NoEvict = 0,
    EvictByWrite,
    EvictSetConflict,
    EvictPrefetch,
};
class CState1 : public StateGeneric<> {
private:
    bool valid;
    bool dirty;
    bool prefetch;
public:
    CState1() {
        invalidate();
    }
    bool isDirty() const {
        return dirty;
    }
    void makeDirty() {
        dirty = true;
    }
    void makeClean() {
        dirty = false;
    }
    void markPrefetch() {
        prefetch = true;
    }
    void clearPrefetch() {
        prefetch = false;
    }
    bool wasPrefetch() const {
        return prefetch;
    }
    bool isValid() const {
        return valid;
    }
    void validate() {
        I(getTag());
        valid = true;
    }
    void invalidate() {
        valid = false;
        dirty = false;
        prefetch = false;
        clearTag();
    }
};

class PrivateCache {
public:
    PrivateCache(const char* section, const char* name, Pid_t p);
    ~PrivateCache();

    bool doLoad(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted);
    bool doStore(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted);
    void doInvalidate(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted);
    void doPrefetches(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted);
    void updatePrefetchers(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted);
    bool findLine(VAddr addr) const { return cache->findLineNoEffect(addr) != NULL; }

    void startTransaction() { isTransactional = true; }
    void stopTransaction() { cache->clearTransactional(); isTransactional = false; }
    bool isInTransaction() const { return isTransactional; }
    size_t getLineSize() const { return cache->getLineSize(); }
private:
    typedef CacheGeneric<CState1, VAddr>            Cache;
    typedef CacheGeneric<CState1, VAddr>::CacheLine Line;

    Line* doFillLine(VAddr addr, bool isPrefetch, std::map<Pid_t, EvictCause>& tmEvicted);

    Pid_t                       pid;
    bool                        isTransactional;
    Cache                      *cache;
    std::vector<MyPrefetcher*>  prefetchers;
    GStatsCntr                  readHit;
    GStatsCntr                  writeHit;
    GStatsCntr                  readMiss;
    GStatsCntr                  writeMiss;
    GStatsCntr                  usefulPrefetch;
    GStatsCntr                  lostPrefetch;
};

class MyPrefetcher {
public:
    virtual ~MyPrefetcher() {}
    virtual VAddr getAddr(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr) = 0;
    virtual void update(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr) = 0;
};

class MyNextLinePrefetcher: public MyPrefetcher {
public:
    VAddr getAddr(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr);
    void update(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr);
};

class MyStridePrefetcher: public MyPrefetcher {
public:
    VAddr getAddr(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr);
    void update(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr);
private:
    std::map<VAddr, int32_t>    prevTable;
    std::map<VAddr, int32_t>    strideTable;
};

class MyMarkovPrefetcher: public MyPrefetcher {
public:
    MyMarkovPrefetcher(size_t s): size(s), prevAddress(0) {}
    VAddr getAddr(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr);
    void update(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr);
private:
    const size_t                        size;
    VAddr                               prevAddress;
    std::map<VAddr, std::list<VAddr>>   corrTable;
};

class TMCoherence {
public:
    TMCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMCoherence() {}
    static TMCoherence *create(int32_t nProcs);

    TMRWStatus read(InstDesc* inst, ThreadContext* context, VAddr raddr, bool* p_l1Hit);
    TMRWStatus write(InstDesc* inst, ThreadContext* context, VAddr raddr, bool* p_l1Hit);
    TMBCStatus abort(Pid_t pid, int tid, TMAbortType_e abortType);
    TMBCStatus commit(Pid_t pid, int tid);
    TMBCStatus begin(Pid_t pid, InstDesc *inst);

    TMBCStatus completeAbort(Pid_t pid);
    TMRWStatus nonTMread(InstDesc* inst, ThreadContext* context, VAddr raddr, bool* p_l1Hit);
    TMRWStatus nonTMwrite(InstDesc* inst, ThreadContext* context, VAddr raddr, bool* p_l1Hit);
    void completeFallback(Pid_t pid);
    void markEvicted(Pid_t evicterPid, VAddr raddr, std::map<Pid_t, EvictCause>& evicted);

    const TransState& getTransState(Pid_t pid) { return transStates.at(pid); }
    int getReturnArgType()          const { return returnArgType; }
    uint64_t getUtid(Pid_t pid)     const { return transStates.at(pid).getUtid(); }
    size_t getDepth(Pid_t pid)      const { return transStates.at(pid).getDepth(); }
    bool checkNacked(Pid_t pid)    const { return transStates.at(pid).getState() == TM_NACKED; }
    bool checkAborting(Pid_t pid)   const { return transStates.at(pid).getState() == TM_ABORTING; }
    bool markedForAbort(Pid_t pid)  const { return transStates.at(pid).getState() == TM_MARKABORT; }
    Pid_t getTMNackOwner(Pid_t pid) const { return nackOwner.find(pid) != nackOwner.end() ? nackOwner.at(pid) : -1; }
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
    std::map<Pid_t, Pid_t> nackOwner;

    VAddr addrToCacheLine(VAddr raddr) {
        while(raddr % cacheLineSize != 0) {
            raddr = raddr-1;
        }
        return raddr;
    }
    void beginTrans(Pid_t pid, InstDesc* inst);
    void commitTrans(Pid_t pid);
    void abortTrans(Pid_t pid);
    void nackTrans(Pid_t pid);
    void readTrans(Pid_t pid, int tid, VAddr raddr, VAddr caddr);
    void writeTrans(Pid_t pid, int tid, VAddr raddr, VAddr caddr);
    void markTransAborted(Pid_t victimPid, Pid_t aborterPid, uint64_t aborterUtid, VAddr caddr, TMAbortType_e abortType);
    void markTransAborted(std::set<Pid_t>& aborted, Pid_t aborterPid, uint64_t aborterUtid, VAddr caddr, TMAbortType_e abortType);

    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr) = 0;
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr) = 0;
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual void myCompleteAbort(Pid_t pid) {}

    int nProcs;
    int cacheLineSize;
    size_t numLines;
    int returnArgType;
    int abortBaseStallCycles;
    int commitBaseStallCycles;
    uint32_t nackStallBaseCycles;
    uint32_t nackStallCap;
    size_t   maxNacks;

    static uint64_t nextUtid;

    std::vector<struct TransState>  transStates;
    std::map<Pid_t, PrivateCache*>    caches;

    void addWrite(VAddr caddr, Pid_t pid);
    void addRead(VAddr caddr, Pid_t pid);
    void removeTransaction(Pid_t pid);
    void removeFromList(std::list<Pid_t>& list, Pid_t pid);
    bool hadWrote(VAddr caddr, Pid_t pid);
    bool hadRead(VAddr caddr, Pid_t pid);
    void getWritersExcept(VAddr caddr, Pid_t pid, std::set<Pid_t>& w);
    void getReadersExcept(VAddr caddr, Pid_t pid, std::set<Pid_t>& r);

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

    std::map<VAddr, size_t> numAbortsCaused;
    std::map<Pid_t, std::set<VAddr> > linesRead;
    std::map<Pid_t, std::set<VAddr> > linesWritten;
    std::map<VAddr, std::list<Pid_t> > writers;
    std::map<VAddr, std::list<Pid_t> > readers;
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
    Pid_t   currentCommitter;                          //!< PID of the currently committing processor
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

class TMLEHourglassCoherence: public TMCoherence {
public:
    TMLEHourglassCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLEHourglassCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual void myCompleteAbort(Pid_t pid);
private:
    std::map<Pid_t, size_t> abortCount;
    size_t                  abortThreshold;
    Pid_t                   hourglassOwner;
    static const Pid_t      INVALID_HOURGLASS = 4096;
};

class TMLESOKCoherence: public TMCoherence {
public:
    TMLESOKCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLESOKCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual void myCompleteAbort(Pid_t pid);
private:
    bool inRunQueue(Pid_t pid);

    std::map<Pid_t, size_t> abortCount;
    std::map<Pid_t, std::list<Pid_t> > runQueues;
};

class TMLESOKQueueCoherence: public TMCoherence {
public:
    TMLESOKQueueCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLESOKQueueCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual void myCompleteAbort(Pid_t pid);
private:
    bool inRunQueue(Pid_t pid);

    std::map<Pid_t, size_t> abortCount;
    std::map<Pid_t, VAddr>  abortCause;
    std::map<Pid_t, std::list<Pid_t> > runQueues;
};

class TMLESOA0Coherence: public TMCoherence {
public:
    TMLESOA0Coherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLESOA0Coherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual void myCompleteAbort(Pid_t pid);
private:
    bool inRunQueue(Pid_t pid);

    std::map<VAddr, std::list<Pid_t> > runQueues;
    std::map<Pid_t, std::set<VAddr> > lockList;
};

class TMLESOA2Coherence: public TMCoherence {
public:
    TMLESOA2Coherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLESOA2Coherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
private:
    bool inRunQueue(Pid_t pid);

    std::map<VAddr, std::list<Pid_t> > runQueues;
    std::map<Pid_t, std::set<VAddr> > lockList;
};

class TMLEWARCoherence: public TMCoherence {
public:
    TMLEWARCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLEWARCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
private:
    void markReaders(VAddr caddr, Pid_t aborterPid, uint64_t aborterUtid, TMAbortType_e abortType);

    std::map<Pid_t, std::set<VAddr> > warChest;
};

class TMLEATSCoherence: public TMCoherence {
public:
    TMLEATSCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLEATSCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual void myCompleteAbort(Pid_t pid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
private:
    std::list<Pid_t> runQueue;
    std::map<Pid_t, double> abortCount;
    double  alpha;
};

class TMLELockCoherence: public TMCoherence {
public:
    TMLELockCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLELockCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual void myCompleteAbort(Pid_t pid);
private:
    void addMember(HWGate& gate, Pid_t pid);
    void updateAbortAddr(VAddr abortAddr, size_t count);
    void markAbort(VAddr caddr, Pid_t pid, HWGate& gate, TMAbortType_e abortType);
    size_t getAbortAddrCount(VAddr caddr);

    HWGate& newGate(Pid_t pid, VAddr caddr, bool readOnly);

    std::map<VAddr, size_t> abortAddrCount;
    std::map<VAddr, size_t> addrGateCount;
    std::list<VAddr> lruList;
    std::map<Pid_t, std::set<VAddr> > accessed;

    std::map<VAddr, HWGate> gates;
};

class TMLELock0Coherence: public TMCoherence {
public:
    TMLELock0Coherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLELock0Coherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual void myCompleteAbort(Pid_t pid);
private:
    void addMember(HWGate& gate, Pid_t pid);
    void updateAbortAddr(VAddr abortAddr, size_t count);
    void markAbort(VAddr caddr, Pid_t pid, HWGate& gate, TMAbortType_e abortType);
    size_t getAbortAddrCount(VAddr caddr);

    HWGate& newGate(Pid_t pid, VAddr caddr, bool readOnly);

    std::map<VAddr, size_t> abortAddrCount;
    std::map<VAddr, size_t> addrGateCount;
    std::list<VAddr> lruList;
    std::map<Pid_t, std::set<VAddr> > accessed;

    std::map<VAddr, HWGate> gates;
};

class TMLEAsetCoherence: public TMCoherence {
public:
    TMLEAsetCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLEAsetCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual void myCompleteAbort(Pid_t pid);
private:
    bool inRunQueue(Pid_t pid);

    std::map<Pid_t, std::set<VAddr> > accessed;
    std::map<Pid_t, size_t> abortCount;
    std::map<Pid_t, VAddr>  abortCause;
    std::map<Pid_t, std::list<Pid_t> > runQueues;
};

class TMLESnoopCoherence: public TMCoherence {
public:
    TMLESnoopCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLESnoopCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual void myCompleteAbort(Pid_t pid);
private:
    std::map<Pid_t, std::set<VAddr> > readLines;
    std::map<Pid_t, std::set<VAddr> > wroteLines;
    std::map<Pid_t, Pid_t> aborters;
};

class TMFirstWinsCoherence: public TMCoherence {
public:
    TMFirstWinsCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMFirstWinsCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus nonTMread(Pid_t pid, VAddr raddr);
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
    void abortOthers(Pid_t pid, VAddr raddr, std::set<Pid_t>& conflicting);
};

class TMOlderCoherence: public TMFirstWinsCoherence {
public:
    TMOlderCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMOlderCoherence() { }
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

class TMOlderAllCoherence: public TMFirstWinsCoherence {
public:
    TMOlderAllCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMOlderAllCoherence() { }
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);

    std::map<Pid_t, Time_t> startedAt;
};

class TMMoreCoherence: public TMFirstWinsCoherence {
public:
    TMMoreCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMMoreCoherence() { }
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

class TMLog2MoreCoherence: public TMFirstWinsCoherence {
public:
    TMLog2MoreCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLog2MoreCoherence() { }
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

class TMCappedMoreCoherence: public TMFirstWinsCoherence {
public:
    TMCappedMoreCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMCappedMoreCoherence() { }
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
    size_t      m_cap;
};

class TMNumAbortsCoherence: public TMFirstWinsCoherence {
public:
    TMNumAbortsCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMNumAbortsCoherence() { }
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual void myCompleteAbort(Pid_t pid);
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
    std::map<Pid_t, VAddr>  lastBegin;
    std::map<Pid_t, size_t> numAbortsSeen;
};

class TMFirstNotifyCoherence: public TMCoherence {
public:
    TMFirstNotifyCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMFirstNotifyCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual void myCompleteAbort(Pid_t pid);
protected:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
    void abortOthers(Pid_t pid, VAddr raddr, std::set<Pid_t>& conflicting);
    void getNacked(Pid_t pid, Pid_t nacker);
    void releaseNacker(Pid_t pid);

    GStatsCntr      usefulNacks;
    GStatsCntr      futileNacks;

    std::map<Pid_t, std::set<Pid_t> > nacking;
    std::map<Pid_t, Pid_t> nackedBy;
};

class TMFirstRetryCoherence: public TMCoherence {
public:
    TMFirstRetryCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMFirstRetryCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual void myCompleteAbort(Pid_t pid);
    virtual TMRWStatus nonTMread(Pid_t pid, VAddr raddr);
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
    void getNacked(Pid_t pid, std::set<Pid_t>& nackers);
    void nackOthers(Pid_t pid, std::set<Pid_t>& conflicting);
    Pid_t removeNack(Pid_t pid);
    void abortOthers(Pid_t pid, VAddr raddr, std::set<Pid_t>& conflicting);
    void selfAbort(Pid_t pid, VAddr caddr);
    void selfResume(Pid_t pid);

    size_t          maxNacks;
    GStatsCntr      usefulNacks;
    GStatsCntr      futileNacks;
    GStatsCntr      timedOutNacks;
    GStatsAvg       nackRefetches;

    std::map<Pid_t, size_t> numNacked;
    std::map<Pid_t, size_t> numRefetched; // # Times a core had to refetch a line
    std::map<Pid_t, Pid_t> nackedBy;
    std::map<Pid_t, uint64_t> nackerUtid;
};

class TMMoreNotifyCoherence: public TMFirstNotifyCoherence {
public:
    TMMoreNotifyCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMMoreNotifyCoherence() { }
protected:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

class TMMoreRetryCoherence: public TMFirstRetryCoherence {
public:
    TMMoreRetryCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMMoreRetryCoherence() { }
protected:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

class TMLog2MoreRetryCoherence: public TMFirstRetryCoherence {
public:
    TMLog2MoreRetryCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLog2MoreRetryCoherence() { }
protected:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

class TMCappedMoreRetryCoherence: public TMFirstRetryCoherence {
public:
    TMCappedMoreRetryCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMCappedMoreRetryCoherence() { }
protected:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
    size_t      m_cap;
};

class TMOlderRetryCoherence: public TMFirstRetryCoherence {
public:
    TMOlderRetryCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMOlderRetryCoherence() { }
protected:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

class TMOlderAllRetryCoherence: public TMFirstRetryCoherence {
public:
    TMOlderAllRetryCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMOlderAllRetryCoherence() { }
    TMBCStatus myBegin(Pid_t pid, InstDesc* inst);
    TMBCStatus myCommit(Pid_t pid, int tid);
protected:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
    std::map<Pid_t, Time_t> startedAt;
};

class TMNumAbortsRetryCoherence: public TMFirstRetryCoherence {
public:
    TMNumAbortsRetryCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMNumAbortsRetryCoherence() { }
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual void myCompleteAbort(Pid_t pid);
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);

    std::map<Pid_t, VAddr>  lastBegin;
    std::map<Pid_t, size_t> numAbortsSeen;
};

extern TMCoherence *tmCohManager;

#endif
