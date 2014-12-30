#include <utility>
#include "PrivateCaches.h"
#include "libll/ThreadContext.h"
#include "libTM/TMCoherence.h"
#include "libcore/ProcessId.h"

using namespace std;

PrivateCaches* privateCacheManager = 0;

PrivateCaches::PrivateCaches(const char *section, size_t n): nCores(n) {
    for(Pid_t p = 0; p < (Pid_t)nCores; ++p) {
        PrivateCache* cache = PrivateCache::create(32*1024,8,64,1,"LRU",false);
        caches.push_back(cache);
    }
    nextLinePrefetch = true;
}

PrivateCaches::~PrivateCaches() {
    for(Pid_t p = 0; p < (Pid_t)nCores; ++p) {
        PrivateCache* cache = caches.back();
        caches.pop_back();
        cache->destroy();
    }
}

///
// Return true if the line is found within pid's private cache
bool PrivateCaches::findLine(Pid_t pid, VAddr addr) {
    PrivateCache* cache = caches.at(pid);
    Line*         line  = cache->findLineNoEffect(addr);

    return line != nullptr;
}

///
// Add a line to the private cache of pid, evicting set conflicting lines
// if necessary.
PrivateCaches::Line* PrivateCaches::doFillLine(Pid_t pid, VAddr addr, bool isTransactional, bool isPrefetch, std::map<Pid_t, EvictCause>& tmEvicted) {
    PrivateCache* cache = caches.at(pid);
    Line*         line  = cache->findLineNoEffect(addr);
    I(line == NULL);

    // The "tag" contains both the set and the real tag
    VAddr myTag = cache->calcTag(addr);
    if(activeAddrs[pid].find(myTag) != activeAddrs[pid].end()) {
        fail("Trying to add tag already there!\n");
    }

    // Find line to replace
    Line* replaced  = cache->findLine2Replace(addr, true, isTransactional);
    if(replaced == nullptr) {
        fail("Replacing line is NULL!\n");
    }

    if(isTransactional && replaced->isTransactional() && cache->countValid(addr) < cache->getAssoc()) {
        fail("Evicted transactional line: \n");
    }

    // Invalidate old line
    if(replaced->isValid()) {
        VAddr replTag = replaced->getTag();
        if(replTag == myTag) {
            fail("Replaced line matches tag!\n");
        }
        if(activeAddrs[pid].find(replTag) == activeAddrs[pid].end()) {
            fail("Replacing line not there anymore!\n");
        }
        activeAddrs[pid].erase(replTag);
        if(isTransactional && replaced->isTransactional()) {
            if(isPrefetch) {
                tmEvicted.insert(make_pair(pid, EvictPrefetch));
            } else {
                tmEvicted.insert(make_pair(pid, EvictSetConflict));
            }
        }

        replaced->invalidate();
    }

    // Replace the line
    replaced->setTag(myTag);
    replaced->validate();

    activeAddrs[pid].insert(myTag);

    return replaced;
}
void PrivateCaches::doNextLinePrefetch(Pid_t pid, VAddr addr, bool isTransactional, std::map<Pid_t, EvictCause>& tmEvicted) {
    PrivateCache* cache = caches.at(pid);
    VAddr nextAddr = addr + cache->getLineSize();

    Line*   next = cache->findLineNoEffect(nextAddr);
    if(next == nullptr) {
        next = doFillLine(pid, nextAddr, isTransactional, true, tmEvicted);
        next->markPrefetch();
    }
}
#if 0
void PrivateCaches::doStridePrefetch(Pid_t pid, VAddr addr, bool isTransactional, std::map<Pid_t, EvictCause>& tmEvicted) {
    VAddr prev = prev_table[pc_value];
    int32_t stride = stride_table[pc_value];

    if(prev + stride == addr) {
        Line*   next = cache->findLineNoEffect(addr + stride);
        if(next == nullptr) {
            next = doFillLine(pid, nextAddr, isTransactional, true, tmEvicted);
        }
        next->markPrefetch();
    } 
    prev_table[pc_value] = iAddr;
    stride[pc_value] = addr - prev;
}
#endif

///
// Do a load operation, bringing the line into pid's private cache
bool PrivateCaches::doLoad(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted) {
    Pid_t pid = context->getPid();
    bool isTransactional = context->isInTM();
    bool wasHit = true;
    PrivateCache* cache = caches.at(pid);
    Line*         line  = cache->findLineNoEffect(addr);
    if(line == nullptr) {
        wasHit = false;
        line = doFillLine(pid, addr, isTransactional, false, tmEvicted);

        if(nextLinePrefetch) {
            doNextLinePrefetch(pid, addr, isTransactional, tmEvicted);
        }
    } else if(activeAddrs[pid].find(line->getTag()) == activeAddrs[pid].end()) {
        fail("Found line not properly tracked by activeAddrs!\n");
    }

    // Update line
    if(isTransactional) {
        line->markTransactional();
    }
    line->clearPrefetch();
    return wasHit;
}
///
// Do a store operation, invalidating all sharers and bringing the line into
// pid's private cache
bool PrivateCaches::doStore(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted) {
    Pid_t pid = context->getPid();
    bool isTransactional = context->isInTM();

    // Invalidate all sharers
    for(Pid_t p = 0; p < (Pid_t)nCores; ++p) {
        if(p != pid) {
            Line* line = caches.at(p)->findLineNoEffect(addr);
            if(line) {
                activeAddrs[p].erase(line->getTag());
                if(isTransactional && line->isTransactional()) {
                    tmEvicted.insert(make_pair(p, EvictByWrite));
                }
                line->invalidate();
            }
        }
    }

    // Add myself
    bool wasHit = true;
    PrivateCache* cache = caches.at(pid);
    Line*         line  = cache->findLineNoEffect(addr);
    if(line == nullptr) {
        wasHit = false;
        line = doFillLine(pid, addr, isTransactional, false, tmEvicted);

        if(nextLinePrefetch) {
            doNextLinePrefetch(pid, addr, isTransactional, tmEvicted);
        }
    } else if(activeAddrs[pid].find(line->getTag()) == activeAddrs[pid].end()) {
        fail("Found line not properly tracked by activeAddrs!\n");
    }

    // Update line
    if(isTransactional) {
        line->markTransactional();
    }
    line->makeDirty();
    line->clearPrefetch();

    return wasHit;
}

