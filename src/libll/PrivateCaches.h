#ifndef PRIVATECACHES_H
#define PRIVATECACHES_H

#include <vector>
#include <set>
#include <map>
#include "libemul/AddressSpace.h"
#include "libemul/InstDesc.h"
#include "CacheCore.h"

class ThreadContext;

enum EvictCause {
    NoEvict = 0,
    EvictByWrite,
    EvictSetConflict,
    EvictPrefetch,
};

class CState1 : public StateGeneric<> {
private:
    bool valid;
    bool dirty;
    bool prefetch;
public:
    CState1() {
        invalidate();
    }
    bool isDirty() const {
        return dirty;
    }
    void makeDirty() {
        dirty = true;
    }
    void makeClean() {
        dirty = false;
    }
    void markPrefetch() {
        prefetch = true;
    }
    void clearPrefetch() {
        prefetch = false;
    }
    bool wasPrefetch() const {
        return prefetch;
    }
    bool isValid() const {
        return valid;
    }
    void validate() {
        I(getTag());
        valid = true;
    }
    void invalidate() {
        valid = false;
        dirty = false;
        prefetch = false;
        clearTag();
    }
};

class PrivateCaches {
public:
    PrivateCaches(const char *section, size_t n);
    ~PrivateCaches();
    bool doLoad(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted);
    bool doStore(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted);
    bool findLine(Pid_t pid, VAddr addr);
    void clearTransactional(Pid_t pid) {
        PrivateCache* cache = caches.at(pid);
        cache->clearTransactional();
    }
private:
    typedef CacheGeneric<CState1, VAddr>            PrivateCache;
    typedef CacheGeneric<CState1, VAddr>::CacheLine Line;

    Line* doFillLine(Pid_t pid, VAddr addr, bool isTransactional, bool isPrefetch, std::map<Pid_t, EvictCause>& tmEvicted);
    void doNextLinePrefetch(Pid_t pid, VAddr addr, bool isTransactional, std::map<Pid_t, EvictCause>& tmEvicted);

    bool                                nextLinePrefetch;
    const size_t                        nCores;
    std::vector<PrivateCache*>          caches;
    std::map<Pid_t, std::set<VAddr> >   activeAddrs;
};

extern PrivateCaches* privateCacheManager;

#endif
