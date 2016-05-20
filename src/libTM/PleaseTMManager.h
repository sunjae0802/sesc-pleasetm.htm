#ifndef PLEASETM_MANAGER
#define PLEASETM_MANAGER

#include <set>
#include <map>
#include <vector>
#include "HTMManager.h"
#include "PrivateCache.h"

class PleaseTM: public HTMManager {
public:
    PleaseTM(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~PleaseTM();

    typedef CacheAssocTM    Cache;
    typedef TMLine          Line;
protected:
    virtual TMRWStatus TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual TMRWStatus TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus);
    virtual void       myStartAborting(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);

    // Helper functions
    Cache* getCache(Pid_t pid) { return caches.at(pid/nSMTWays); }
    void getPeers(Pid_t pid, std::set<Pid_t>& peers);
    void updateOverflow(Pid_t pid, VAddr newCaddr);
    Line* replaceLine(Pid_t pid, VAddr raddr);
    void cleanDirtyLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except);
    void invalidateLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except);
    void abortTMWriters(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except, InstContext* p_opStatus);
    void abortTMSharers(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except, InstContext* p_opStatus);
    void handleConflicts(Pid_t pid, VAddr caddr, bool isTM, std::set<Pid_t>& conflicting);
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other) = 0;

    // Configurable member variables
    int             totalSize;
    int             assoc;
    size_t          maxOverflowSize;

    // Statistics
    GStatsCntr      getSMsg;
    GStatsCntr      fwdGetSMsg;
    GStatsCntr      getMMsg;
    GStatsCntr      invMsg;
    GStatsCntr      flushMsg;
    GStatsCntr      fwdGetSConflictMsg;
    GStatsCntr      invConflictMsg;
    GStatsCntr      rfchSuccMsg;
    GStatsCntr      rfchFailMsg;

    // State member variables
    std::vector<Cache*>         caches;
    std::map<Pid_t, std::set<VAddr> >   overflow;
};

class PTMRequesterLoses: public PleaseTM {
public:
    PTMRequesterLoses(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~PTMRequesterLoses() { }
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

class PTMMoreReadsWins: public PleaseTM {
public:
    PTMMoreReadsWins(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~PTMMoreReadsWins() { }
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

class PTMOlderWins: public PleaseTM {
public:
    PTMOlderWins(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~PTMOlderWins() { }

    virtual TMBCStatus myBegin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual void completeFallback(Pid_t pid);
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
    std::map<Pid_t, Time_t>             startTime;
};

class PTMMoreAbortsWins: public PleaseTM {
public:
    PTMMoreAbortsWins(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~PTMMoreAbortsWins() { }
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

class PTMLog2MoreCoherence: public PleaseTM {
public:
    PTMLog2MoreCoherence(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~PTMLog2MoreCoherence() { }
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

class PTMCappedMoreCoherence: public PleaseTM {
public:
    PTMCappedMoreCoherence(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~PTMCappedMoreCoherence() { }
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
    const size_t m_cap;
};

class PTMOlderAllWins: public PleaseTM {
public:
    PTMOlderAllWins(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~PTMOlderAllWins() { }

    virtual TMBCStatus myBegin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual void completeFallback(Pid_t pid);
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
    std::map<Pid_t, Time_t>             startTime;
};


#endif
