#ifndef TM_COHERENCE
#define TM_COHERENCE

#include <map>
#include <set>
#include <algorithm>
#include <list>
#include <vector>

#include "Snippets.h"
#include "libemul/InstDesc.h"
#include "HWGate.h"

#define MAX_CPU_COUNT 512

typedef unsigned long long ID; 
typedef unsigned long long INSTCOUNT;

enum TMState_e { TM_INVALID, TM_RUNNING, TM_NACKED, TM_ABORTING, TM_MARKABORT };
enum TMBCStatus { TMBC_INVALID, TMBC_SUCCESS, TMBC_NACK, TMBC_ABORT, TMBC_IGNORE };
enum TMRWStatus { TMRW_SUCCESS, TMRW_NACKED, TMRW_ABORT };
enum TMAbortType_e { TM_ATYPE_NONTM = 255, TM_ATYPE_DEFAULT = 0, TM_ATYPE_USER = 1, TM_ATYPE_CAPACITY = 2, TM_ATYPE_SYSCALL = 3 };

static const Time_t INVALID_TIMESTAMP = ((~0ULL) - 1024);
static const uint64_t INVALID_UTID = -1;

class TransState {

public:
    TransState(Pid_t pid): myPid(pid), state(TM_INVALID), stallUntil(0), timestamp(INVALID_TIMESTAMP),
            utid(INVALID_UTID), depth(0), restartPending(false) {}

    struct AbortState {
        AbortState(): aborterPid(-1), aborterUtid(INVALID_UTID), abortByAddr(0), abortType(TM_ATYPE_DEFAULT) {}
        void clear() {
            aborterPid  = -1;
            aborterUtid = INVALID_UTID;
            abortByAddr = 0;
            abortType   = TM_ATYPE_DEFAULT;
        }
        void markAbort(Pid_t byPid, uint64_t byUtid, VAddr byCaddr, int type) {
            aborterPid  = byPid;
            aborterUtid = byUtid;
            abortByAddr = byCaddr;
            abortType   = type;
        }
        Pid_t   aborterPid;
        uint64_t aborterUtid;
        VAddr   abortByAddr;
        int     abortType;
    };

    void begin(uint64_t newUtid) {
        timestamp   = globalClock;
        utid        = newUtid;
        state       = TM_RUNNING;
        I(depth == 0);
        depth       = 1;

        abortState.clear();
        restartPending = false;
    }
    void beginNested() {
        depth++;
    }
    void commitNested() {
        depth--;
    }
    void startAborting() {
        state       = TM_ABORTING;
        // We can't just decrement because we should be going back to the original begin
        //depth       = 0;
    }
    void startNacking() {
        state       = TM_NACKED;
    }
    void resumeAfterNack() {
        I(state == TM_NACKED);
        state       = TM_RUNNING;
    }
    void completeAbort() {
        I(state == TM_ABORTING);
        timestamp   = INVALID_TIMESTAMP;
        utid        = INVALID_UTID;
        state       = TM_INVALID;
        // We can't just decrement because we should be going back to the original begin
        depth       = 0;
        restartPending = true;
    }
    void completeFallback() {
        restartPending = false;
        abortState.clear();
    }
    void markAbort(Pid_t byPid, uint64_t byUtid, VAddr byCaddr, int abortType) {
        state       = TM_MARKABORT;
        abortState.markAbort(byPid, byUtid, byCaddr, abortType);
    }
    void commit() {
        timestamp   = INVALID_TIMESTAMP;
        utid        = INVALID_UTID;
        state       = TM_INVALID;
        I(depth == 1);
        depth       = 0;
        abortState.clear();
    }
    void startStalling(Time_t util) {
        stallUntil = util;
    }
    bool checkStall(Time_t clock) const {
        return stallUntil >= clock;
    }
    void print() const;

