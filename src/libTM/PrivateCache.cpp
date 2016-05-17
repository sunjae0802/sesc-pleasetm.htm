#include <cmath>
#include <algorithm>
#include "libsuc/nanassert.h"
#include "libemul/EmulInit.h"
#include "SescConf.h"
#include "libll/Instruction.h"
#include "PrivateCache.h"

using namespace std;

/*********************************************************
 *  TMLine
 *********************************************************/
void TMLine::addReader(Pid_t reader) {
    if(isValid() == false) {
        fail("trying to add reader to invalid line\n");
    }
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
    if(isValid() == false) {
        fail("trying to mark TMDirty to invalid line\n");
    }
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
void TMLine::validate(VAddr t, VAddr c) {
    if(isValid()) {
        fail("Line should be invalidated before validating: 0x%lx", caddr);
    }
    if(t == 0) {
        fail("New tag should not be null\n");
    }
    if(c == 0) {
        fail("New caddr should not be null\n");
    }
    setTag(t);
    caddr = c;
}
void TMLine::invalidate() {
    dirty           = false;
    transactional   = false;
    caddr           = INVALID_CADDR;
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

    if(tag == 0) {
        fail("Cannot lookup null: 0x%lx\n", addr);
    }

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

    if(tag == 0) {
        fail("Cannot find null: 0x%lx\n", addr);
    }

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
// Search through the set and find the oldest line satisfying comp
TMLine
**CacheAssocTM::findOldestLine(TMLine **theSet, const LineComparator& comp)
{
    TMLine **lineFree = 0;
    TMLine **setEnd = theSet + assoc;

    {
        TMLine **l = setEnd -1;
        while(l >= theSet) {
            if (comp(*l)) {
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

    VAddr replTag = replaced->getTag();
    if(replTag == tag) {
        fail("Replaced line matches tag!\n");
    }

    return replaced;
}

TMLine
*CacheAssocTM::findLine2Replace(TMLine** theSet)
{
    TMLine **line2Replace=0;
    TMLine **setEnd = theSet + assoc;

    // Find oldest invalid line
    LineInvalidComparator invalCmp;
    line2Replace = findOldestLine(theSet, invalCmp);

    if(line2Replace == nullptr) {
        // Or give up and return the oldest line
        line2Replace = setEnd-1;
    }

    // No matter what is the policy, move line2Replace to the *theSet. This
    // increases locality
    if (line2Replace == theSet) {
        return *line2Replace; // Hit in the first position
    } else {
        moveToMRU(theSet, line2Replace);
        return *theSet;
    }
}

///
// Search through the set and count lines that satisfy comp.
size_t
CacheAssocTM::countLines(VAddr addr, const LineComparator& comp) const
{
    VAddr tag    = this->calcTag(addr);
    TMLine **theSet = &content[this->calcIndex4Tag(tag)];

    return countLines(theSet, comp);
}

///
// Search through the set and count lines that satisfy comp. Private version
// that accepts theSet as the argument.
size_t
CacheAssocTM::countLines(TMLine **theSet, const LineComparator& comp) const
{
    TMLine **setEnd = theSet + assoc;

    size_t count = 0;
    {
        TMLine **l = theSet;
        while(l < setEnd) {
            if (comp(*l)) {
                count++;
            }
            l++;
        }
    }
    return count;
}

///
// Collect all transactional lines in the core.
void CacheAssocTM::collectLines(std::vector<TMLine*>& lines, const LineComparator& comp) {
    for(uint32_t i = 0; i < numLines; i++) {
        TMLine* line = content[i];
        if (comp(line)) {
            lines.push_back(line);
        }
    }
}


