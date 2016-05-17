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
    virtual void myStartAborting(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);

    // Helper functions
    Cache* getCache(Pid_t pid) { return caches.at(pid/nSMTWays); }
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

#endif
