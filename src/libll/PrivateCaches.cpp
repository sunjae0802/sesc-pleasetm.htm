#include <iostream>
#include <string>
#include <utility>
#include "PrivateCaches.h"
#include "libll/ThreadContext.h"
#include "libll/Instruction.h"
#include "libTM/TMCoherence.h"
#include "libcore/ProcessId.h"

using namespace std;

PrivateCaches* privateCacheManager = 0;

PrivateCaches::PrivateCaches(const char *section, size_t n)
        : nCores(n)
{
    for(Pid_t p = 0; p < (Pid_t)nCores; ++p) {
        caches.push_back(new PrivateCache(section, "privateCache", p));
    }
}
PrivateCaches::~PrivateCaches()
{
    while(caches.size() > 0) {
        PrivateCache* cache = caches.back();
        caches.pop_back();
        delete cache;
    }
}

///
// Do a load operation, bringing the line into pid's private cache
bool PrivateCaches::doLoad(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted) {
    Pid_t pid = context->getPid();
    PrivateCache* cache = caches.at(pid);

    bool wasHit = cache->doLoad(inst, context, addr, tmEvicted);
    if(wasHit == false) {
        cache->doPrefetches(inst, context, addr, tmEvicted);
    } else {
        cache->updatePrefetchers(inst, context, addr, tmEvicted);
    }
    return wasHit;
}

///
// Do a store operation, invalidating all sharers and bringing the line into
// pid's private cache
bool PrivateCaches::doStore(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted) {
    Pid_t pid = context->getPid();

    // Invalidate all sharers
    for(Pid_t p = 0; p < (Pid_t)nCores; ++p) {
        if(p != pid) {
            caches.at(p)->doInvalidate(inst, context, addr, tmEvicted);
        }
    }

    PrivateCache* cache = caches.at(pid);
    bool wasHit = cache->doStore(inst, context, addr, tmEvicted);
    if(wasHit == false) {
        cache->doPrefetches(inst, context, addr, tmEvicted);
    } else {
        cache->updatePrefetchers(inst, context, addr, tmEvicted);
    }

    return wasHit;
}

///
// Constructor for PrivateCache. Allocate members and GStat counters
PrivateCache::PrivateCache(const char* section, const char* name, Pid_t p)
        : pid(p)
        , isTransactional(false)
        , readHit("%s_%d:readHit", name, p)
        , writeHit("%s_%d:writeHit", name, p)
        , readMiss("%s_%d:readMiss", name, p)
        , writeMiss("%s_%d:writeMiss", name, p)
        , usefulPrefetch("%s_%d:usefulPrefetch", name, p)
        , lostPrefetch("%s_%d:lostPrefetch", name, p)
{
    const int size = SescConf->getInt(section, "size");
    const int assoc = SescConf->getInt(section, "assoc");
    const int bsize = SescConf->getInt(section, "bsize");
    cache = new CacheAssocTM<CState1, VAddr>(size, assoc, bsize, 1, "LRU");

    if(SescConf->checkCharPtr(section, "prefetchers")) {
        string prefetchers_str = SescConf->getCharPtr(section, "prefetchers");
        size_t start = 0;
        size_t end = 0;
        do {
            string prefType;
            end = prefetchers_str.find(' ', start);
            if(end == string::npos) {
                prefType = prefetchers_str.substr(start, string::npos);
            } else {
                size_t len = end - start;
                prefType = prefetchers_str.substr(start, len);
                start = end + 1;
            }

            if(prefType == "NextLine") {
                prefetchers.push_back(new MyNextLinePrefetcher);
            } else if(prefType == "Stride") {
                prefetchers.push_back(new MyStridePrefetcher);
            } else if(prefType.size() > 0 && prefType.at(0) != ' ') {
                MSG("Unknown prefetcher type: %s", prefType.c_str());
            }
        } while(end != string::npos);
    }
}

///
// Destructor for PrivateCache. Delete all allocated members
PrivateCache::~PrivateCache() {
    cache->destroy();
    cache = 0;

    while(prefetchers.size() > 0) {
        MyPrefetcher* prefetcher = prefetchers.back();
        prefetchers.pop_back();
        delete prefetcher;
    }
}

