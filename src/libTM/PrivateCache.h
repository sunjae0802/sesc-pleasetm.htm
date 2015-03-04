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

class TMLine : public StateGeneric<> {
private:
    bool dirty;
    bool transactional;
public:
    TMLine() {
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

    TMLine *mem;
    TMLine **content;
    void moveToMRU(TMLine** theSet, TMLine** theTMLine);
    TMLine *findLine2Replace(bool isInTM, TMLine** theSet);
    TMLine **findInvalid(TMLine **theSet);
    TMLine **findOldestNonTMClean(TMLine **theSet);
    TMLine **findOldestClean(TMLine **theSet);
    TMLine **findOldestNonTM(TMLine **theSet);

    typedef bool (*lineConditionFunc)(TMLine *l);
    static bool lineValid(TMLine *l) { return l->isValid(); }
    static bool lineDirty(TMLine *l) { return l->isDirty(); }
    static bool lineTransactional(TMLine *l) { return l->isTransactional(); }
    static bool lineTransactionalDirty(TMLine *l) { return l->isTransactional() && l->isDirty(); }
    size_t countLines(TMLine **theSet, lineConditionFunc func) const;

public:
    CacheAssocTM(int32_t size, int32_t assoc, int32_t blksize, int32_t addrUnit);
    virtual ~CacheAssocTM() {
        delete [] content;
        delete [] mem;
    }

    TMLine *findLine2Replace(bool isInTM, VAddr addr);
    TMLine *lookupLine(VAddr addr);
    TMLine *findLine(VAddr addr);
    void clearTransactional();

    uint32_t  getTMLineSize() const   {
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

    VAddr calcTag(VAddr addr)       const {
        return (addr >> log2AddrLs);
    }

    uint32_t calcSet4Tag(VAddr tag)     const {
        return (tag & maskSets);
    }
    uint32_t calcSet4Addr(VAddr addr)   const {
        return calcSet4Tag(calcTag(addr));
    }

    uint32_t calcIndex4Set(uint32_t set)    const {
        return (set << log2Assoc);
    }
    uint32_t calcIndex4Tag(uint32_t tag)    const {
        return calcIndex4Set(calcSet4Tag(tag));
    }
    uint32_t calcIndex4Addr(VAddr addr) const {
        return calcIndex4Set(calcSet4Addr(addr));
    }

    VAddr calcAddr4Tag(VAddr tag)   const {
        return (tag << log2AddrLs);
    }
};

#endif
