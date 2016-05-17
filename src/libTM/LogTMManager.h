#ifndef LOGTM_MANAGER
#define LOGTM_MANAGER

#include <set>
#include <map>
#include <vector>
#include "HTMManager.h"
#include "PrivateCache.h"

class IdealLogTM: public HTMManager {
public:
    IdealLogTM(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~IdealLogTM();
    virtual uint32_t getNackRetryStallCycles(ThreadContext* context);

    typedef CacheAssocTM    Cache;
    typedef TMLine          Line;
protected:
    virtual TMRWStatus TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual TMRWStatus TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       myStartAborting(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myBegin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual void completeFallback(Pid_t pid);

    Cache* getCache(Pid_t pid) { return caches.at(pid); }
    TMRWStatus handleConflicts(Pid_t pid, VAddr caddr, std::set<Pid_t>& conflicting);
    virtual bool isHigherOrEqualPriority(Pid_t pid, Pid_t conflictPid);
    Time_t getStartTime(Pid_t pid)   const { return startTime.at(pid); }

    Line* replaceLine(Pid_t pid, VAddr raddr);
    void cleanDirtyLines(VAddr raddr, std::set<Cache*>& except);
    void invalidateLines(VAddr raddr, std::set<Cache*>& except);
    TMRWStatus abortTMWriters(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except);
    TMRWStatus abortTMSharers(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except);

    // Configurable member variables
    int             totalSize;
    int             assoc;
    uint32_t        nackBase;
    uint32_t        nackCap;

    // RNG-related data
    struct random_data randBuf;
    static const int   RBUF_SIZE = 32;
    char               rbuf[RBUF_SIZE];

    // Statistics
    GStatsCntr      getSMsg;
    GStatsCntr      fwdGetSMsg;
    GStatsCntr      getMMsg;
    GStatsCntr      invMsg;
    GStatsCntr      flushMsg;
    GStatsCntr      fwdGetSConflictMsg;
    GStatsCntr      invConflictMsg;
    GStatsCntr      nackMsg;

    // State member variables
    std::vector<Cache*>         caches;

    std::map<Pid_t, bool>               cycleFlags;
    std::map<Pid_t, Time_t>             startTime;
    std::map<Pid_t, size_t>             nackCount;
    std::map<Pid_t, Pid_t>              nackedBy;
};

#endif
