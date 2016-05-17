#include "nanassert.h"
#include "SescConf.h"
#include "libemul/EmulInit.h"
#include "libll/ThreadContext.h"
#include "IdealTSXManager.h"

using namespace std;

/////////////////////////////////////////////////////////////////////////////////////////
// Infinite-capacity version of TSX HTM
/////////////////////////////////////////////////////////////////////////////////////////
IdealTSXManager::IdealTSXManager(const char tmStyle[], int32_t nCores, int32_t line):
        HTMManager(tmStyle, nCores, line) {

    int totalSize = SescConf->getInt("TransactionalMemory", "totalSize");
    int assoc = SescConf->getInt("TransactionalMemory", "assoc");

    for(int coreId = 0; coreId < nCores; coreId++) {
        caches.push_back(new CacheAssocTM(totalSize, assoc, lineSize, 1));
    }
}

///
// Destructor for PrivateCache. Delete all allocated members
IdealTSXManager::~IdealTSXManager() {
    while(caches.size() > 0) {
        Cache* cache = caches.back();
        caches.pop_back();
        delete cache;
    }
}


///
// Helper function that replaces a line in the Cache
IdealTSXManager::Line* IdealTSXManager::replaceLine(Cache* cache, VAddr raddr) {
    Line* replaced = cache->findLine2Replace(raddr);
    if(replaced == nullptr) {
        fail("Replacement policy failed");
    }

    // Replace the line
    VAddr caddr = addrToCacheLine(raddr);
    VAddr myTag = cache->calcTag(raddr);
    replaced->invalidate();
    replaced->validate(myTag, caddr);

    return replaced;
}
///
// Helper function that aborts all transactional readers
void IdealTSXManager::clearTransactional(VAddr caddr, const PidSet& toClear) {
    PidSet::const_iterator iPid;
    for(iPid = toClear.begin(); iPid != toClear.end(); ++iPid) {
        Cache* cache = getCache(*iPid);
        Line* line = cache->findLine(caddr);
        if(line) {
            line->clearTransactional(*iPid);
        }
    }
}

///
// Helper function that cleans dirty lines in each cache except pid's.
void IdealTSXManager::cleanDirtyLines(VAddr caddr, Pid_t pid) {
    Cache* myCache = getCache(pid);
    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        Line* line = cache->findLine(caddr);
        if(line && line->isValid() && line->isDirty()) {
            if(cache != myCache) {
                line->makeClean();
            }
            // Requester's cache should be left alone
        }
    }
}

///
// Helper function that invalidates lines except pid's.
void IdealTSXManager::invalidateLines(VAddr caddr, Pid_t pid) {
    Cache* myCache = getCache(pid);
    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        Line* line = cache->findLine(caddr);
        if(line && line->isValid()) {
            if(cache != myCache) {
                line->invalidate();
            }
            // Requester's cache should be left alone
        }
    }
}

///
// Helper function that aborts all transactional readers
void IdealTSXManager::abortTMWriters(Pid_t pid, VAddr caddr, bool isTM) {
    // Collect writers
    set<Pid_t> aborted;
    rwSetManager.getWriters(caddr, aborted);
    aborted.erase(pid);

    TMAbortType_e abortType = isTM ? TM_ATYPE_DEFAULT : TM_ATYPE_NONTM;

    if(aborted.size() > 0) {
        // Do the abort
        clearTransactional(caddr, aborted);
        markTransAborted(aborted, pid, caddr, abortType);
    }
}

///
// Helper function that aborts all transactional readers and writers
void IdealTSXManager::abortTMSharers(Pid_t pid, VAddr caddr, bool isTM) {
    // Collect sharers
    set<Pid_t> aborted;
    rwSetManager.getWriters(caddr, aborted);
    rwSetManager.getReaders(caddr, aborted);
    aborted.erase(pid);

    TMAbortType_e abortType = isTM ? TM_ATYPE_DEFAULT : TM_ATYPE_NONTM;

    if(aborted.size() > 0) {
        // Do the abort
        clearTransactional(caddr, aborted);
        markTransAborted(aborted, pid, caddr, abortType);
    }
}

