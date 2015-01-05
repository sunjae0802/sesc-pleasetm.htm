#ifndef PRIVATECACHES_H
#define PRIVATECACHES_H

#include <vector>
#include <set>
#include <map>
#include "GStats.h"
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

class PrivateCache {
public:
    PrivateCache(const char* section, const char* name, Pid_t p);
    ~PrivateCache();

    bool doLoad(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted);
    bool doStore(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted);
    void doInvalidate(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted);
    void doPrefetches(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted);
    void updatePrefetchers(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted);
    bool findLine(VAddr addr) const { return cache->findLineNoEffect(addr) != NULL; }

    void startTransaction() { isTransactional = true; }
    void stopTransaction() { cache->clearTransactional(); isTransactional = false; }
    bool isInTransaction() const { return isTransactional; }
    size_t getLineSize() const { return cache->getLineSize(); }
private:
    typedef CacheGeneric<CState1, VAddr>            Cache;
    typedef CacheGeneric<CState1, VAddr>::CacheLine Line;

    Line* doFillLine(VAddr addr, bool isPrefetch, std::map<Pid_t, EvictCause>& tmEvicted);

    Pid_t                       pid;
    bool                        isTransactional;
    Cache                      *cache;
    std::vector<MyPrefetcher*>  prefetchers;
    GStatsCntr                  readHit;
    GStatsCntr                  writeHit;
    GStatsCntr                  readMiss;
    GStatsCntr                  writeMiss;
    GStatsCntr                  usefulPrefetch;
    GStatsCntr                  lostPrefetch;
};

class PrivateCaches {
public:
    PrivateCaches(const char *section, size_t n);
    ~PrivateCaches();
    bool doLoad(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted);
    bool doStore(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted);
    void startTransaction(Pid_t pid) { caches.at(pid)->startTransaction();}
    void stopTransaction(Pid_t pid) { caches.at(pid)->stopTransaction(); }

private:
    const size_t                   nCores;
    std::vector<PrivateCache*>     caches;
};

class MyPrefetcher {
public:
    virtual ~MyPrefetcher() {}
    virtual VAddr getAddr(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr) = 0;
    virtual void update(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr) = 0;
};

class MyNextLinePrefetcher: public MyPrefetcher {
public:
    VAddr getAddr(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr);
    void update(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr);
};

class MyStridePrefetcher: public MyPrefetcher {
public:
    VAddr getAddr(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr);
    void update(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr);
private:
    std::map<VAddr, int32_t>    prevTable;
    std::map<VAddr, int32_t>    strideTable;
};

class MyMarkovPrefetcher: public MyPrefetcher {
public:
    MyMarkovPrefetcher(size_t s): size(s), prevAddress(0) {}
    VAddr getAddr(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr);
    void update(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr);
private:
    const size_t                        size;
    VAddr                               prevAddress;
    std::map<VAddr, std::list<VAddr>>   corrTable;
};

extern PrivateCaches* privateCacheManager;

#endif
