/*
   SESC: Super ESCalar simulator
   Copyright (C) 2003 University of Illinois.

   Contributed by Milos Prvulovic

This file is part of SESC.

SESC is free software; you can redistribute it and/or modify it under the terms
of the GNU General Public License as published by the Free Software Foundation;
either version 2, or (at your option) any later version.

SESC is    distributed in the  hope that  it will  be  useful, but  WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should  have received a copy of  the GNU General  Public License along with
SESC; see the file COPYING.  If not, write to the  Free Software Foundation, 59
Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

// For ostringstream
#include <sstream>
#include "ThreadContext.h"
#include "libemul/FileSys.h"
#include "libcore/ProcessId.h"
#include "libcore/DInst.h"

ThreadContext::ContextVector ThreadContext::pid2context;
bool ThreadContext::ff;
Time_t ThreadContext::resetTS = 0;
TimeTrackerStats ThreadContext::timeTrackerStats;
std::set<uint32_t> ThreadContext::tmFallbackMutexCAddrs;
bool ThreadContext::simDone = false;
int64_t ThreadContext::finalSkip = 0;
bool ThreadContext::inMain = false;
size_t ThreadContext::numThreads = 0;

void ThreadContext::initialize(bool child) {
    if(pid == getMainThreadContext()->getPid()) {
        char filename[256];
        sprintf(filename, "datafile.out");
        tracefile.open(filename);
    }

    prevDInstRetired = 0;
    nRetiredInsts = 0;
    spinning    = false;

#if (defined TM)
    tmArg       = 0;
    tmLat       = 0;
    tmContext   = NULL;
    tmDepth     = 0;
    tmCallsite  = 0;
    tmlibUserTid= INVALID_USER_TID;
    tmBeginSubtype=TM_BEGIN_INVALID;
    tmCommitSubtype=TM_COMMIT_INVALID;
    tmMemopHadStalled = false;
#endif

    getTracefile() << ThreadContext::numThreads << " 0 "
            << nRetiredInsts << ' ' << globalClock << std::endl;
    ThreadContext::numThreads++;
}

void ThreadContext::cleanup() {
    ThreadContext::numThreads--;
    getTracefile() << ThreadContext::numThreads << " 1 "
                << nRetiredInsts << ' ' << globalClock << std::endl;

    ThreadContext::timeTrackerStats.sum(myTimeStats);

	if(pid == getMainThreadContext()->getPid()) {
        ThreadContext::timeTrackerStats.print();
        if(tracefile.is_open()) {
            tracefile.close();
        }
    }
}

#if defined(TM)
void ThreadContext::setTMlibUserTid(uint32_t arg) {
    if(tmlibUserTid == INVALID_USER_TID) {
        tmlibUserTid = arg;
    } else if(tmlibUserTid != arg) {
        fail("TMlib user TID changed?\n");
    }
}
TMBCStatus ThreadContext::beginTransaction(InstDesc* inst) {
    if(tmDepth > 0) {
        fail("Transaction nesting not complete\n");
    }
    TMBCStatus status = tmCohManager->begin(inst, this);
    switch(status) {
        case TMBC_SUCCESS: {
            const TransState& transState = tmCohManager->getTransState(pid);
            tmBeginSubtype=TM_BEGIN_REGULAR;
            tmCommitSubtype=TM_COMMIT_INVALID;

            uint64_t utid = transState.getUtid();
            tmContext   = new TMContext(this, inst, utid);
            tmContext->saveContext();
            saveCallRetStack();
            tmDepth++;

            break;
        }
        case TMBC_NACK: {
            tmBeginSubtype=TM_BEGIN_NACKED;
            break;
        }
        default:
            fail("Unhanded TM begin");
    }
    return status;
}
TMBCStatus ThreadContext::commitTransaction(InstDesc* inst) {
    if(tmDepth == 0) {
        fail("Commit fail: tmDepth is 0\n");
    }
    if(tmContext == NULL) {
        fail("Commit fail: tmContext is NULL\n");
    }

    // Save UTID before committing
    uint64_t utid = tmCohManager->getUtid(pid);
    size_t numWrites = tmCohManager->getNumWrites(pid);
    tmLat       = 4;

    TMBCStatus status = tmCohManager->commit(inst, this);
    switch(status) {
        case TMBC_NACK: {
            // In the case of a Lazy model that can not commit yet
            break;
        }
        case TMBC_ABORT: {
            // In the case of a Lazy model where we are forced to Abort
            tmCommitSubtype=TM_COMMIT_ABORTED;
            abortTransaction(inst);
            break;
        }
        case TMBC_SUCCESS: {
            // If we have already delayed, go ahead and finalize commit in memory
            tmContext->flushMemory();

            tmLat       += numWrites;
            tmCommitSubtype=TM_COMMIT_REGULAR;

            TMContext* oldTMContext = tmContext;
            if(isInTM()) {
                tmContext = oldTMContext->getParentContext();
            } else {
                tmContext = NULL;
            }
            delete oldTMContext;
            tmDepth--;

            break;
        }
        default:
            fail("Unhanded TM commit");
    }
    return status;
}
TMBCStatus ThreadContext::abortTransaction(InstDesc* inst, TMAbortType_e abortType) {
    tmCohManager->markAbort(inst, this, abortType);

    return abortTransaction(inst);
}
TMBCStatus ThreadContext::abortTransaction(InstDesc* inst) {
    if(tmContext == NULL) {
        fail("Abort fail: tmContext is NULL\n");
    }

    // Save UTID before aborting
    uint64_t utid = tmCohManager->getUtid(pid);

    TMBCStatus status = tmCohManager->abort(inst, this);
    switch(status) {
        case TMBC_SUCCESS: {
            const TransState& transState = tmCohManager->getTransState(pid);

            // Since we jump to the outer-most context, find it first
            TMContext* rootTMContext = tmContext;
            while(rootTMContext->getParentContext()) {
                TMContext* oldTMContext = rootTMContext;
                rootTMContext = rootTMContext->getParentContext();
                delete oldTMContext;
            }
            rootTMContext->restoreContext();

            VAddr beginIAddr = rootTMContext->getBeginIAddr();

            tmContext = NULL;
            delete rootTMContext;

            tmDepth = 0;

            restoreCallRetStack();

            // Move instruction pointer to BEGIN
            setIAddr(beginIAddr);

            break;
        }
        default:
            fail("Unhanded TM abort");
    }
    return status;
}

void ThreadContext::completeAbort(InstDesc* inst) {
    tmCohManager->completeAbort(pid);
    tmBeginSubtype=TM_COMPLETE_ABORT;
}

uint32_t ThreadContext::getAbortRV() {
    // Get abort state
    const TransState &transState = tmCohManager->getTransState(pid);
    const TMAbortState& abortState = tmCohManager->getAbortState(pid);

    // LSB is 1 to show that this is an abort
    uint32_t abortRV = 1;

    // bottom 8 bits are reserved
    abortRV |= tmArg << 8;
    // Set aborter Pid in upper bits
    abortRV |= (abortState.getAborterPid()) << 12;

    TMAbortType_e abortType = abortState.getAbortType();
    switch(abortType) {
        case TM_ATYPE_SYSCALL:
            abortRV |= 2;
            break;
        case TM_ATYPE_USER:
            abortRV |= 4;
            break;
        case TM_ATYPE_SETCONFLICT:
            abortRV |= 8;
            break;
        default:
            // Do nothing
            break;
    }

    return abortRV;
}

uint32_t ThreadContext::getBeginRV(TMBCStatus status) {
    if(status == TMBC_NACK) {
        return 2;
    } else {
        return 0;
    }
}
void ThreadContext::beginFallback(uint32_t pFallbackMutex) {
    VAddr mutexCAddr = tmCohManager->addrToCacheLine(pFallbackMutex);
    ThreadContext::tmFallbackMutexCAddrs.insert(mutexCAddr);
    tmCohManager->beginFallback(pid);
}

void ThreadContext::completeFallback() {
    tmCohManager->completeFallback(pid);
}

#endif

ThreadContext *ThreadContext::getContext(Pid_t pid)
{
    I(pid>=0);
    I((size_t)pid<pid2context.size());
    return pid2context[pid];
}

void ThreadContext::setMode(ExecMode mode) {
    execMode=mode;
    if(mySystem)
        delete mySystem;
    mySystem=LinuxSys::create(execMode);
}

ThreadContext::ThreadContext(FileSys::FileSys *fileSys)
    :
    myStackAddrLb(0),
    myStackAddrUb(0),
    execMode(ExecModeNone),
    iAddr(0),
    iDesc(InvalidInstDesc),
    dAddr(0),
    l1Hit(false),
    nDInsts(0),
    stallUntil(0),
    fileSys(fileSys),
    openFiles(new FileSys::OpenFiles()),
    sigTable(new SignalTable()),
    sigMask(),
    maskedSig(),
    readySig(),
    suspSig(false),
    mySystem(0),
    parentID(-1),
    childIDs(),
    exitSig(SigNone),
    clear_child_tid(0),
    robust_list(0),
    exited(false),
    exitCode(0),
    killSignal(SigNone),
    callStack()
{
    for(tid=0; (tid<int(pid2context.size()))&&pid2context[tid]; tid++);
    if(tid==int(pid2context.size())) {
        pid2context.push_back(this);
    } else {
        pid2context[tid]=this;
    }
    pid=tid;
    tgid=tid;
    pgid=tid;

    memset(regs,0,sizeof(regs));
    setAddressSpace(new AddressSpace());
    initialize(false);
}

ThreadContext::ThreadContext(ThreadContext &parent,
                             bool cloneParent, bool cloneFileSys, bool newNameSpace,
                             bool cloneFiles, bool cloneSighand,
                             bool cloneVm, bool cloneThread,
                             SignalID sig, VAddr clearChildTid)
    :
    myStackAddrLb(parent.myStackAddrLb),
    myStackAddrUb(parent.myStackAddrUb),
    dAddr(0),
    l1Hit(false),
    nDInsts(0),
    stallUntil(0),
    fileSys(cloneFileSys?((FileSys::FileSys *)(parent.fileSys)):(new FileSys::FileSys(*(parent.fileSys),newNameSpace))),
    openFiles(cloneFiles?((FileSys::OpenFiles *)(parent.openFiles)):(new FileSys::OpenFiles(*(parent.openFiles)))),
    sigTable(cloneSighand?((SignalTable *)(parent.sigTable)):(new SignalTable(*(parent.sigTable)))),
    sigMask(),
    maskedSig(),
    readySig(),
    suspSig(false),
    mySystem(0),
    parentID(cloneParent?parent.parentID:parent.pid),
    childIDs(),
    exitSig(sig),
    clear_child_tid(0),
    robust_list(0),
    exited(false),
    exitCode(0),
    killSignal(SigNone),
    callStack(parent.callStack)
{
    I((!newNameSpace)||(!cloneFileSys));
    setMode(parent.execMode);
    for(tid=0; (tid<int(pid2context.size()))&&pid2context[tid]; tid++);
    if(tid==int(pid2context.size())) {
        pid2context.push_back(this);
    } else {
        pid2context[tid]=this;
    }
    pid=tid;
    if(cloneThread) {
        tgid=parent.tgid;
        I(tgid!=-1);
        I(pid2context[tgid]);
        pid2context[tgid]->tgtids.insert(tid);
    } else {
        tgid=tid;
    }
    pgid=parent.pgid;
    if(parentID!=-1)
        pid2context[parentID]->childIDs.insert(pid);
    memcpy(regs,parent.regs,sizeof(regs));
    // Copy address space and instruction pointer
    if(cloneVm) {
        setAddressSpace(parent.getAddressSpace());
        iAddr=parent.iAddr;
        iDesc=parent.iDesc;
    } else {
        setAddressSpace(new AddressSpace(*(parent.getAddressSpace())));
        iAddr=parent.iAddr;
        iDesc=virt2inst(iAddr);
    }
    // This must be after setAddressSpace (it resets clear_child_tid)
    clear_child_tid=clearChildTid;

    initialize(true);
}

ThreadContext::~ThreadContext(void) {
    I(!nDInsts);
    while(!maskedSig.empty()) {
        delete maskedSig.back();
        maskedSig.pop_back();
    }
    while(!readySig.empty()) {
        delete readySig.back();
        readySig.pop_back();
    }
    if(getAddressSpace())
        setAddressSpace(0);
    if(mySystem)
        delete mySystem;
}

void ThreadContext::setAddressSpace(AddressSpace *newAddressSpace) {
    if(addressSpace)
        getSystem()->clearChildTid(this,clear_child_tid);
    addressSpace=newAddressSpace;
}

#include "libcore/OSSim.h"

int32_t ThreadContext::findZombieChild(void) const {
    for(IntSet::iterator childIt=childIDs.begin(); childIt!=childIDs.end(); childIt++) {
        ThreadContext *childContext=getContext(*childIt);
        if(childContext->isExited()||childContext->isKilled())
            return *childIt;
    }
    return 0;
}

void ThreadContext::suspend(void) {
    I(!isSuspended());
    I(!isExited());
    suspSig=true;
    osSim->eventSuspend(pid,pid);
}

void ThreadContext::signal(SigInfo *sigInfo) {
    I(!isExited());
    SignalID sig=sigInfo->signo;
    if(sigMask.test(sig)) {
        maskedSig.push_back(sigInfo);
    } else {
        readySig.push_back(sigInfo);
        if(suspSig)
            resume();
    }
}

void ThreadContext::resume(void) {
    I(suspSig);
    I(!exited);
    suspSig=false;
    osSim->eventResume(pid,pid);
}

bool ThreadContext::exit(int32_t code) {
    if(!retsEmpty()) {
        fail("TM lib call not balanced");
    }
	cleanup();
    I(!isExited());
    I(!isKilled());
    I(!isSuspended());
    openFiles=0;
    sigTable=0;
    exited=true;
    exitCode=code;
    if(tgid!=tid) {
        I(tgid!=-1);
        I(pid2context[tgid]);
        pid2context[tgid]->tgtids.erase(tid);
        tgid=-1;
    }
    if(pgid==tid) {
        // TODO: Send SIGHUP to each process in the process group
    }
    osSim->eventExit(pid,exitCode);
    while(!childIDs.empty()) {
        ThreadContext *childContext=getContext(*(childIDs.begin()));
        I(childContext->parentID==pid);
        childIDs.erase(childContext->pid);
        childContext->parentID=-1;
        if(childContext->exited)
            childContext->reap();
    }
    iAddr=0;
    iDesc=InvalidInstDesc;
    if(robust_list)
        getSystem()->exitRobustList(this,robust_list);
    if(parentID==-1) {
        reap();
        return true;
    }
    ThreadContext *parent=getContext(parentID);
    I(parent->pid==parentID);
    I(parent->childIDs.count(pid));
    return false;
}
void ThreadContext::reap() {
    I(exited);
    if(parentID!=-1) {
        ThreadContext *parent=getContext(parentID);
        I(parent);
        I(parent->pid==parentID);
        I(parent->childIDs.count(pid));
        parent->childIDs.erase(pid);
    }
    pid2context[pid]=0;
}

inline bool ThreadContext::skipInst(void) {
    if(isSuspended())
        return false;
    if(isExited())
        return false;
#if (defined DEBUG_InstDesc)
    iDesc->debug();
#endif
    (*iDesc)(this);
    return true;
}

int64_t ThreadContext::skipInsts(int64_t skipCount) {
    int64_t skipped=0;
    int nowPid=0;
    if(skipCount<0) {
        ThreadContext::ff = true;
        while(ThreadContext::ff) {
            nowPid=nextReady(nowPid);
            if(nowPid==-1)
                return skipped;
            ThreadContext::pointer context=pid2context[nowPid];
            I(context);
            I(!context->isSuspended());
            I(!context->isExited());
			int nowSkip = 500;
            while(nowSkip&&ThreadContext::ff&&context->skipInst()) {
				nowSkip--;
                skipped++;
            }
            nowPid++;
		}
    } else {
        while(skipped<skipCount) {
            nowPid=nextReady(nowPid);
            if(nowPid==-1)
                return skipped;
            ThreadContext::pointer context=pid2context[nowPid];
            I(context);
            I(!context->isSuspended());
            I(!context->isExited());
            int nowSkip=(skipCount-skipped<500)?(skipCount-skipped):500;
            while(nowSkip&&context->skipInst()) {
                nowSkip--;
                skipped++;
            }
            nowPid++;
        }
    }
    return skipped;
}

void ThreadContext::writeMemFromBuf(VAddr addr, size_t len, const void *buf) {
    I(canWrite(addr,len));
    const uint8_t *byteBuf=(uint8_t *)buf;
    while(len) {
        if((addr&sizeof(uint8_t))||(len<sizeof(uint16_t))) {
            writeMemRaw(addr,*((uint8_t *)byteBuf));
            addr+=sizeof(uint8_t);
            byteBuf+=sizeof(uint8_t);
            len-=sizeof(uint8_t);
        } else if((addr&sizeof(uint16_t))||(len<sizeof(uint32_t))) {
            writeMemRaw(addr,*((uint16_t *)byteBuf));
            addr+=sizeof(uint16_t);
            byteBuf+=sizeof(uint16_t);
            len-=sizeof(uint16_t);
        } else if((addr&sizeof(uint32_t))||(len<sizeof(uint64_t))) {
            writeMemRaw(addr,*((uint32_t *)byteBuf));
            addr+=sizeof(uint32_t);
            byteBuf+=sizeof(uint32_t);
            len-=sizeof(uint32_t);
        } else {
            I(!(addr%sizeof(uint64_t)));
            I(len>=sizeof(uint64_t));
            writeMemRaw(addr,*((uint64_t *)byteBuf));
            addr+=sizeof(uint64_t);
            byteBuf+=sizeof(uint64_t);
            len-=sizeof(uint64_t);
        }
    }
}
/*
ssize_t ThreadContext::writeMemFromFile(VAddr addr, size_t len, int32_t fd, bool natFile, bool usePread, off_t offs){
  I(canWrite(addr,len));
  ssize_t retVal=0;
  uint8_t buf[AddressSpace::getPageSize()];
  while(len){
    size_t ioSiz=AddressSpace::getPageSize()-(addr&(AddressSpace::getPageSize()-1));
    if(ioSiz>len)
      ioSiz=len;
    ssize_t nowRet;
    if(usePread){
      nowRet=(natFile?(pread(fd,buf,ioSiz,offs+retVal)):(openFiles->pread(fd,buf,ioSiz,offs+retVal)));
    }else{
      nowRet=(natFile?(read(fd,buf,ioSiz)):(openFiles->read(fd,buf,ioSiz)));
    }
    if(nowRet==-1)
      return nowRet;
    retVal+=nowRet;
    writeMemFromBuf(addr,nowRet,buf);
    addr+=nowRet;
    len-=nowRet;
    if(nowRet<(ssize_t)ioSiz)
      break;
  }
  return retVal;
}
*/
void ThreadContext::writeMemWithByte(VAddr addr, size_t len, uint8_t c) {
    I(canWrite(addr,len));
    uint8_t buf[AddressSpace::getPageSize()];
    memset(buf,c,AddressSpace::getPageSize());
    while(len) {
        size_t wrSiz=AddressSpace::getPageSize()-(addr&(AddressSpace::getPageSize()-1));
        if(wrSiz>len) wrSiz=len;
        writeMemFromBuf(addr,wrSiz,buf);
        addr+=wrSiz;
        len-=wrSiz;
    }
}
void ThreadContext::readMemToBuf(VAddr addr, size_t len, void *buf) {
    I(canRead(addr,len));
    uint8_t *byteBuf=(uint8_t *)buf;
    while(len) {
        if((addr&sizeof(uint8_t))||(len<sizeof(uint16_t))) {
            *((uint8_t *)byteBuf)=readMemRaw<uint8_t>(addr);
            addr+=sizeof(uint8_t);
            byteBuf+=sizeof(uint8_t);
            len-=sizeof(uint8_t);
        } else if((addr&sizeof(uint16_t))||(len<sizeof(uint32_t))) {
            *((uint16_t *)byteBuf)=readMemRaw<uint16_t>(addr);
            addr+=sizeof(uint16_t);
            byteBuf+=sizeof(uint16_t);
            len-=sizeof(uint16_t);
        } else if((addr&sizeof(uint32_t))||(len<sizeof(uint64_t))) {
            *((uint32_t *)byteBuf)=readMemRaw<uint32_t>(addr);
            addr+=sizeof(uint32_t);
            byteBuf+=sizeof(uint32_t);
            len-=sizeof(uint32_t);
        } else {
            I(!(addr%sizeof(uint64_t)));
            I(len>=sizeof(uint64_t));
            *((uint64_t *)byteBuf)=readMemRaw<uint64_t>(addr);
            addr+=sizeof(uint64_t);
            byteBuf+=sizeof(uint64_t);
            len-=sizeof(uint64_t);
        }
    }
}
/*
ssize_t ThreadContext::readMemToFile(VAddr addr, size_t len, int32_t fd, bool natFile){
  I(canRead(addr,len));
  ssize_t retVal=0;
  uint8_t buf[AddressSpace::getPageSize()];
  while(len){
    size_t ioSiz=AddressSpace::getPageSize()-(addr&(AddressSpace::getPageSize()-1));
    if(ioSiz>len) ioSiz=len;
    readMemToBuf(addr,ioSiz,buf);
    ssize_t nowRet=-1;
    if(natFile)
      nowRet=write(fd,buf,ioSiz);
    else
      nowRet=openFiles->write(fd,buf,ioSiz);
    if(nowRet==-1)
      return nowRet;
    retVal+=nowRet;
    addr+=nowRet;
    len-=nowRet;
    if(nowRet<(ssize_t)ioSiz)
      break;
  }
  return retVal;
}
*/
ssize_t ThreadContext::readMemString(VAddr stringVAddr, size_t maxSize, char *dstStr) {
    size_t i=0;
    while(true) {
        if(!canRead(stringVAddr+i,sizeof(char)))
            return -1;
        char c=readMemRaw<char>(stringVAddr+i);
        if(i<maxSize)
            dstStr[i]=c;
        i++;
        if(c==(char)0)
            break;
    }
    return i;
}
#if (defined DEBUG_BENCH)
VAddr ThreadContext::readMemWord(VAddr addr) {
    return readMemRaw<VAddr>(addr);
}
#endif

