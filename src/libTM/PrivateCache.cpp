#include <cmath>
#include <algorithm>
#include "libsuc/nanassert.h"
#include "SescConf.h"
#include "libll/Instruction.h"
#include "PrivateCache.h"

using namespace std;

/*********************************************************
 *  TMLine
 *********************************************************/
void TMLine::addReader(Pid_t reader) {
    if(!transactional) {
        fail("A non-TM line should not add reader\n");
    }
    if(tmWriter != INVALID_PID && !isReader(reader)) {
        fail("A written line should be cleaned first\n");
    }
    tmReaders.insert(reader);
}
void TMLine::makeDirty() {
    if(transactional) {
        fail("A transactional line should use transactionalDirty\n");
    }
    dirty = true;
}
void TMLine::makeTransactionalDirty(Pid_t writer) {
    if(!transactional) {
        fail("A non-TM line should use dirty\n");
    }
    if(tmWriter != INVALID_PID && !isWriter(writer)) {
        fail("Cannot have multiple writers\n");
    }
    
    tmReaders.insert(writer);
    tmWriter = writer;
    dirty = true;
}
void TMLine::makeClean() {
    tmWriter = INVALID_PID;
    dirty = false;
}
void TMLine::clearTransactional() {
    tmReaders.clear();
    tmWriter = INVALID_PID;
    transactional = false;
}
void TMLine::clearTransactional(Pid_t p) {
    tmReaders.erase(p);
    if(isWriter(p)) {
        tmWriter = INVALID_PID;
        dirty = false;
    }
    if(tmReaders.empty() && tmWriter == INVALID_PID) {
        transactional = false;
    }
}
void TMLine::getAccessors(std::set<Pid_t>& accessors) const {
    if(tmWriter != INVALID_PID) {
        accessors.insert(tmWriter);
    }
    accessors.insert(tmReaders.begin(), tmReaders.end());
}
void TMLine::invalidate() {
    dirty           = false;
    transactional   = false;
    caddr           = 0;
    tmWriter        = INVALID_PID;
    tmReaders.clear();
    StateGeneric::invalidate();
}

/*********************************************************
 *  CacheAssocTM
 *********************************************************/
CacheAssocTM::CacheAssocTM(int32_t s, int32_t a, int32_t b, int32_t u)
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

    mem     = new TMLine [numLines + 1];
    content = new TMLine* [numLines + 1];

    for(uint32_t i = 0; i < numLines; i++) {
        mem[i].initialize(this);
        mem[i].invalidate();
        content[i] = &mem[i];
    }
}

///
// Look up an cache line and return a pointer to that line, or NULL if not found
TMLine *CacheAssocTM::lookupLine(VAddr addr)
{
    VAddr tag = this->calcTag(addr);
    TMLine **theSet = &content[this->calcIndex4Tag(tag)];

    // Check most typical case
    if ((*theSet)->getTag() == tag) {
        return *theSet;
    }

    TMLine **lineHit=0;
    TMLine **setEnd = theSet + assoc;

    // For sure that position 0 is not (short-cut)
    {
        TMLine **l = theSet + 1;
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
// Look up an cache line and return a pointer to that line, or NULL if not found
TMLine *CacheAssocTM::findLine(VAddr addr)
{
    VAddr tag = this->calcTag(addr);
    TMLine **theSet = &content[this->calcIndex4Tag(tag)];

    // Check most typical case
    if ((*theSet)->getTag() == tag) {
        return *theSet;
    }

    TMLine **lineHit=0;
    TMLine **setEnd = theSet + assoc;

    // For sure that position 0 is not (short-cut)
    {
        TMLine **l = theSet + 1;
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
    else
        return *lineHit;
}

///
// Move theTMLine to the MRU position in theSet
void
CacheAssocTM::moveToMRU(TMLine** theSet, TMLine** theTMLine)
{
    TMLine *tmp = *theTMLine;
    {
        TMLine **l = theTMLine;
        while(l > theSet) {
            TMLine **prev = l - 1;
            *l = *prev;;
            l = prev;
        }
        *theSet = tmp;
    }
}

///
// Find and return the oldest invalid block
TMLine
**CacheAssocTM::findInvalid(TMLine **theSet)
{
    TMLine **lineFree = 0;
    TMLine **setEnd = theSet + assoc;

    {
        TMLine **l = setEnd -1;
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
TMLine
**CacheAssocTM::findOldestNonTMClean(TMLine **theSet)
{
    TMLine **lineFree = 0;
    TMLine **setEnd = theSet + assoc;

    {
        TMLine **l = setEnd -1;
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

TMLine
*CacheAssocTM::findLine2Replace(VAddr addr)
{
    VAddr tag    = this->calcTag(addr);
    TMLine **theSet = &content[this->calcIndex4Tag(tag)];

    TMLine *replaced = findLine2Replace(theSet);
    if(replaced == NULL) {
        fail("Replacing line is NULL!\n");
    }

    // Do various checks to see if replaced line is correctly chosen
    if(replaced->isTransactional() && replaced->isDirty() && countLines(theSet, &lineTransactionalDirty) < assoc) {
        fail("Evicted transactional line too early: %d\n", countLines(theSet, &lineTransactionalDirty));
    }

    VAddr replTag = replaced->getTag();
    if(replTag == tag) {
        fail("Replaced line matches tag!\n");
    }

    return replaced;
}

TMLine
*CacheAssocTM::findLine2Replace(TMLine** theSet)
{
    TMLine **lineFree=0; // Order of preference, invalid
    TMLine **setEnd = theSet + assoc;

    lineFree = findInvalid(theSet);

    if (lineFree) {
        return *lineFree;
    }

    if(countLines(theSet, &lineTransactionalDirty) == assoc) {
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

size_t
CacheAssocTM::countLines(TMLine** theSet, lineConditionFunc func) const
{
    TMLine **setEnd = theSet + assoc;

    size_t count = 0;
    {
        TMLine **l = theSet;
        while(l < setEnd) {
            if (func(*l)) {
                count++;
            }
            l++;
        }
    }
    return count;
}

void CacheAssocTM::clearTransactional() {
    for(uint32_t i = 0; i < numLines; i++) {
        mem[i].makeClean();
        mem[i].clearTransactional();
    }
}

