#ifndef TM_CONTEXT
#define TM_CONTEXT

#include "TMStorage.h"

class TMContext
{
public:
    TMContext(ThreadContext* c, InstDesc* inst, uint64_t tmUtid);
    ~TMContext() {}

    /* Public Methods */
    InstDesc*   getBeginCode() { return tmBeginCode; }
    uint64_t    getUtid() const { return utid; }
    TMContext*  getParentContext() { return parent; }
    VAddr       getBeginIAddr() { return beginIAddr; }

    template<class T>
    void  cacheAccess(VAddr addr, T oval, T* p_val);
    template<class T>
    void  cacheWrite(VAddr addr, T val);

    void    saveContext();
    void    restoreContext();
    void    flushMemory() {
        cache2.flush(context);
    }

private:
    /* Variables */
    ThreadContext* context;   // Owner thread context
    InstDesc*   tmBeginCode;  // TM Begin Code Pointer
    VAddr       beginIAddr;
    Pid_t       pid;          // Copy of PID of owner thread
    uint64_t    utid;         // Unique transaction identifier
    RegVal      regs[NumOfRegs];      // Int Register Backup
    TMStorage2  cache2;        // The Memory Cache
    TMContext  *parent;      // Parent Transaction
};

template<class T>
void TMContext::cacheAccess(VAddr addr, T oval, T* p_val) {
    cache2.loadLine(context, addr);
    T newval = cache2.load<T>(addr);

    *p_val = newval;
}

template<class T>
void TMContext::cacheWrite(VAddr addr, T val) {
    cache2.loadLine(context, addr);
    cache2.store<T>(context, addr, val);
}

#endif
