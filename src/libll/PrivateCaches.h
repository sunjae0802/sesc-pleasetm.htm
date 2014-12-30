#ifndef PRIVATECACHES_H
#define PRIVATECACHES_H

#include <vector>
#include <set>
#include <map>
#include "libemul/AddressSpace.h"
#include "libemul/InstDesc.h"
#include "CacheCore.h"

// Forward declarations
class ThreadContext;
class MyPrefetcher;

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
    typedef CacheGeneric<CState1, VAddr>            PrivateCache;
    typedef CacheGeneric<CState1, VAddr>::CacheLine Line;
private:

    Line* doFillLine(Pid_t pid, VAddr addr, bool isTransactional, bool isPrefetch, std::map<Pid_t, EvictCause>& tmEvicted);
    void doPrefetches(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted);

    const size_t                        nCores;
    std::vector<PrivateCache*>          caches;
    std::vector<MyPrefetcher*>            prefetchers;
    std::map<Pid_t, std::set<VAddr> >   activeAddrs;
};

class MyPrefetcher {
public:
    virtual VAddr getAddr(InstDesc* inst, ThreadContext* context, PrivateCaches::PrivateCache* cache, VAddr addr) = 0;
};

class MyNextLinePrefetcher: public MyPrefetcher {
public:
    VAddr getAddr(InstDesc* inst, ThreadContext* context, PrivateCaches::PrivateCache* cache, VAddr addr);
};

class MyStridePrefetcher: public MyPrefetcher {
public:
    VAddr getAddr(InstDesc* inst, ThreadContext* context, PrivateCaches::PrivateCache* cache, VAddr addr);
private:
    std::map<VAddr, int32_t>    prevTable;
    std::map<VAddr, int32_t>    strideTable;
};

extern PrivateCaches* privateCacheManager;

#endif
