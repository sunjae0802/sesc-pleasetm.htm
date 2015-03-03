#include <cmath>
#include <algorithm>
#include "libsuc/nanassert.h"
#include "SescConf.h"
#include "libll/Instruction.h"
#include "PrivateCache.h"

using namespace std;

/*********************************************************
 *  CacheAssocTM
 *********************************************************/

template<class Line, class Addr_t>
CacheAssocTM<Line, Addr_t>::CacheAssocTM(int32_t s, int32_t a, int32_t b, int32_t u)
        : size(s)
        ,lineSize(b)
        ,addrUnit(u)
        ,assoc(a)
        ,log2Assoc(log2i(a))
        ,log2AddrLs(log2i(b/u))
        ,maskAssoc(a-1)
        ,sets((s/b)/a)
        ,maskSets(sets-1)
        ,numLines(s/b)
{
    I(numLines>0);

    mem     = new Line [numLines + 1];
    content = new Line* [numLines + 1];

    for(uint32_t i = 0; i < numLines; i++) {
        mem[i].initialize(this);
        mem[i].invalidate();
        content[i] = &mem[i];
    }
}

///
// Look up an cache line and return a pointer to that line, or NULL if not found
template<class Line, class Addr_t>
Line *CacheAssocTM<Line, Addr_t>::lookupLine(Addr_t addr)
{
    Addr_t tag = this->calcTag(addr);
    Line **theSet = &content[this->calcIndex4Tag(tag)];

    // Check most typical case
    if ((*theSet)->getTag() == tag) {
        return *theSet;
    }

    Line **lineHit=0;
    Line **setEnd = theSet + assoc;

    // For sure that position 0 is not (short-cut)
    {
        Line **l = theSet + 1;
        while(l < setEnd) {
            if ((*l)->getTag() == tag) {
                lineHit = l;
                break;
            }
            l++;
        }
    }
    Line* line = findLine(addr);

    if (lineHit == 0)
        return 0;

    // No matter what is the policy, move lineHit to the *theSet. This
    // increases locality
    moveToMRU(theSet, lineHit);

    return *theSet;
}

///
// Look up an cache line and return a pointer to that line, or NULL if not found
template<class Line, class Addr_t>
Line *CacheAssocTM<Line, Addr_t>::findLine(Addr_t addr)
{
    Addr_t tag = this->calcTag(addr);
    Line **theSet = &content[this->calcIndex4Tag(tag)];

    // Check most typical case
    if ((*theSet)->getTag() == tag) {
        return *theSet;
    }

    Line **lineHit=0;
    Line **setEnd = theSet + assoc;

    // For sure that position 0 is not (short-cut)
    {
        Line **l = theSet + 1;
        while(l < setEnd) {
            if ((*theSet)->getTag() == tag) {
                lineHit = l;
                break;
            }
            l++;
        }
    }

    if (lineHit == 0)
        return 0;
    else
        return *lineHit;
}

///
// Move theLine to the MRU position in theSet
template<class Line, class Addr_t>
void
CacheAssocTM<Line, Addr_t>::moveToMRU(Line** theSet, Line** theLine)
{
    Line *tmp = *theLine;
    {
        Line **l = theLine;
        while(l > theSet) {
            Line **prev = l - 1;
            *l = *prev;;
            l = prev;
        }
        *theSet = tmp;
    }
}

///
// Find and return the oldest invalid block
template<class Line, class Addr_t>
Line
**CacheAssocTM<Line, Addr_t>::findInvalid(Line **theSet)
{
    Line **lineFree = 0;
    Line **setEnd = theSet + assoc;

    {
        Line **l = setEnd -1;
        while(l >= theSet) {
            if (!(*l)->isValid()) {
                lineFree = l;
                break;
            }
            l--;
        }
    }

    return lineFree;
}
///
// Find and return the oldest block that is clean or non-transactional
template<class Line, class Addr_t>
Line
**CacheAssocTM<Line, Addr_t>::findOldestNonTMClean(Line **theSet)
{
    Line **lineFree = 0;
    Line **setEnd = theSet + assoc;

    {
        Line **l = setEnd -1;
        while(l >= theSet) {
            if (!(*l)->isDirty() || !(*l)->isTransactional()) {
                lineFree = l;
                break;
            }

            l--;
        }
    }

    return lineFree;
}

template<class Line, class Addr_t>
Line
*CacheAssocTM<Line, Addr_t>::findLine2Replace(bool isInTM, Addr_t addr)
{
    Addr_t tag    = this->calcTag(addr);
    Line **theSet = &content[this->calcIndex4Tag(tag)];

    Line *replaced = findLine2Replace(isInTM, theSet);

    // Do various checks to see if replaced line is correctly chosen
    if(isInTM && replaced->isTransactional() && replaced->isDirty() && countLines(theSet, &lineTransactionalDirty) < assoc) {
        fail("Evicted transactional line too early: %d\n", countLines(theSet, &lineTransactionalDirty));
    }

    VAddr replTag = replaced->getTag();
    if(replTag == tag) {
        fail("Replaced line matches tag!\n");
    }

    return replaced;
}

