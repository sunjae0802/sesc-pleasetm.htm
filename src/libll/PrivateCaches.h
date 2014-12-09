#ifndef PRIVATECACHES_H
#define PRIVATECACHES_H

#include <vector>
#include <set>
#include <map>
#include "libemul/AddressSpace.h"
#include "CacheCore.h"

class CState1 : public StateGeneric<> {
private:
    bool valid;
    bool dirty;
public:
    CState1() {
        valid = false;
        dirty = false;
        clearTag();
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
        clearTag();
    }
};

class PrivateCaches {
public:
    PrivateCaches(const char *section, size_t n);
    ~PrivateCaches();
    bool doLoad(Pid_t pid, VAddr addr);
    bool doStore(Pid_t pid, VAddr addr);
    bool findLine(Pid_t pid, VAddr addr);
private:
    bool doFillLine(Pid_t pid, VAddr addr, bool isWrite);

    typedef CacheGeneric<CState1, VAddr>            PrivateCache;
    typedef CacheGeneric<CState1, VAddr>::CacheLine Line;

    const size_t                        nCores;
    std::vector<PrivateCache*>          caches;
    std::map<Pid_t, std::set<VAddr> >   activeAddrs;
};

extern PrivateCaches* privateCacheManager;

#endif
