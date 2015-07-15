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
    TM_ATYPE_NONTM              = 4,    // Aborts due to conflict by a non-transaction
    TM_ATYPE_FALLBACK           = 254,  // Aborts due to an active fallback
    TM_ATYPE_INVALID            = 0xDEAD
};

static const uint64_t INVALID_UTID = -1;

class TMAbortState {
public:
    TMAbortState(Pid_t pid): myPid(pid) {
        clear();
    }
    void clear() {
        aborterPid  = INVALID_PID;
        aborterUtid = INVALID_UTID;
        abortType   = TM_ATYPE_INVALID;
        abortByAddr = 0;
        abortIAddr  = 0;
    }
    void markAbort(Pid_t byPid, uint64_t byUtid, VAddr byCaddr, TMAbortType_e type) {
        aborterPid  = byPid;
        aborterUtid = byUtid;
        abortType   = type;
        abortByAddr = byCaddr;
    }
    void setAbortIAddr(VAddr iAddr) {
        abortIAddr = iAddr;
    }
    Pid_t   getAborterPid() const   { return aborterPid; }
    uint64_t getAborterUtid() const { return aborterUtid; }
    TMAbortType_e getAbortType() const { return abortType; }
    VAddr   getAbortByAddr() const  { return abortByAddr; }
    VAddr   getAbortIAddr() const   { return abortIAddr; }
private:
    // The PID of the owner
    Pid_t           myPid;
    // The PID of the aborter
    Pid_t           aborterPid;
    // The UTID of the aborter
    uint64_t        aborterUtid;
    // Type of abort
    TMAbortType_e   abortType;
    // The abort-causing memory address (in case of data conflict abort)
    VAddr           abortByAddr;
    // The IAddr when we found out TM has aborted
    VAddr           abortIAddr;
};
class TransState {
public:

    TransState(Pid_t pid);

    void begin(uint64_t newUtid);
    void startAborting();
    void suspend();
    void resume();
    void completeAbort();
    void completeFallback();
    void markAbort();
    void commit();
    void print() const;

    TMState_e   getState()      const { return state; }
    uint64_t    getUtid()       const { return utid; }
    bool        getRestartPending() const { return restartPending; }

private:
    Pid_t           myPid;
    TMState_e       state;
    uint64_t        utid;
    bool            restartPending;
};

#endif
