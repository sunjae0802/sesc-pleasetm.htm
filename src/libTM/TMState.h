#ifndef TM_STATE
#define TM_STATE

#include "Snippets.h"
#include "libemul/Addressing.h"

enum TMState_e { TM_INVALID, TM_RUNNING, TM_SUSPENDED, TM_ABORTING, TM_MARKABORT };
enum TMAbortType_e {
    TM_ATYPE_DEFAULT            = 0,    // Aborts due to data conflict
    TM_ATYPE_USER               = 1,    // Aborts by the user (external abort)
    TM_ATYPE_SYSCALL            = 2,    // Aborts due to syscall (external abort)
    TM_ATYPE_SETCONFLICT        = 3,    // Aborts due to a set conflict (capacity)
    TM_ATYPE_NONTM              = 255
};

static const Time_t INVALID_TIMESTAMP = ((~0ULL) - 1024);
static const uint64_t INVALID_UTID = -1;

struct TMAbortState {
    TMAbortState(): aborterPid(-1), aborterUtid(INVALID_UTID),
                abortByAddr(0), abortType(TM_ATYPE_DEFAULT) {}
    void clear() {
        aborterPid  = -1;
        aborterUtid = INVALID_UTID;
        abortByAddr = 0;
        abortType   = TM_ATYPE_DEFAULT;
    }
    void markAbort(Pid_t byPid, uint64_t byUtid, VAddr byCaddr, TMAbortType_e type) {
        aborterPid  = byPid;
        aborterUtid = byUtid;
        abortByAddr = byCaddr;
        abortType   = type;
    }
    Pid_t           aborterPid;
    uint64_t        aborterUtid;
    VAddr           abortByAddr;
    TMAbortType_e   abortType;
};
class TransState {
public:

    TransState(Pid_t pid);

    void begin(uint64_t newUtid);
    void beginNested();
    void commitNested();
    void startAborting();
    void suspend();
    void resume();
    void completeAbort();
    void completeFallback();
    void markAbort(Pid_t byPid, uint64_t byUtid, VAddr byCaddr, TMAbortType_e abortType);
    void commit();
    void print() const;

    TMState_e   getState()      const { return state; }
    uint64_t    getUtid()       const { return utid; }
    Time_t      getTimestamp()  const { return timestamp; }
    size_t      getDepth()      const { return depth; }
    bool        getRestartPending() const { return restartPending; }
    const TMAbortState& getAbortState() const { return abortState; }
    TMAbortType_e getAbortType() const { return abortState.abortType; }
    Pid_t       getAborterPid() const { return abortState.aborterPid; }
    uint64_t    getAborterUtid() const { return abortState.aborterUtid; }
    VAddr       getAbortBy()    const { return abortState.abortByAddr; }

private:
    Pid_t           myPid;
    TMState_e       state;
    Time_t          timestamp;
    uint64_t        utid;
    size_t          depth;
    bool            restartPending;
    TMAbortState    abortState;
};

#endif
