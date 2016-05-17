#ifndef PLEASETM_MANAGER
#define PLEASETM_MANAGER

#include <set>
#include <map>
#include <vector>
#include "HTMManager.h"
#include "PrivateCache.h"

class IdealPleaseTM: public HTMManager {
public:
    IdealPleaseTM(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~IdealPleaseTM();

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
};

class TMIdealRequesterLoses: public IdealPleaseTM {
public:
    TMIdealRequesterLoses(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~TMIdealRequesterLoses() { }
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

class TMIdealMoreReadsWins: public IdealPleaseTM {
public:
    TMIdealMoreReadsWins(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~TMIdealMoreReadsWins() { }
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

class TMIdealOlderWins: public IdealPleaseTM {
public:
    TMIdealOlderWins(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~TMIdealOlderWins() { }

    virtual TMBCStatus myBegin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual void completeFallback(Pid_t pid);
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
    std::map<Pid_t, Time_t>             startTime;
};

class TMIdealMoreAbortsWins: public IdealPleaseTM {
public:
    TMIdealMoreAbortsWins(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~TMIdealMoreAbortsWins() { }
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

class TMLog2MoreCoherence: public IdealPleaseTM {
public:
    TMLog2MoreCoherence(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~TMLog2MoreCoherence() { }
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
};

class TMCappedMoreCoherence: public IdealPleaseTM {
public:
    TMCappedMoreCoherence(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~TMCappedMoreCoherence() { }
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
    const size_t m_cap;
};

class TMIdealOlderAllWins: public IdealPleaseTM {
public:
    TMIdealOlderAllWins(const char tmStyle[], int32_t nCores, int32_t line);
    virtual ~TMIdealOlderAllWins() { }

    virtual TMBCStatus myBegin(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual TMBCStatus myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus);
    virtual void completeFallback(Pid_t pid);
private:
    virtual bool shouldAbort(Pid_t pid, VAddr raddr, Pid_t other);
    std::map<Pid_t, Time_t>             startTime;
};


#endif
