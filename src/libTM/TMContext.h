#ifndef TM_CONTEXT
#define TM_CONTEXT

#include <assert.h>

#include "TMCoherence.h"
#include "TMStorage.h"

class TMContext
{
public:
    TMContext(ThreadContext* c, InstDesc* inst, uint64_t tmUtid);
    ~TMContext() {}

    /* Public Methods */
    InstDesc*   getBeginCode() { return tmBeginCode; }
    int         getId() const { return tid; }
    uint64_t    getUtid() const { return utid; }
    TMContext*  getParentContext() { return parent; }
    VAddr       getBeginIAddr() { return beginIAddr; }

    TMBCStatus  beginTransaction(InstDesc *inst);
    TMBCStatus  abortTransaction();
    TMBCStatus  commitTransaction(InstDesc *inst);

    template<class T>
    TMRWStatus  cacheAccess(VAddr addr, T oval, T* p_val);
    template<class T>
    TMRWStatus  cacheWrite(VAddr addr, T val);

    void    saveContext();
    void    restoreContext();
    void    flushMemory() {
        cache.flush(context);
    }

private:
    /* Variables */
    ThreadContext* context;   // Owner thread context
    InstDesc*   tmBeginCode;  // TM Begin Code Pointer
    VAddr       beginIAddr;
    Pid_t       pid;          // Copy of PID of owner thread
    int         tid;          // Transaction ID
    uint64_t    utid;         // Unique transaction identifier
    RegVal      regs[NumOfRegs];      // Int Register Backup
    TMStorage     cache;        // The Memory Cache
    TMContext  *parent;      // Parent Transaction
};

template<class T>
TMRWStatus TMContext::cacheAccess(VAddr addr, T oval, T* p_val) {
    T val = 0x10;
    bool found = false;

    TMRWStatus status = tmCohManager->read(pid, tid, addr);
    if(status == TMRW_SUCCESS) {
        if(sizeof(T) == 1) {
            val = (T)cache.load8(addr, &found);
        } else if(sizeof(T) == 2) {
            val = (T)cache.load16(addr, &found);
        } else if(sizeof(T) == 4) {
            val = (T)cache.load32(addr, &found);
        } else if(sizeof(T) == 8) {
            val = (T)cache.load64(addr, &found);
        } else {
            assert(0);
        }
    }

    if (!found) {
        *p_val = oval;
    } else {
        *p_val = val;
    }
    return status;
}

template<class T>
TMRWStatus TMContext::cacheWrite(VAddr addr, T val) {
    TMRWStatus status = tmCohManager->write(pid, tid, addr);

    if(status == TMRW_SUCCESS) {
        cache.store(context, addr, val);
    }
    return status;
}

#endif
