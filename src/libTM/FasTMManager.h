#ifndef FASTM_MANAGER
#define FASTM_MANAGER

#include <set>
#include <map>
#include <vector>
#include "HTMManager.h"
#include "PrivateCache.h"

class FasTMAbort: public HTMManager {
public:
    FasTMAbort(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~FasTMAbort();

    typedef CacheAssocTM    Cache;
    typedef TMLine          Line;
protected:
    virtual TMRWStatus TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual TMRWStatus TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       myStartAborting(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);

    Cache* getCache(Pid_t pid) { return caches.at(pid); }
    TMRWStatus handleConflicts(Pid_t pid, VAddr caddr, std::set<Pid_t>& conflicting);
    virtual bool isHigherOrEqualPriority(Pid_t pid, Pid_t conflictPid) = 0;

    Line* replaceLine(Pid_t pid, VAddr raddr);
    void cleanDirtyLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except);
    void invalidateLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except);
    TMRWStatus abortTMWriters(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except);
    TMRWStatus abortTMSharers(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except);

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
    GStatsCntr      nackMsg;

    // State member variables
    std::vector<Cache*>         caches;
};

class FasTMAbortMoreReadsWins: public FasTMAbort {
public:
    FasTMAbortMoreReadsWins(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~FasTMAbortMoreReadsWins() { }

private:
    virtual bool isHigherOrEqualPriority(Pid_t pid, Pid_t conflictPid);
};

class FasTMAbortOlderWins: public FasTMAbort {
public:
    FasTMAbortOlderWins(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~FasTMAbortOlderWins() { }

    virtual TMBCStatus myBegin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual void completeFallback(Pid_t pid);
private:
    virtual bool isHigherOrEqualPriority(Pid_t pid, Pid_t conflictPid);
    Time_t getStartTime(Pid_t pid)   const { return startTime.at(pid); }

    std::map<Pid_t, Time_t>             startTime;
};

#endif
