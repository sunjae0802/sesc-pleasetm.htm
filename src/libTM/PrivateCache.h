#ifndef PRIVATE_CACHE
#define PRIVATE_CACHE

#include "libemul/InstDesc.h"
#include "libsuc/CacheCore.h"

enum EvictCause {
    NoEvict = 0,
    EvictByWrite,
    EvictSetConflict,
};

struct MemOpStatus {
    bool wasHit;
    bool setConflict;
    MemOpStatus(): wasHit(false), setConflict(false) {}
};

class CState1 : public StateGeneric<> {
private:
    bool dirty;
    bool transactional;
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
    bool isTransactional() const {
        return transactional;
    }
    void markTransactional() {
        transactional = true;
    }
    void clearTransactional() {
        transactional = false;
    }
    void invalidate() {
        dirty = false;
        transactional = false;
        clearTag();
    }
};

template<class Line, class Addr_t>
class CacheAssocTM {
    const uint32_t  size;
    const uint32_t  lineSize;
    const uint32_t  addrUnit; //Addressable unit: for most caches = 1 byte
    const uint32_t  assoc;
    const uint32_t  log2Assoc;
    const uint64_t  log2AddrLs;
    const uint64_t  maskAssoc;
    const uint32_t  sets;
    const uint32_t  maskSets;
    const uint32_t  numLines;

protected:

    Line *mem;
    Line **content;
    void moveToMRU(Line** theSet, Line** theLine);
    Line *findLine2Replace(bool isInTM, Line** theSet);
    Line **findInvalid(Line **theSet);
    Line **findOldestNonTMClean(Line **theSet);
    Line **findOldestClean(Line **theSet);
    Line **findOldestNonTM(Line **theSet);

    typedef bool (*lineConditionFunc)(Line *l);
    static bool lineValid(Line *l) { return l->isValid(); }
    static bool lineDirty(Line *l) { return l->isDirty(); }
    static bool lineTransactional(Line *l) { return l->isTransactional(); }
    static bool lineTransactionalDirty(Line *l) { return l->isTransactional() && l->isDirty(); }
    size_t countLines(Line **theSet, lineConditionFunc func) const;

public:
    CacheAssocTM(int32_t size, int32_t assoc, int32_t blksize, int32_t addrUnit);
    virtual ~CacheAssocTM() {
        delete [] content;
        delete [] mem;
    }

    Line *findLine2Replace(bool isInTM, Addr_t addr);
    Line *lookupLine(Addr_t addr);
    Line *findLine(Addr_t addr);

    void clearTransactional();

    uint32_t  getLineSize() const   {
        return lineSize;
    }
    uint32_t  getAssoc() const      {
        return assoc;
    }
    uint32_t  getLog2AddrLs() const {
        return log2AddrLs;
    }
    uint32_t  getLog2Assoc() const  {
        return log2Assoc;
    }
    uint32_t  getMaskSets() const   {
        return maskSets;
    }
    uint32_t  getNumLines() const   {
        return numLines;
    }
    uint32_t  getNumSets() const    {
        return sets;
    }

    Addr_t calcTag(Addr_t addr)       const {
        return (addr >> log2AddrLs);
    }

    uint32_t calcSet4Tag(Addr_t tag)     const {
        return (tag & maskSets);
    }
    uint32_t calcSet4Addr(Addr_t addr)   const {
        return calcSet4Tag(calcTag(addr));
    }

    uint32_t calcIndex4Set(uint32_t set)    const {
        return (set << log2Assoc);
    }
    uint32_t calcIndex4Tag(uint32_t tag)    const {
        return calcIndex4Set(calcSet4Tag(tag));
    }
    uint32_t calcIndex4Addr(Addr_t addr) const {
        return calcIndex4Set(calcSet4Addr(addr));
    }

    Addr_t calcAddr4Tag(Addr_t tag)   const {
        return (tag << log2AddrLs);
    }
};

class PrivateCache {
public:
    PrivateCache(const char* section, int nProcs);
    ~PrivateCache();

    typedef CacheAssocTM<CState1, VAddr>            Cache;
    typedef CState1 Line;

    void doLoad(InstDesc* inst, ThreadContext* context, VAddr addr, MemOpStatus* p_opStatus);
    void doStore(InstDesc* inst, ThreadContext* context, VAddr addr, MemOpStatus* p_opStatus);

    // Functions forwarded to Cache
    Line* findLine(Pid_t pid, VAddr addr);
    void clearTransactional(Pid_t pid);
private:

    Line* doFillLine(Pid_t pid, bool isInTM, VAddr addr, MemOpStatus* p_opStatus);

    // Member variables
    std::vector<Cache*>         caches;
};

template<class Line, class Addr_t>
void
CacheAssocTM<Line, Addr_t>::clearTransactional()
{
    for(uint32_t i = 0; i < numLines; i++) {
        mem[i].clearTransactional();
    }
}


#endif