    TMState_e getState() const { return state; }
    uint64_t getUtid() const { return utid; }
    Time_t getTimestamp() const { return timestamp; }
    size_t getDepth() const { return depth; }
    bool  getRestartPending() const { return restartPending; }
    int   getAbortType() const { return abortState.abortType; }
    Pid_t getAborterPid() const { return abortState.aborterPid; }
    uint64_t getAborterUtid() const { return abortState.aborterUtid; }
    VAddr getAbortBy() const { return abortState.abortByAddr; }

private:
    Pid_t   myPid;
    TMState_e state;
    Time_t  stallUntil;
    Time_t  timestamp;
    uint64_t utid;
    size_t  depth;
    bool    restartPending;
    AbortState abortState;
};

class TMCoherence {
public:
    TMCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMCoherence() {}

    TMRWStatus read(Pid_t pid, int tid, VAddr raddr);
    TMRWStatus write(Pid_t pid, int tid, VAddr raddr);
    TMBCStatus abort(Pid_t pid, int tid, uint32_t abortType);
    TMBCStatus commit(Pid_t pid, int tid);
    TMBCStatus begin(Pid_t pid, InstDesc *inst);

    TMBCStatus completeAbort(Pid_t pid);
    TMRWStatus nonTMread(Pid_t pid, VAddr raddr);
    TMRWStatus nonTMwrite(Pid_t pid, VAddr raddr);
    void completeFallback(Pid_t pid);

    const TransState& getTransState(Pid_t pid) { return transStates.at(pid); }
    int getReturnArgType()          const { return returnArgType; }
    uint64_t getUtid(Pid_t pid)     const { return transStates.at(pid).getUtid(); }
    size_t getDepth(Pid_t pid)      const { return transStates.at(pid).getDepth(); }
    bool checkNacking(Pid_t pid)    const { return transStates.at(pid).getState() == TM_NACKED; }
    bool checkAborting(Pid_t pid)   const { return transStates.at(pid).getState() == TM_ABORTING; }
    bool markedForAbort(Pid_t pid)  const { return transStates.at(pid).getState() == TM_MARKABORT; }
    Pid_t getTMNackOwner(Pid_t pid) const { return nackOwner.find(pid) != nackOwner.end() ? nackOwner.at(pid) : -1; }

    bool checkStall(Pid_t pid)      const {
        return pid >= 0 && transStates.at(pid).checkStall(globalClock);
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
    void nackTrans(Pid_t pid, TimeDelta_t stallCycles);
    void readTrans(Pid_t pid, int tid, VAddr raddr, VAddr caddr);
    void writeTrans(Pid_t pid, int tid, VAddr raddr, VAddr caddr);
    void markTransAborted(Pid_t victimPid, Pid_t aborterPid, uint64_t aborterUtid, VAddr caddr, int abortType);
    void markTransAborted(std::set<Pid_t>& aborted, Pid_t aborterPid, uint64_t aborterUtid, VAddr caddr, int abortType);

    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr) = 0;
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr) = 0;
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual void myCompleteAbort(Pid_t pid) {}

    bool cacheOverflowed(Pid_t pid, VAddr caddr) const {
        const std::set<VAddr>& linesAccessed = cacheLines.at(pid);
        return (linesAccessed.size() == numLines && linesAccessed.find(caddr) == linesAccessed.end());
    }

    int nProcs;
    int cacheLineSize;
    size_t numLines;
    int returnArgType;
    int abortBaseStallCycles;
    int commitBaseStallCycles;

    static uint64_t nextUtid;

    std::vector<struct TransState>  transStates;
    static const int MAX_EXP_BACKOFF = 10;
    std::map<Pid_t, std::set<VAddr> > cacheLines;

    void addWrite(VAddr caddr, Pid_t pid);
    void addRead(VAddr caddr, Pid_t pid);
    void removeTransaction(Pid_t pid);
    void removeFromList(std::list<Pid_t>& list, Pid_t pid);
    bool hadWrote(VAddr caddr, Pid_t pid);
    bool hadRead(VAddr caddr, Pid_t pid);
    void getWritersExcept(VAddr caddr, Pid_t pid, std::set<Pid_t>& w);
    void getReadersExcept(VAddr caddr, Pid_t pid, std::set<Pid_t>& r);
    std::map<Pid_t, std::set<VAddr> > linesRead;
    std::map<Pid_t, std::set<VAddr> > linesWritten;
    std::map<VAddr, std::list<Pid_t> > writers2;
    std::map<VAddr, std::list<Pid_t> > readers2;
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
    int     nackStallCycles;
};