void ThreadContext::execCall(VAddr entry, VAddr  ra, VAddr sp) {
    I(entry!=0x418968);
    // Unwind stack if needed
    while(!callStack.empty()) {
        if(sp<callStack.back().sp)
            break;
        if((sp==callStack.back().sp)&&(addressSpace->getFuncAddr(ra)==callStack.back().entry))
            break;
        callStack.pop_back();
    }
    bool tailr=(!callStack.empty())&&(sp==callStack.back().sp)&&(ra==callStack.back().ra);
    callStack.push_back(CallStackEntry(entry,ra,sp,tailr));
#ifdef DEBUG
    if(!callStack.empty()) {
        CallStack::reverse_iterator it=callStack.rbegin();
        while(it->tailr) {
            I(it!=callStack.rend());
            it++;
        }
        it++;
        I((it==callStack.rend())||(it->entry==addressSpace->getFuncAddr(ra))||(it->entry==addressSpace->getFuncAddr(ra-1)));
    }
#endif
}
void ThreadContext::execRet(VAddr entry, VAddr ra, VAddr sp) {
    while(callStack.back().sp!=sp) {
        I(callStack.back().sp<sp);
        callStack.pop_back();
    }
    while(callStack.back().tailr) {
        I(sp==callStack.back().sp);
        I(ra==callStack.back().ra);
        callStack.pop_back();
    }
    I(sp==callStack.back().sp);
    I(ra==callStack.back().ra);
    callStack.pop_back();
}
void ThreadContext::dumpCallStack(void) {
    printf("Call stack dump for thread %d begins\n",pid);
    for(size_t i=0; i<callStack.size(); i++)
        printf("  Entry 0x%08llx from 0x%08llx with sp 0x%08llx tail %d Name %s File %s\n",
               (unsigned long long)(callStack[i].entry),(unsigned long long)(callStack[i].ra),
               (unsigned long long)(callStack[i].sp),callStack[i].tailr,
               addressSpace->getFuncName(callStack[i].entry).c_str(),
               addressSpace->getFuncFile(callStack[i].entry).c_str());
    printf("Call stack dump for thread %d ends\n",pid);
}