template<class Line, class Addr_t>
Line
*CacheAssocTM<Line, Addr_t>::findLine2Replace(bool isInTM, Line** theSet)
{
    Line **lineFree=0; // Order of preference, invalid
    Line **setEnd = theSet + assoc;

    lineFree = findInvalid(theSet);

    if (lineFree) {
        return *lineFree;
    }

    if(isInTM == false || (countLines(theSet, &lineTransactionalDirty) == assoc)) {
        // If not inside a transaction, or if we ran out of non-transactional or clean lines
        // Get the oldest line possible
        lineFree = setEnd-1;
    } else {
        lineFree = findOldestNonTMClean(theSet);
        if(lineFree && (*lineFree)->isDirty() && (*lineFree)->isTransactional()) {
            fail("Replacing transactional dirty line");
        }
    }
    if(lineFree == 0) {
        fail("Replacement policy failed\n");
    }

    if (lineFree == theSet) {
        return *lineFree; // Hit in the first possition
    }

    // No matter what is the policy, move lineHit to the *theSet. This
    // increases locality
    moveToMRU(theSet, lineFree);

    return *theSet;
}

template<class Line, class Addr_t>
size_t
CacheAssocTM<Line, Addr_t>::countLines(Line** theSet, lineConditionFunc func) const
{
    Line **setEnd = theSet + assoc;

    size_t count = 0;
    {
        Line **l = theSet;
        while(l < setEnd) {
            if (func(*l)) {
                count++;
            }
            l++;
        }
    }
    return count;
}

/*********************************************************
 *  PrivateCache
 *********************************************************/
///
// Constructor for PrivateCache. Allocate members and GStat counters
PrivateCache::PrivateCache(const char* section, int nProcs)
{
    const int size = SescConf->getInt(section, "size");
    const int assoc = SescConf->getInt(section, "assoc");
    const int bsize = SescConf->getInt(section, "bsize");

    for(Pid_t pid = 0; pid < nProcs; pid++) {
        caches.push_back(new CacheAssocTM<CState1, VAddr>(size, assoc, bsize, 1));
    }
}

///
// Destructor for PrivateCache. Delete all allocated members
PrivateCache::~PrivateCache() {
    for(size_t cid = caches.size() - 1; cid >= 0; cid--) {
        Cache* cache = caches.back();
        caches.pop_back();
        delete cache;
    }
}

///
// Add a line to the private cache of pid, evicting set conflicting lines
// if necessary.
PrivateCache::Line* PrivateCache::doFillLine(Pid_t pid, bool isInTM, VAddr addr, MemOpStatus* p_opStatus) {
    Cache* cache = caches.at(pid);

    // The "tag" contains both the set and the real tag
    VAddr myTag = cache->calcTag(addr);

    // Find line to replace
    Line* replaced  = cache->findLine2Replace(isInTM, addr);
    if(replaced == nullptr) {
        fail("Replacing line is NULL!\n");
    }

    // Invalidate old line
    if(replaced->isValid()) {
        // Update MemOpStatus if this is a set conflict
        if(isInTM && replaced->isTransactional() && replaced->isDirty()) {
            p_opStatus->setConflict = true;
        }

        // Invalidate line
        replaced->invalidate();
    }

    // Replace the line
    replaced->setTag(myTag);

    return replaced;
}

PrivateCache::Line* PrivateCache::findLine(Pid_t pid, VAddr addr) {
    Cache* cache = caches.at(pid);
    return cache->findLine(addr);
}

void PrivateCache::doLoad(InstDesc* inst, ThreadContext* context, VAddr addr, MemOpStatus* p_opStatus) {
    Pid_t pid = context->getPid();
    Cache* cache = caches.at(pid);
    bool isInTM = context->isInTM();

    // Lookup line
    Line*   line  = cache->lookupLine(addr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        line = doFillLine(pid, isInTM, addr, p_opStatus);
    } else {
        p_opStatus->wasHit = true;
    }

    // Update line
    if(isInTM) {
        line->markTransactional();
    }
}

void PrivateCache::doStore(InstDesc* inst, ThreadContext* context, VAddr addr, MemOpStatus* p_opStatus) {
    Pid_t pid = context->getPid();
    Cache* cache = caches.at(pid);
    bool isInTM = context->isInTM();

    // Lookup line
    Line*   line  = cache->lookupLine(addr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        line = doFillLine(pid, isInTM, addr, p_opStatus);
    } else {
        p_opStatus->wasHit = true;
    }

    // Update line
    if(isInTM) {
        line->markTransactional();
    }
    line->makeDirty();
}

void PrivateCache::clearTransactional(Pid_t pid) {
    Cache* cache = caches.at(pid);
    cache->clearTransactional();
}
