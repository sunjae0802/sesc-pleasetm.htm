#ifndef TM_STATE
#define TM_STATE

#include "Snippets.h"
#include "libemul/Addressing.h"

enum TMState_e { TM_INVALID, TM_RUNNING, TM_NACKED, TM_ABORTING, TM_MARKABORT };
enum TMAbortType_e { TM_ATYPE_NONTM = 255, TM_ATYPE_DEFAULT = 0, TM_ATYPE_USER = 1, TM_ATYPE_CAPACITY = 2, TM_ATYPE_SYSCALL = 3 };

static const Time_t INVALID_TIMESTAMP = ((~0ULL) - 1024);
static const uint64_t INVALID_UTID = -1;

class TransState {
public:
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

    TransState(Pid_t pid);

    void begin(uint64_t newUtid);
    void beginNested();
    void commitNested();
    void startAborting();
    void startNacking();
    void resumeAfterNack();
    void completeAbort();
    void completeFallback();
    void markAbort(Pid_t byPid, uint64_t byUtid, VAddr byCaddr, int abortType);
    void commit();
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

#endif
