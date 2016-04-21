#ifndef IDEAL_TSX_MANAGER
#define IDEAL_TSX_MANAGER

#include <set>
#include <vector>
#include "HTMManager.h"
#include "PrivateCache.h"

class IdealTSXManager: public HTMManager {
public:
    IdealTSXManager(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~IdealTSXManager();

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
    Cache* getCache(Pid_t pid) const { return caches.at(pid/nSMTWays); }
    Line* replaceLine(Cache* cache, VAddr raddr);
    void clearTransactional(VAddr caddr, const PidSet& toClear);
    void cleanDirtyLines(VAddr caddr, Pid_t pid);
    void invalidateLines(VAddr caddr, Pid_t pid);
    void abortTMWriters(Pid_t pid, VAddr caddr, bool isTM);
    void abortTMSharers(Pid_t pid, VAddr caddr, bool isTM);

    // Configurable member variables
    int             totalSize;
    int             assoc;

    // State member variables
    std::vector<Cache*>         caches;
};

#endif