void ThreadContext::clearCallStack(void) {
    printf("Clearing call stack for %d\n",pid);
    callStack.clear();
}

/// Keep track of statistics for each retired DInst.
void ThreadContext::markRetire(DInst* dinst) {
    nRetiredInsts++;

    const Instruction* inst = dinst->getInst();

    if(inst->isTM()) {
        traceTM(dinst);
        currentRegion.markRetireTM(dinst);
    }

    if(dinst->getTMMemopHadStalled()) {
        currentRegion.addNackStall(dinst, prevDInstRetired);
    }

    // Track function boundaries, by for example initializing and ending atomic regions.
    for(std::vector<FuncBoundaryData>::iterator i_funcData = dinst->funcData.begin();
            i_funcData != dinst->funcData.end(); ++i_funcData) {
        traceFunction(dinst, *i_funcData);

        switch(i_funcData->funcName) {
            case FUNC_TM_BEGIN:
                currentRegion.init(dinst->getInst()->getAddr(), globalClock);
                break;
            case FUNC_TM_END:
                currentRegion.markEnd(globalClock);
                currentRegion.calculate(&myTimeStats);
                currentRegion.clear();
                break;
            default:
                currentRegion.markRetireFuncBoundary(dinst, *i_funcData);
                break;
        } // end switch(funcName)
    } // end foreach funcBoundaryData

    prevDInstRetired = globalClock;
}

