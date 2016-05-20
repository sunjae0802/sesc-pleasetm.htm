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
        HTMManager(tmStyle, nCores, line),
        getSMsg("tm:getSMsg"),
        fwdGetSMsg("tm:fwdGetSMsg"),
        getMMsg("tm:getMMsg"),
        invMsg("tm:invMsg"),
        flushMsg("tm:flushMsg"),
        fwdGetSConflictMsg("tm:fwdGetSConflictMsg"),
        invConflictMsg("tm:invConflictMsg") {

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
IdealTSXManager::Line* IdealTSXManager::replaceLine(Pid_t pid, VAddr raddr) {
    Cache* cache= getCache(pid);

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
void IdealTSXManager::abortTMWriters(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except) {
    // Collect writers
    set<Pid_t> aborted;
    rwSetManager.getWriters(caddr, aborted);
    aborted.erase(pid);

    TMAbortType_e abortType = isTM ? TM_ATYPE_DEFAULT : TM_ATYPE_NONTM;

    if(aborted.size() > 0) {
        // Do the abort
        for(Pid_t a: aborted) {
            Cache* cache = getCache(a);
            Line* line = cache->findLine(caddr);
            if(line) {
                line->clearTransactional(a);
            } else if(getTMState(a) == TMStateEngine::TM_RUNNING) {
                fail("[%d] Aborting non-writer %d?: 0x%lx", pid, a, caddr);
            }
        }
        markTransAborted(aborted, pid, caddr, abortType);
    }
}

///
// Helper function that aborts all transactional readers and writers
void IdealTSXManager::abortTMSharers(Pid_t pid, VAddr caddr, bool isTM, std::set<Cache*>& except) {
    // Collect sharers
    set<Pid_t> aborted;
    rwSetManager.getWriters(caddr, aborted);
    rwSetManager.getReaders(caddr, aborted);
    aborted.erase(pid);

    TMAbortType_e abortType = isTM ? TM_ATYPE_DEFAULT : TM_ATYPE_NONTM;

    if(aborted.size() > 0) {
        // Do the abort
        markTransAborted(aborted, pid, caddr, abortType);
        for(Pid_t a: aborted) {
            Cache* cache = getCache(a);
            Line* line = cache->findLine(caddr);
            if(line) {
                line->clearTransactional(a);
            }
        }
    }
}

///
// Helper function that cleans dirty lines in each cache except pid's.
void IdealTSXManager::cleanDirtyLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except) {
    getSMsg.inc();

    Cache* myCache = getCache(pid);
    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        Line* line = cache->findLine(caddr);
        if(line && line->isValid() && line->isDirty()) {
            if(cache == myCache) {
                // Requester's cache should be left alone
            } else if(except.find(cache) == except.end()) {
                if(line->isTransactional()) {
                    fwdGetSConflictMsg.inc();
                } else {
                    fwdGetSMsg.inc();
                }
                line->makeClean();
            } else {
                // In except set so don't do anything
            }
        }
    }
}

///
// Helper function that invalidates lines except pid's.
void IdealTSXManager::invalidateLines(Pid_t pid, VAddr caddr, std::set<Cache*>& except) {
    getMMsg.inc();

    Cache* myCache = getCache(pid);
    for(size_t cid = 0; cid < caches.size(); cid++) {
        Cache* cache = caches.at(cid);
        Line* line = cache->findLine(caddr);
        if(line && line->isValid()) {
            if(cache == myCache) {
                // Requester's cache should be left alone
            } else if(except.find(cache) == except.end()) {
                if(line->isTransactional()) {
                    invConflictMsg.inc();
                } else {
                    invMsg.inc();
                }
                line->invalidate();
            } else {
                // In except set so don't do anything
            }
        }
    }
}

///
// Do a transactional read.
TMRWStatus IdealTSXManager::TMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    std::set<Cache*> except;
    abortTMWriters(pid, caddr, true, except);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        cleanDirtyLines(pid, caddr, except);
        line  = replaceLine(pid, raddr);
    } else {
        p_opStatus->wasHit = true;
        if(line->isTransactional() == false && line->isDirty()) {
            // If we were the previous writer, make clean and start anew
            line->makeClean();
            flushMsg.inc();
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

    return TMRW_SUCCESS;
}


///
// Do a transactional write.
TMRWStatus IdealTSXManager::TMWrite(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    std::set<Cache*> except;
    abortTMSharers(pid, caddr, true, except);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        invalidateLines(pid, caddr, except);
        line  = replaceLine(pid, raddr);
    } else if(line->isDirty() == false) {
        p_opStatus->wasHit = false;
        invalidateLines(pid, caddr, except);
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

    return TMRW_SUCCESS;
}

///
// Do a non-transactional read, i.e. when a thread not inside a transaction.
void IdealTSXManager::nonTMRead(InstDesc* inst, const ThreadContext* context, VAddr raddr, InstContext* p_opStatus) {
    Pid_t pid   = context->getPid();
    Cache* cache= getCache(pid);
	VAddr caddr = addrToCacheLine(raddr);

    std::set<Cache*> except;
    abortTMWriters(pid, caddr, false, except);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        cleanDirtyLines(pid, caddr, except);
        line  = replaceLine(pid, raddr);
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

    std::set<Cache*> except;
    abortTMSharers(pid, caddr, false, except);

    // Do cache hit/miss stats
    Line*   line  = cache->lookupLine(raddr);
    if(line == nullptr) {
        p_opStatus->wasHit = false;
        invalidateLines(pid, caddr, except);
        line  = replaceLine(pid, raddr);
    } else if(line->isDirty() == false) {
        p_opStatus->wasHit = false;
        invalidateLines(pid, caddr, except);
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
    // On commit, we clear all transactional bits, but otherwise leave lines alone
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

void IdealTSXManager::myStartAborting(InstDesc* inst, const ThreadContext* context, InstContext* p_opStatus) {
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
}

