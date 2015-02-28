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
        ,isTransactional(false)
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
            if ((*l)->getTag() == tag) {
                lineHit = l;
                break;
            }
            l++;
        }
    }

    if (lineHit == 0)
        return 0;

    // No matter what is the policy, move lineHit to the *theSet. This
    // increases locality
    moveToMRU(theSet, lineHit);

    return *theSet;
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
// Find and return the oldest block that is clean (may be transactional)
template<class Line, class Addr_t>
Line
**CacheAssocTM<Line, Addr_t>::findOldestClean(Line **theSet)
{
    Line **lineFree = 0;
    Line **setEnd = theSet + assoc;

    {
        Line **l = setEnd -1;
        while(l >= theSet) {
            if (!(*l)->isDirty()) {
                lineFree = l;
                break;
            }

            l--;
        }
    }

    return lineFree;
}

///
// Find and return the oldest block that is non transactional (may be dirty)
template<class Line, class Addr_t>
Line
**CacheAssocTM<Line, Addr_t>::findOldestNonTM(Line **theSet)
{
    Line **lineFree = 0;
    Line **setEnd = theSet + assoc;

    {
        Line **l = setEnd -1;
        while(l >= theSet) {
            if (!(*l)->isTransactional()) {
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
*CacheAssocTM<Line, Addr_t>::findLine2Replace(Addr_t addr)
{
    Addr_t tag    = this->calcTag(addr);
    Line **theSet = &content[this->calcIndex4Tag(tag)];

    Line **lineFree=0; // Order of preference, invalid
    Line **setEnd = theSet + assoc;

    lineFree = findInvalid(theSet);

    if (lineFree) {
        return *lineFree;
    }

    if(isTransactional == false || (countTransactionalDirty(addr) == assoc)) {
        // If not inside a transaction, or if we ran out of non-transactional or clean lines
        // Get the oldest line possible
        lineFree = setEnd-1;
    } else {
        if(countTransactional(addr) < assoc && countDirty(addr) < assoc) {
            lineFree = findOldestNonTM(theSet);
            if(lineFree == 0) {
                lineFree = findOldestClean(theSet);
            }
        } else if(countTransactional(addr) == assoc && countDirty(addr) < assoc) {
            lineFree = findOldestClean(theSet);
        } else if(countTransactional(addr) < assoc && countDirty(addr) == assoc) {
            lineFree = findOldestNonTM(theSet);
        } else {
            fail("Non-accounted for case\n");
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
CacheAssocTM<Line, Addr_t>::countValid(Addr_t addr)
{
    Addr_t tag = this->calcTag(addr);

    Line **theSet = &content[this->calcIndex4Tag(tag)];
    Line **setEnd = theSet + assoc;

    size_t count = 0;
    {
        Line **l = theSet;
        while(l < setEnd) {
            if ((*l)->isValid()) {
                count++;
            }
            l++;
        }
    }
    return count;
}

template<class Line, class Addr_t>
size_t
CacheAssocTM<Line, Addr_t>::countDirty(Addr_t addr)
{
    Addr_t tag = this->calcTag(addr);

    Line **theSet = &content[this->calcIndex4Tag(tag)];
    Line **setEnd = theSet + assoc;

    size_t count = 0;
    {
        Line **l = theSet;
        while(l < setEnd) {
            if ((*l)->isValid() && (*l)->isDirty()) {
                count++;
            }
            l++;
        }
    }
    return count;
}

template<class Line, class Addr_t>
size_t
CacheAssocTM<Line, Addr_t>::countTransactional(Addr_t addr)
{
    Addr_t tag = this->calcTag(addr);

    Line **theSet = &content[this->calcIndex4Tag(tag)];
    Line **setEnd = theSet + assoc;

    size_t count = 0;
    {
        Line **l = theSet;
        while(l < setEnd) {
            if ((*l)->isValid() && (*l)->isTransactional()) {
                count++;
            }
            l++;
        }
    }
    return count;
}

template<class Line, class Addr_t>
size_t
CacheAssocTM<Line, Addr_t>::countTransactionalDirty(Addr_t addr)
{
    Addr_t tag = this->calcTag(addr);

    Line **theSet = &content[this->calcIndex4Tag(tag)];
    Line **setEnd = theSet + assoc;

    size_t count = 0;
    {
        Line **l = theSet;
        while(l < setEnd) {
            if ((*l)->isValid() && (*l)->isTransactional() && (*l)->isDirty()) {
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
PrivateCache::PrivateCache(const char* section, const char* name, Pid_t p)
        : pid(p)
        , readHit("%s_%d:readHit", name, p)
        , writeHit("%s_%d:writeHit", name, p)
        , readMiss("%s_%d:readMiss", name, p)
        , writeMiss("%s_%d:writeMiss", name, p)
{
    const int size = SescConf->getInt(section, "size");
    const int assoc = SescConf->getInt(section, "assoc");
    const int bsize = SescConf->getInt(section, "bsize");
    cache = new CacheAssocTM<CState1, VAddr>(size, assoc, bsize, 1);
}

///
// Destructor for PrivateCache. Delete all allocated members
PrivateCache::~PrivateCache() {
    delete cache;
    cache = 0;
}

///
// Add a line to the private cache of pid, evicting set conflicting lines
// if necessary.
PrivateCache::Line* PrivateCache::doFillLine(VAddr addr, MemOpStatus* p_opStatus) {
    Line*         line  = findLine(addr);
    I(line == NULL);

    // The "tag" contains both the set and the real tag
    VAddr myTag = cache->calcTag(addr);

    // Find line to replace
    Line* replaced  = cache->findLine2Replace(addr);
    if(replaced == nullptr) {
        fail("Replacing line is NULL!\n");
    }

    // Invalidate old line
    if(replaced->isValid()) {
        // Do various checks to see if replaced line is correctly chosen
        if(isInTransaction() && replaced->isTransactional() && cache->countTransactional(addr) < cache->getAssoc()) {
            fail("%d evicted transactional line too early: %d\n", pid, cache->countTransactional(addr));
        }
        if(isInTransaction() && replaced->isTransactional() && replaced->isDirty() && cache->countDirty(addr) < cache->getAssoc()) {
            fail("%d evicted transactional dirty line too early: %d\n", pid, cache->countDirty(addr));
        }

        VAddr replTag = replaced->getTag();
        if(replTag == myTag) {
            fail("Replaced line matches tag!\n");
        }

        // Update MemOpStatus if this is a set conflict
        if(isInTransaction() && replaced->isTransactional() && replaced->isDirty()) {
            p_opStatus->setConflict = true;
        }

        // Invalidate line
        replaced->invalidate();
    }

    // Replace the line
    replaced->setTag(myTag);

    return replaced;
}

PrivateCache::Line* PrivateCache::findLine(VAddr addr) {
    return cache->findLine(addr);
}

void PrivateCache::doLoad(InstDesc* inst, ThreadContext* context, VAddr addr, MemOpStatus* p_opStatus) {
    // Lookup line
    Line*   line  = findLine(addr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        readMiss.inc();
        line = doFillLine(addr, p_opStatus);
    } else {
        p_opStatus->wasHit = true;
        readHit.inc();
    }

    // Update line
    if(isInTransaction()) {
        line->markTransactional();
    }
}

void PrivateCache::doStore(InstDesc* inst, ThreadContext* context, VAddr addr, MemOpStatus* p_opStatus) {
    // Lookup line
    Line*   line  = findLine(addr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        writeMiss.inc();
        line = doFillLine(addr, p_opStatus);
    } else {
        p_opStatus->wasHit = true;
        writeHit.inc();
    }

    // Update line
    if(isInTransaction()) {
        line->markTransactional();
    }
    line->makeDirty();
}