/// Trace function boundaries (call and return)
void ThreadContext::traceFunction(DInst *dinst, FuncBoundaryData& funcData) {
    char eventType = '?';
    switch(funcData.funcName) {
        case FUNC_PTHREAD_BARRIER:
            if(funcData.isCall) {
                eventType = 'B';
            } else {
                eventType = 'b';
            }
            break;
        case FUNC_TM_BEGIN:
            if(funcData.isCall) {
                eventType = 'S';
            }
            break;
        case FUNC_TM_BEGIN_FALLBACK:
            if(funcData.isCall) {
                eventType = 'F';
            } else {
                eventType = 'E';
            }
            break;
        case FUNC_TM_END_FALLBACK:
            if(funcData.isCall == false) {
                eventType = 'f';
            }
            break;
        case FUNC_TM_WAIT:
            if(funcData.isCall) {
                eventType = 'V';
            } else {
                eventType = 'v';
            }
            break;
        case FUNC_TM_END:
            if(funcData.isCall == false) {
                eventType = 's';
            }
            break;
        default:
            // Do nothing
            break;
    }
    if(eventType != '?') {
        std::ofstream& out = getTracefile();
        out << pid << ' ' << eventType;
        if(funcData.isCall) {
            out << " 0x" << hex << funcData.ra
                << " 0x" << funcData.arg0
                << " 0x" << funcData.arg1 << dec;
        } else {
            out << " 0x" << hex << funcData.rv << dec;
        }
        out << ' ' << nRetiredInsts << ' ' << globalClock << '\n';
    }
}

