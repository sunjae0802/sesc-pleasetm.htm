#ifndef PRIVATE_CACHE
#define PRIVATE_CACHE

#include <set>
#include "Snippets.h"
#include "libemul/InstDesc.h"
#include "libemul/Addressing.h"
#include "CacheCore.h"

enum EvictCause {
    NoEvict = 0,
    EvictByWrite,
    EvictSetConflict,
};

class TMLine : public StateGeneric<> {
private:
    bool            dirty;
    bool            transactional;
    std::set<Pid_t> tmReaders;
    Pid_t           tmWriter;
    VAddr           caddr;
    static const VAddr INVALID_CADDR = 0xDEADCADD;
public:
    TMLine() {
        invalidate();
    }
    bool isReader(Pid_t p) const {
        return tmReaders.find(p) != tmReaders.end();
    }
    bool isWriter(Pid_t p) const {
        return tmWriter == p;
    }
    void addReader(Pid_t p);
    const std::set<Pid_t>& getReaders() const {
        return tmReaders;
    }
    Pid_t getWriter() const {
        return tmWriter;
    }
    void getAccessors(std::set<Pid_t>& accessors) const;
    void validate(VAddr t, VAddr c);
    VAddr getCaddr() const {
        return caddr;
    }
    bool isDirty() const {
        return dirty;
    }
    void makeDirty();
    void makeTransactionalDirty(Pid_t writer);
    void makeClean();
    bool isTransactional() const {
        return transactional;
    }
    void markTransactional() {
        transactional = true;
    }
    void clearTransactional(Pid_t p);
    virtual void invalidate();
};

struct LineComparator {
    virtual bool operator()(TMLine* l) const = 0;
};

struct LineValidComparator: public LineComparator {
    virtual bool operator()(TMLine* l) const { return l->isValid(); }
};
struct LineTMComparator: public LineComparator {
    virtual bool operator()(TMLine* l) const {
        return l->isValid() && l->isTransactional();
    }
};
struct LineInvalidComparator: public LineComparator {
    virtual bool operator()(TMLine* l) const { return l->isValid() == false; }
};
struct LineInvalidOrNonTMOrCleanComparator: public LineComparator {
    virtual bool operator()(TMLine* l) const {
        return (l->isValid() == false) || (l->isTransactional() == false) || (l->isDirty() == false);
    }
};
struct LineNonTMComparator: public LineComparator {
    virtual bool operator()(TMLine* l) const {
        return l->isTransactional() == false;
    }
};
struct LineNonTMOrCleanComparator: public LineComparator {
    virtual bool operator()(TMLine* l) const {
        return (l->isTransactional() == false) || (l->isDirty() == false);
    }
};
struct LineTMDirtyComparator: public LineComparator {
    virtual bool operator()(TMLine* l) const {
        return l->isValid() && l->isTransactional() && l->isDirty();
    }
};
struct LineTMWrittenByComparator: public LineComparator {
    LineTMWrittenByComparator(Pid_t p): pid(p) {}
    virtual bool operator()(TMLine* l) const {
        return (l->isValid() && l->isTransactional() && l->isWriter(pid));
    }
private:
    Pid_t pid;
};
struct LineTMAccessedByComparator: public LineComparator {
    LineTMAccessedByComparator(Pid_t p): pid(p) {}
    virtual bool operator()(TMLine* l) const {
        return (l->isValid() && l->isTransactional() && (l->isReader(pid) || l->isWriter(pid)));
    }
private:
    Pid_t pid;
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
    TMLine *findLine2Replace(TMLine** theSet);

    TMLine **findOldestLine(TMLine **theSet, const LineComparator& comp);
    size_t countLines(TMLine **theSet, const LineComparator& comp) const;

public:
    CacheAssocTM(int32_t size, int32_t assoc, int32_t blksize, int32_t addrUnit);
    virtual ~CacheAssocTM() {
        delete [] content;
        delete [] mem;
    }

    TMLine *findLine2Replace(VAddr addr);
    TMLine *lookupLine(VAddr addr);
    TMLine *findLine(VAddr addr);
    size_t countLines(VAddr addr, const LineComparator& comp) const;
    void collectLines(std::vector<TMLine*>& lines, const LineComparator& comp);

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