///
// Add a line to the private cache of pid, evicting set conflicting lines
// if necessary.
PrivateCache::Line* PrivateCache::doFillLine(VAddr addr, bool isPrefetch, std::map<Pid_t, EvictCause>& tmEvicted) {
    Line*         line  = cache->findLineNoEffect(addr);
    I(line == NULL);

    // The "tag" contains both the set and the real tag
    VAddr myTag = cache->calcTag(addr);

    // Find line to replace
    Line* replaced  = cache->findLine2Replace(addr, true, isTransactional);
    if(replaced == nullptr) {
        fail("Replacing line is NULL!\n");
    }

    if(isTransactional && replaced->isTransactional() && cache->countTransactional(addr) < cache->getAssoc()) {
        fail("%d evicted transactional line to early: %d\n", pid, cache->countTransactional(addr));
    }

    // Invalidate old line
    if(replaced->isValid()) {
        VAddr replTag = replaced->getTag();
        if(replTag == myTag) {
            fail("Replaced line matches tag!\n");
        }
        if(replaced->wasPrefetch()) {
            lostPrefetch.inc();
        }

        if(isTransactional && replaced->isTransactional() && replaced->isDirty()) {
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

    return replaced;
}

///
// Loop through each prefetcher and try each
void PrivateCache::doPrefetches(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted) {
    std::vector<MyPrefetcher*>::iterator i_prefetcher;
    for(i_prefetcher = prefetchers.begin(); i_prefetcher != prefetchers.end(); ++i_prefetcher) {
        // Access the prefetcher
        VAddr prefetchAddr = (*i_prefetcher)->getAddr(inst, context, this, addr);
        (*i_prefetcher)->update(inst, context, this, addr);

        // If we get a valid prefetch address
        if(prefetchAddr) {
            Line* prefetch = cache->findLineNoEffect(prefetchAddr);
            if(prefetch == nullptr) {
                Line* prefetch = doFillLine(prefetchAddr, true, tmEvicted);
                prefetch->markPrefetch();
            } else {
                // Prefetch target already in the cache
            }
        }
    }
}
void PrivateCache::updatePrefetchers(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted) {
    // Loop through each prefetcher and try each
    std::vector<MyPrefetcher*>::iterator i_prefetcher;
    for(i_prefetcher = prefetchers.begin(); i_prefetcher != prefetchers.end(); ++i_prefetcher) {
        (*i_prefetcher)->update(inst, context, this, addr);
    }
}



bool PrivateCache::doLoad(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted) {
    // Lookup line
    bool        wasHit = true;
    Line*       line  = cache->findLineNoEffect(addr);
    if(line == nullptr) {
        wasHit = false;
        readMiss.inc();
        line = doFillLine(addr, false, tmEvicted);
    } else {
        readHit.inc();
    }

    // Update line
    if(isTransactional) {
        line->markTransactional();
    }
    if(line->wasPrefetch()) {
        usefulPrefetch.inc();
        line->clearPrefetch();
    }
    return wasHit;
}

void PrivateCache::doInvalidate(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted) {
    // Lookup line
    Line* line = cache->findLineNoEffect(addr);
    if(line) {
        if(isTransactional && line->isTransactional()) {
            tmEvicted.insert(make_pair(pid, EvictByWrite));
        }
        line->invalidate();
    }
}

bool PrivateCache::doStore(InstDesc* inst, ThreadContext* context, VAddr addr, std::map<Pid_t, EvictCause>& tmEvicted) {
    // Lookup line
    bool    wasHit = true;
    Line*   line  = cache->findLineNoEffect(addr);
    if(line == nullptr) {
        wasHit = false;
        writeMiss.inc();
        line = doFillLine(addr, false, tmEvicted);
    } else {
        writeHit.inc();
    }

    // Update line
    if(isTransactional) {
        line->markTransactional();
    }
    line->makeDirty();
    if(line->wasPrefetch()) {
        usefulPrefetch.inc();
        line->clearPrefetch();
    }
    return wasHit;
}

VAddr MyNextLinePrefetcher::getAddr(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr) {
    VAddr nextAddr = addr + cache->getLineSize();
    return nextAddr;
}
void MyNextLinePrefetcher::update(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr) {
}

VAddr MyStridePrefetcher::getAddr(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr) {
    int32_t pcValue = inst->getSescInst()->getAddr();
    VAddr prev = prevTable[pcValue];
    int32_t stride = strideTable[pcValue];

    VAddr nextAddr = 0;
    if(prev + stride == addr) {
        nextAddr = addr + stride;
    }

    return nextAddr;
}
void MyStridePrefetcher::update(InstDesc* inst, ThreadContext* context, PrivateCache* cache, VAddr addr) {
    int32_t pcValue = inst->getSescInst()->getAddr();
    VAddr prev = prevTable[pcValue];

    prevTable[pcValue] = addr;
    strideTable[pcValue] = addr - prev;
}