/// Trace TM related instructions
void ThreadContext::traceTM(DInst* dinst) {
    const Instruction *inst = dinst->getInst();

    if(dinst->tmAbortCompleteOp()) {
        // Get abort state
        const TMAbortState& abortState = tmCohManager->getAbortState(pid);
        Pid_t aborter           = abortState.getAborterPid();
        TMAbortType_e abortType = abortState.getAbortType();
        VAddr abortByAddr       = abortState.getAbortByAddr();
        VAddr abortIAddr        = abortState.getAbortIAddr();

        bool causedByFallback = ThreadContext::tmFallbackMutexCAddrs.find(abortByAddr)
                                   != ThreadContext::tmFallbackMutexCAddrs.end();

        // Trace this instruction
        if(abortType == TM_ATYPE_DEFAULT) {
            if(abortByAddr == 0) {
                fail("Why abort addr NULL?\n");
            }
            getTracefile()<<pid<<" A"
                            <<" 0x"<<std::hex<<abortIAddr<<std::dec
                            <<" 0x"<<std::hex<<abortByAddr<<std::dec
                            <<" "<<aborter
                            <<" "<< nRetiredInsts
                            <<" "<< globalClock << std::endl;
        } else if(causedByFallback) {
            getTracefile()<<pid<<" Z"
                            <<" 0x"<<std::hex<<abortIAddr<<std::dec
                            <<" 254"
                            <<" "<<aborter
                            <<" "<< nRetiredInsts
                            <<" "<< globalClock << std::endl;
        } else if(abortType == TM_ATYPE_NONTM) {
            if(abortByAddr == 0) {
                fail("Why abort addr NULL?\n");
            }
            getTracefile()<<pid<<" a"
                            <<" 0x"<<std::hex<<abortIAddr<<std::dec
                            <<" 0x"<<std::hex<<abortByAddr<<std::dec
                            <<" "<<aborter
                            <<" "<< nRetiredInsts
                            <<" "<< globalClock << std::endl;
        } else {
            uint32_t abortArg = 0;
            if(abortType == TM_ATYPE_USER) {
                abortArg = dinst->tmArg;
            } else {
                abortArg = abortByAddr;
            }
            getTracefile()<<pid<<" Z"
                            <<" 0x"<<std::hex<<abortIAddr<<std::dec
                            <<" "<<abortType
                            <<" 0x"<<std::hex<<abortArg<<std::dec
                            <<" "<< nRetiredInsts
                            <<" "<< globalClock << std::endl;
        }
    } else if(dinst->tmBeginOp()) {
        if(dinst->getTMBeginSubtype() == TM_BEGIN_REGULAR) {
            getTracefile()<<pid<<" T"
                        <<" 0x"<<std::hex<<dinst->tmCallsite<<std::dec
                        <<" "<<dinst->tmState.getUtid()
                        <<" "<<dinst->tmArg
                        <<" "<< nRetiredInsts
                        <<" "<< globalClock << std::endl;
        }
    } else if(dinst->tmCommitOp()) {
        if(dinst->getTMCommitSubtype() == TM_COMMIT_REGULAR) {
            getTracefile()<<pid<<" C"
                        <<" 0x"<<std::hex<<dinst->tmCallsite<<std::dec
                        <<" "<<(100-dinst->tmLat)
                        <<" "<<dinst->tmArg
                        <<" "<< nRetiredInsts
                        <<" "<< globalClock << std::endl;
        }
    }
}