class TMLLCoherence: public TMCoherence {
public:
    TMLLCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLLCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
private:
    Pid_t   currentCommitter;                          //!< PID of the currently committing processor
    int     abortVarStallCycles;
    int     commitVarStallCycles;
    int     nackStallCycles;
};

class TMLECoherence: public TMCoherence {
public:
    TMLECoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLECoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
private:
};

class TMLEHourglassCoherence: public TMCoherence {
public:
    TMLEHourglassCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLEHourglassCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
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
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual void myCompleteAbort(Pid_t pid);
private:
    bool inRunQueue(Pid_t pid);

    std::map<Pid_t, size_t> abortCount;
    std::map<Pid_t, VAddr>  abortCause;
    std::map<Pid_t, std::list<Pid_t> > runQueues;
};

class TMLESOKQueueCoherence: public TMCoherence {
public:
    TMLESOKQueueCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLESOKQueueCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
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
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
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
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
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
    void markReaders(VAddr caddr, Pid_t aborterPid, uint64_t aborterUtid, int abortType);

    std::map<Pid_t, std::set<VAddr> > warChest;
};

class TMLEATSCoherence: public TMCoherence {
public:
    TMLEATSCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMLEATSCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
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
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual void myCompleteAbort(Pid_t pid);
private:
    void addMember(HWGate& gate, Pid_t pid);
    void updateAbortAddr(VAddr abortAddr, size_t count);
    void markAbort(VAddr caddr, Pid_t pid, HWGate& gate, int abortType);
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
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual void myCompleteAbort(Pid_t pid);
private:
    void addMember(HWGate& gate, Pid_t pid);
    void updateAbortAddr(VAddr abortAddr, size_t count);
    void markAbort(VAddr caddr, Pid_t pid, HWGate& gate, int abortType);
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
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
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
private:
    Pid_t shouldAbort(std::set<Pid_t>& m, Pid_t pid);
};

class TMOlderCoherence: public TMCoherence {
public:
    TMOlderCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMOlderCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
private:
    Pid_t shouldAbort(std::set<Pid_t>& m, Pid_t pid);
    std::map<Pid_t, Time_t> started;
};

class TMOlderAllCoherence: public TMCoherence {
public:
    TMOlderAllCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMOlderAllCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
private:
    Pid_t shouldAbort(std::set<Pid_t>& m, Pid_t pid);
    std::map<Pid_t, Time_t> started;
};

class TMMoreCoherence: public TMCoherence {
public:
    TMMoreCoherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMMoreCoherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
private:
    Pid_t shouldAbort(std::set<Pid_t>& m, Pid_t pid);
};

class TMFirstWins2Coherence: public TMCoherence {
public:
    TMFirstWins2Coherence(int32_t nProcs, int lineSize, int lines, int returnArgType);
    virtual ~TMFirstWins2Coherence() { }
    virtual TMRWStatus myRead(Pid_t pid, int tid, VAddr raddr);
    virtual TMRWStatus myWrite(Pid_t pid, int tid, VAddr raddr);
    virtual TMBCStatus myBegin(Pid_t pid, InstDesc *inst);
    virtual TMBCStatus myCommit(Pid_t pid, int tid);
    virtual TMBCStatus myAbort(Pid_t pid, int tid);
private:
    std::map<Pid_t, std::set<Pid_t> > nacking;
    std::map<Pid_t, Pid_t> nackedBy;
    std::set<Pid_t> nackingTMs;
};

extern TMCoherence *tmCohManager;

#endif
