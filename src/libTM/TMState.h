#ifndef TM_STATE
#define TM_STATE

#include "Snippets.h"
#include "libemul/Addressing.h"

static const uint64_t INVALID_UTID = -1;

/// Enum of return status values that are returned from begin/commit
enum TMBCStatus {
    TMBC_INVALID,       // Invalid. Getting this value indicates a bug
    TMBC_SUCCESS,       // Success
    TMBC_NACK,          // Operation was NACKED. Should retry later
    TMBC_ABORT          // Operation was ABORTED. Should call abort
};

/// Enum of return status values that are returned from read/write
enum TMRWStatus {
    TMRW_INVALID,       // Invalid. Getting this value indicates a bug
    TMRW_NONTM,         // Operation was non-transactional
    TMRW_SUCCESS,       // Success
    TMRW_NACKED,        // Operation was NACKED. Should retry later
    TMRW_ABORT          // Operation was ABORTED. Should call abort
};

enum TMAbortType_e {
    TM_ATYPE_DEFAULT            = 0,    // Aborts due to data conflict
    TM_ATYPE_USER               = 1,    // Aborts by the user (external abort)
    TM_ATYPE_SYSCALL            = 2,    // Aborts due to syscall (external abort)
    TM_ATYPE_SETCONFLICT        = 3,    // Aborts due to a set conflict (capacity)
    TM_ATYPE_NONTM              = 4,    // Aborts due to conflict by a non-transaction
    TM_ATYPE_INVALID            = 0xDEAD
};

// The states in which a TMBegin instruction can be in after being executed
enum TMBeginSubtype {
    TM_BEGIN_INVALID            = 0, // Uninitialized
    TM_BEGIN_REGULAR            = 1, // If the transaction started without problems

    TM_COMPLETE_ABORT           = 9, // Aborted transaction 're-executes' TMBegin with this state
};

// The states in which a TMCommit instruction can be in after being executed
enum TMCommitSubtype {
    TM_COMMIT_INVALID           = 0, // Unitialized
    TM_COMMIT_REGULAR           = 1, // If the transaction has committed
    TM_COMMIT_ABORTED           = 2, // The transaction failed to commit
};

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
class TMStateEngine {
public:
    enum State_e { TM_INVALID, TM_RUNNING, TM_ABORTING, TM_MARKABORT };
    TMStateEngine(Pid_t pid);

    void begin();
    void clear();
    void startAborting();
    void completeAbort();
    void markAbort();
    void print() const;

    // Getters
    State_e   getState()      const { return myState; }
    const char* getStateStr() { return getStateStr(myState); }
    static const char* getStateStr(State_e state);

private:
    void triggerFail(State_e nextState);

    // The PID of the owner
    Pid_t           myPid;
    // Current state of the transaction
    State_e         myState;
};

#endif