void TimeTrackerStats::print() const {
    uint64_t totalOther = totalLengths -
        (totalCommitted + totalAborted + totalWait
            + totalMutexWait + totalMutex + totalNackStalled);
    std::cout << "Committed: " << totalCommitted << "\n";
    std::cout << "Aborted: " << totalAborted << "\n";
    std::cout << "WaitForMutex: " << totalMutexWait << "\n";
    std::cout << "InMutex: " << totalMutex << "\n";
    std::cout << "Wait: " << totalWait << "\n";
    std::cout << "NACKStall: " << totalNackStalled << "\n";
    std::cout << "Other: " << totalOther << "\n";
}

/// Add other to this statistics structure
void TimeTrackerStats::sum(const TimeTrackerStats& other) {
    totalLengths    += other.totalLengths;
    totalCommitted  += other.totalCommitted;
    totalAborted    += other.totalAborted;
    totalWait       += other.totalWait;
    totalMutexWait  += other.totalMutexWait;
    totalMutex      += other.totalMutex;
    totalNackStalled+= other.totalNackStalled;
}

/// If the DInst is at a function boundary, update statistics
void AtomicRegionStats::markRetireFuncBoundary(DInst* dinst, FuncBoundaryData& funcData) {
    switch(funcData.funcName) {
        case FUNC_TM_BEGIN_FALLBACK:
            if(funcData.isCall) {
                p_current = new CSSubregion(globalClock);
            } else {
                CSSubregion* p_cs = dynamic_cast<CSSubregion*>(p_current);
                p_cs->markAcquired(globalClock);
            }
            break;
        case FUNC_TM_END_FALLBACK:
            if(funcData.isCall) {
                p_current->markEnd(globalClock);
                subregions.push_back(p_current);
                p_current = NULL;
            }
            break;
        case FUNC_TM_WAIT:
            if(funcData.isCall) {
                p_current = new AtomicSubregion(AtomicSubregion::SR_WAIT, globalClock);
            } else {
                p_current->markEnd(globalClock);
                subregions.push_back(p_current);
                p_current = NULL;
            }
            break;
        default:
            // Do nothing
            break;
    } // end switch(funcName)
}