///
// Do a transactional read.
TMRWStatus IdealTSXManager::TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    abortTMWriters(pid, caddr, true);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        cleanDirtyLines(caddr, pid);
        line  = replaceLine(cache, raddr);
    } else {
        p_opStatus->wasHit = true;
        if(line->isTransactional() == false && line->isDirty()) {
            // If we were the previous writer, make clean and start anew
            line->makeClean();
        }
    }
    if(line->isValid() == false || line->getCaddr() != caddr) {
        fail("got wrong line");
    }

    if(getTMState(pid) == TMStateEngine::TM_MARKABORT) {
        return TMRW_ABORT;
    }

    // Update line
    line->markTransactional();
    line->addReader(pid);

    // Do the read
    readTrans(pid, raddr, caddr);

    return TMRW_SUCCESS;
}


///
// Do a transactional write.
TMRWStatus IdealTSXManager::TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    abortTMSharers(pid, caddr, true);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        invalidateLines(caddr, pid);
        line  = replaceLine(cache, raddr);
    } else if(line->isDirty() == false) {
        p_opStatus->wasHit = false;
        invalidateLines(caddr, pid);
    } else {
        p_opStatus->wasHit = true;
    }
    if(line->isValid() == false || line->getCaddr() != caddr) {
        fail("got wrong line");
    }
    if(getTMState(pid) == TMStateEngine::TM_MARKABORT) {
        return TMRW_ABORT;
    }

    // Update line
    line->markTransactional();
    line->makeTransactionalDirty(pid);

    // Do the write
    writeTrans(pid, raddr, caddr);

    return TMRW_SUCCESS;
}

///
// Do a non-transactional read, i.e. when a thread not inside a transaction.
void IdealTSXManager::nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    abortTMWriters(pid, caddr, false);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        cleanDirtyLines(caddr, pid);
        line  = replaceLine(cache, raddr);
    } else {
        p_opStatus->wasHit = true;
    }
    if(line->isValid() == false || line->getCaddr() != caddr) {
        fail("got wrong line");
    }
}

///
// Do a non-transactional write, i.e. when a thread not inside a transaction.
void IdealTSXManager::nonTMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    abortTMSharers(pid, caddr, false);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        invalidateLines(caddr, pid);
        line  = replaceLine(cache, raddr);
    } else if(line->isDirty() == false) {
        p_opStatus->wasHit = false;
        invalidateLines(caddr, pid);
    } else {
        p_opStatus->wasHit = true;
    }
    if(line->isValid() == false || line->getCaddr() != caddr) {
        fail("got wrong line");
    }


    // Update line
    line->makeDirty();
}

TMBCStatus IdealTSXManager::myCommit(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();

    p_opStatus->tmLat           = 4 + rwSetManager.getNumWrites(pid);
    p_opStatus->tmCommitSubtype =TM_COMMIT_REGULAR;

    // On commit, we clear all transactional bits, but otherwise leave lines alone
    Cache* cache = getCache(pid);

    LineTMComparator tmCmp;
    std::vector<Line*> lines;
    cache->collectLines(lines, tmCmp);

    for(Line* line: lines) {
        line->clearTransactional(pid);
    }

    return TMBC_SUCCESS;
}

TMBCStatus IdealTSXManager::myAbort(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
    // On abort, we need to throw away the work we've done so far, so invalidate them
    Pid_t pid   = context->getPid();
    Cache* cache = getCache(pid);

    LineTMComparator tmCmp;
    std::vector<Line*> lines;
    cache->collectLines(lines, tmCmp);

    for(Line* line: lines) {
        if(line->isDirty() && line->getWriter() == pid) {
            line->invalidate();
        } else {
            line->clearTransactional(pid);
        }
    }

    return TMBC_SUCCESS;
}

