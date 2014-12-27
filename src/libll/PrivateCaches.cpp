#include "PrivateCaches.h"
#include "libcore/ProcessId.h"

PrivateCaches* privateCacheManager = 0;

PrivateCaches::PrivateCaches(const char *section, size_t n): nCores(n) {
    for(Pid_t p = 0; p < (Pid_t)nCores; ++p) {
        PrivateCache* cache = PrivateCache::create(32*1024,8,64,1,"LRU",false);
        caches.push_back(cache);
    }
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
PrivateCaches::Line* PrivateCaches::doFillLine(Pid_t pid, VAddr addr, bool isTransactional, std::set<Pid_t>& tmEvicted) {
    PrivateCache* cache = caches.at(pid);
    Line*         line  = cache->findLineNoEffect(addr);
    I(line == NULL);

    // do next line prefetch
    Line*   next = cache->findLineNoEffect(addr+cache->getLineSize());
    if(next == nullptr) {
    }

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
            tmEvicted.insert(pid);
        }

        replaced->invalidate();
    }

    // Replace the line
    replaced->setTag(myTag);
    replaced->validate();

    activeAddrs[pid].insert(myTag);

    return replaced;
}
///
// Do a load operation, bringing the line into pid's private cache
bool PrivateCaches::doLoad(Pid_t pid, VAddr addr, bool isTransactional, std::set<Pid_t>& tmEvicted) {
    bool wasHit = true;
    PrivateCache* cache = caches.at(pid);
    Line*         line  = cache->findLineNoEffect(addr);
    if(line == nullptr) {
        wasHit = false;
        line = doFillLine(pid, addr, isTransactional, tmEvicted);
    } else if(activeAddrs[pid].find(line->getTag()) == activeAddrs[pid].end()) {
        fail("Found line not properly tracked by activeAddrs!\n");
    }

    // Update line
    if(isTransactional) {
        line->markTransactional();
    }
    return wasHit;
}
///
// Do a store operation, invalidating all sharers and bringing the line into
// pid's private cache
bool PrivateCaches::doStore(Pid_t pid, VAddr addr, bool isTransactional, std::set<Pid_t>& tmEvicted) {
    // Invalidate all sharers
    for(Pid_t p = 0; p < (Pid_t)nCores; ++p) {
        if(p != pid) {
            Line* line = caches.at(p)->findLineNoEffect(addr);
            if(line) {
                activeAddrs[p].erase(line->getTag());
                if(isTransactional && line->isTransactional()) {
                    tmEvicted.insert(p);
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
        line = doFillLine(pid, addr, isTransactional, tmEvicted);
    } else if(activeAddrs[pid].find(line->getTag()) == activeAddrs[pid].end()) {
        fail("Found line not properly tracked by activeAddrs!\n");
    }

    // Update line
    if(isTransactional) {
        line->markTransactional();
    }
    line->makeDirty();

    return wasHit;
}