/// If the DInst is a TM instruction, update statistics
void AtomicRegionStats::markRetireTM(DInst* dinst) {
    Pid_t pid = dinst->context->getPid();

    if(dinst->tmAbortCompleteOp()) {
        TMSubregion* p_tm = dynamic_cast<TMSubregion*>(p_current);
        p_tm->markAborted(globalClock);
        subregions.push_back(p_current);
        p_current = NULL;
    } else if(dinst->tmBeginOp()) {
        p_current = new TMSubregion(globalClock);
    } else if(dinst->tmCommitOp()) {
        if(p_current == NULL) {
            fail("%d current is NULL when committing\n", pid);
        }
        p_current->markEnd(globalClock);
        subregions.push_back(p_current);
        p_current = NULL;
    }
}

void AtomicRegionStats::addNackStall(DInst* dinst, Time_t prevRetired) {
    Pid_t pid = dinst->context->getPid();
    if(p_current == NULL) {
        fail("[%d] Nacked MemOP outside of a atomic subregion?\n", pid);
    }
    p_current->totalNackStalled += globalClock - prevRetired;
}

/// Add this region's statistics to p_stats.
void AtomicRegionStats::calculate(TimeTrackerStats* p_stats) {
    // Make sure the stats are valid
    if(startPC == 0) {
        fail("Un-initialized region");
    }
    if(startAt == 0 || endAt == 0) {
        fail("Unclosed region");
    }

    // Compute total length
    p_stats->totalLengths += endAt - startAt;

    // Compute subregions
    Subregions::iterator i_subregion;
    for(i_subregion = subregions.begin(); i_subregion != subregions.end(); ++i_subregion) {
        AtomicSubregion* p_subregion = *i_subregion;
        switch(p_subregion->type) {
            case AtomicSubregion::SR_TRANSACTION: {
                TMSubregion* p_tm = dynamic_cast<TMSubregion*>(p_subregion);
                if(p_tm->aborted) {
                    p_stats->totalAborted   += p_tm->endAt - p_tm->startAt - p_tm->totalNackStalled;
                    p_stats->totalNackStalled += p_tm->totalNackStalled;
                } else {
                    p_stats->totalCommitted += p_tm->endAt - p_tm->startAt - p_tm->totalNackStalled;
                    p_stats->totalNackStalled += p_tm->totalNackStalled;
                }
                break;
            }
            case AtomicSubregion::SR_WAIT: {
                p_stats->totalWait +=  p_subregion->endAt - p_subregion->startAt;
                if(p_subregion->totalNackStalled > 0) {
                    fail("Wait region has NACK stall\n");
                }
                break;
            }
            case AtomicSubregion::SR_CRITICAL_SECTION: {
                CSSubregion* p_cs = dynamic_cast<CSSubregion*>(p_subregion);
                p_stats->totalMutexWait += p_cs->acquiredAt - p_cs->startAt;
                p_stats->totalMutex     += p_cs->endAt - p_cs->acquiredAt;
                if(p_subregion->totalNackStalled > 0) {
                    fail("CS has NACK stall\n");
                }
                break;
            }
            default:
                fail("Unhandled Subregion type\n");
        }
    } // end foreach subregion
}
void AtomicRegionStats::clear() {
    startPC = startAt = endAt = 0;
    p_current = NULL;
    while(subregions.size() > 0) {
        AtomicSubregion* p_subregion = subregions.back();
        subregions.pop_back();
        delete p_subregion;
    }
}
