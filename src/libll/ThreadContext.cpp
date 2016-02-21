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
std::set<uint32_t> ThreadContext::tmFallbackMutexCAddrs;
bool ThreadContext::simDone = false;
int64_t ThreadContext::finalSkip = 0;
bool ThreadContext::inMain = false;
std::ofstream ThreadContext::tracefile;
size_t ThreadContext::numThreads = 0;

void InstContext::clear() {
    wasHit      = false;
    setConflict = false;
    tmLat       = 0;
    tmArg       = 0;
    funcData.clear();

    tmBeginSubtype=TM_BEGIN_INVALID;
    tmCommitSubtype=TM_COMMIT_INVALID;
}

void ThreadContext::initialize(bool child) {
    nRetiredInsts = 0;
    nExedInsts = 0;
    spinning    = false;

#if (defined TM)
    tmAbortArg  = 0;
    tmContext   = NULL;
    tmDepth     = 0;
    tmlibUserTid= INVALID_USER_TID;
#endif

    ThreadContext::numThreads++;
}

void ThreadContext::cleanup() {
    ThreadContext::numThreads--;
}

void ThreadContext::openTraceFile() {
    char filename[256];
    sprintf(filename, "datafile.out");
    tracefile.open(filename);
}

void ThreadContext::closeTraceFile() {
    TimeTrackerStats allTimerStats;

    for(ThreadContext* c: ThreadContext::pid2context) {
        allTimerStats.sum(c->timeStats);
    }

    allTimerStats.print();
    if(getTracefile().is_open()) {
        getTracefile().close();
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
        fail("[%d] Transaction nesting not complete\n", pid);
    }
    TMBCStatus status = htmManager->begin(inst, this, &instContext);
    if(instContext.tmBeginSubtype == TM_BEGIN_INVALID) {
        fail("tmBeginSubtype invalid\n");
    }
    switch(status) {
        case TMBC_SUCCESS: {
            tmAbortArg  = 0;

            uint64_t utid = htmManager->getUtid(pid);
            tmContext   = new TMContext(this, inst, utid);
            tmContext->saveContext();
            saveCallRetStack();
            tmDepth++;

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
    uint64_t utid = htmManager->getUtid(pid);
    size_t numWrites = htmManager->getNumWrites(pid);

    TMBCStatus status = htmManager->commit(inst, this, &instContext);
    if(instContext.tmCommitSubtype == TM_COMMIT_INVALID) {
        fail("tmCommitSubtype invalid\n");
    }
    switch(status) {
        case TMBC_ABORT: {
            // In the case of a Lazy model where we are forced to Abort
            abortTransaction(inst);
            break;
        }
        case TMBC_SUCCESS: {
            // If we have already delayed, go ahead and finalize commit in memory
            tmContext->flushMemory();

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
void ThreadContext::userAbortTM(InstDesc* inst, uint32_t arg) {
    instContext.tmArg = arg;
    tmAbortArg        = arg;
    htmManager->markUserAbort(inst, this, arg);

    abortTransaction(inst);
}
void ThreadContext::syscallAbortTM(InstDesc* inst) {
    htmManager->markSyscallAbort(inst, this);

    abortTransaction(inst);
}

TMBCStatus ThreadContext::abortTransaction(InstDesc* inst) {
    if(tmContext == NULL) {
        fail("Abort fail: tmContext is NULL\n");
    }

    // Save UTID before aborting
    uint64_t utid = htmManager->getUtid(pid);

    TMBCStatus status = htmManager->abort(inst, this, &instContext);
    switch(status) {
        case TMBC_SUCCESS: {
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
    htmManager->completeAbort(inst, this, &instContext);
}

uint32_t ThreadContext::getAbortRV() {
    // Get abort state
    const TMAbortState& abortState = htmManager->getAbortState(pid);

    // LSB is 1 to show that this is an abort
    uint32_t abortRV = 1;

    // bottom 8 bits are reserved
    abortRV |= tmAbortArg << 8;
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
    VAddr mutexCAddr = htmManager->addrToCacheLine(pFallbackMutex);
    ThreadContext::tmFallbackMutexCAddrs.insert(mutexCAddr);
    htmManager->beginFallback(pid);
}

void ThreadContext::completeFallback() {
    htmManager->completeFallback(pid);
}

#endif

ThreadContext *ThreadContext::getContext(Pid_t pid)
{
    I(pid>=0);
    I((size_t)pid<pid2context.size());
    return pid2context[pid];
}

void ThreadContext::printPCs(void)
{
    for(ThreadContext* c: ThreadContext::pid2context) {
        printf("[%d]: 0x%lx (%ld/%ld)\n", c->getPid(), c->getIAddr(), c->getNExedInsts(), c->getNRetiredInsts());
    }
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
        currentRegion.markRetireTM(dinst);
    }

    // Track function boundaries, by for example initializing and ending atomic regions.
    for(std::vector<FuncBoundaryData>::const_iterator i_funcData = dinst->getInstContext().funcData.begin();
            i_funcData != dinst->getInstContext().funcData.end(); ++i_funcData) {
        switch(i_funcData->funcName) {
            case FUNC_TM_BEGIN:
                currentRegion.init(pid, dinst->getInst()->getAddr(), globalClock);
                break;
            case FUNC_TM_END: {
                TimeTrackerStats myTimeStats;
                currentRegion.markEnd(globalClock);
                currentRegion.calculate(&myTimeStats);
                currentRegion.clear();

                timeStats.sum(myTimeStats);
                break;
            }
            default:
                currentRegion.markRetireFuncBoundary(dinst, *i_funcData);
                break;
        } // end switch(funcName)
    } // end foreach funcBoundaryData
}

TimeTrackerStats::TimeTrackerStats():
    duration(0),
    inMutex(0),
    mutexQueue(0),
    committed(0),
    aborted(0),
    activeFBWait(0),
    backoffWait(0) {
}
uint64_t TimeTrackerStats::totalAccounted() const {
    return inMutex
        + mutexQueue
        + committed
        + aborted
        + activeFBWait
        + backoffWait
    ;
}

void TimeTrackerStats::print() const {
    if(totalAccounted() > duration) {
        fail("Accounted cycles is too high");
    }

    uint64_t totalOther = duration - totalAccounted();

    std::cout << "InMutex: "        << inMutex      << "\n";
    std::cout << "MutexQueue: "     << mutexQueue   << "\n";
    std::cout << "Committed: "      << committed    << "\n";
    std::cout << "Aborted: "        << aborted      << "\n";
    std::cout << "ActiveFBWait: "   << activeFBWait << "\n";
    std::cout << "BackoffWait: "    << backoffWait  << "\n";
    std::cout << "Other: "          << totalOther   << std::endl;
}

/// Add other to this statistics structure
void TimeTrackerStats::sum(const TimeTrackerStats& other) {
    duration        += other.duration;
    inMutex         += other.inMutex;
    mutexQueue      += other.mutexQueue;
    committed       += other.committed;
    aborted         += other.aborted;
    activeFBWait    += other.activeFBWait;
    backoffWait     += other.backoffWait;
}

// Uninitialize stats structure
void AtomicRegionStats::clear() {
    pid = INVALID_PID;
    startPC = startAt = endAt = 0;
    events.clear();
}
// Initalize status structure
void AtomicRegionStats::init(Pid_t p, VAddr pc, Time_t at) {
    clear();
    pid     = p;
    startPC = pc;
    startAt = at;
}
// Create new AtomicRegion event
void AtomicRegionStats::newAREvent(enum AREventType type) {
    AtomicRegionEvents newEvent(type, globalClock);
    events.push_back(newEvent);
}

/// If the DInst is at a function boundary, update statistics
void AtomicRegionStats::markRetireFuncBoundary(DInst* dinst, const FuncBoundaryData& funcData) {
    switch(funcData.funcName) {
        case FUNC_TM_BEGIN_FALLBACK:
            if(funcData.isCall) {
                newAREvent(AR_EVENT_LOCK_REQUEST);
            } else {
                newAREvent(AR_EVENT_LOCK_ACQUIRE);
            }
            break;
        case FUNC_TM_END_FALLBACK:
            if(funcData.isCall) {
                newAREvent(AR_EVENT_LOCK_RELEASE);
            }
            break;
        case FUNC_TM_WAIT:
            if(funcData.isCall) {
                if(funcData.arg0 == 0) {
                    newAREvent(AR_EVENT_ACTIVEFB_WAIT_BEGIN);
                } else if(funcData.arg0 == 1) {
                    newAREvent(AR_EVENT_BACKOFF_BEGIN);
                } else {
                }
            } else {
                newAREvent(AR_EVENT_WAIT_END);
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
        newAREvent(AR_EVENT_HTM_ABORT);
    } else if(dinst->tmBeginOp()) {
        switch(dinst->getTMBeginSubtype()) {
            case TM_BEGIN_REGULAR:
                newAREvent(AR_EVENT_HTM_BEGIN);
                break;
            default:
                fail("Unhandled tmBeginSubtype: %d\n", dinst->getTMBeginSubtype());
        }
    } else if(dinst->tmCommitOp()) {
        switch(dinst->getTMCommitSubtype()) {
            case TM_COMMIT_REGULAR:
                newAREvent(AR_EVENT_HTM_COMMIT);
                break;
            case TM_COMMIT_ABORTED:
                newAREvent(AR_EVENT_HTM_ABORT);
                break;
            default:
                fail("Unhandled tmCommitSubtype: %d\n", dinst->getTMCommitSubtype());
        }
    }
}

// Print all the events stored in an AtomicRegion, with ``current'' marked
void AtomicRegionStats::printEvents(const AtomicRegionEvents& current) const {
    std::cout << pid << ": [B 0], ";
    for(size_t eid = 0; eid < events.size(); eid++) {
        const AtomicRegionEvents& event = events.at(eid);
        if(current.getTimestamp() == event.getTimestamp()) {
            std::cout << "{" << event.getType() << " " << (event.getTimestamp() - startAt) << "}, ";
        } else {
            std::cout << "[" << event.getType() << " " << (event.getTimestamp() - startAt) << "], ";
        }
    }
    std::cout << "[E " << (endAt - startAt) << "]";
    std::cout << std::endl;
}

/// Add this region's statistics to p_stats.
void AtomicRegionStats::calculate(TimeTrackerStats* p_stats) {
    // Make sure the stats are valid
    if(startPC == 0) {
        fail("Un-initialized region\n");
    }
    if(startAt == 0 || endAt == 0) {
        fail("Unclosed region\n");
    }

    // Handle subregions events
    size_t eid = 0;
    size_t INVALID_EID = events.size() + 1;
    size_t htm_begin_eid = INVALID_EID;
    while(eid < events.size()) {
        const AtomicRegionEvents& event = events.at(eid);
        switch(event.getType()) {
            case AR_EVENT_HTM_BEGIN: {
                htm_begin_eid = eid;
                eid += 1;
                break;
            }
            case AR_EVENT_HTM_ABORT: {
                if(htm_begin_eid == INVALID_EID) {
                    printEvents(event);
                    fail("[%d] abort should be proceeded by begin\n", pid);
                }
                const AtomicRegionEvents& beginEvent = events.at(htm_begin_eid);

                p_stats->aborted   += event.getTimestamp() - beginEvent.getTimestamp();

                htm_begin_eid = INVALID_EID;
                eid += 1;
                break;
            }
            case AR_EVENT_HTM_COMMIT: {
                if(htm_begin_eid == INVALID_EID) {
                    printEvents(event);
                    fail("[%d] commit should be proceeded by begin\n", pid);
                }
                const AtomicRegionEvents& beginEvent = events.at(htm_begin_eid);

                p_stats->committed += event.getTimestamp() - beginEvent.getTimestamp();

                htm_begin_eid = INVALID_EID;
                eid += 1;
                break;
            }
            case AR_EVENT_LOCK_REQUEST: {
                // Lock Request/Acquire/Release
                if(eid + 2 >= events.size()) {
                    fail("Lock end event is not found\n");
                }
                const AtomicRegionEvents& lockAcqEvent = events.at(eid + 1);
                const AtomicRegionEvents& lockRelEvent = events.at(eid + 2);
                if(lockAcqEvent.getType() != AR_EVENT_LOCK_ACQUIRE) {
                    fail("Unknown event after lock request: %d\n", lockAcqEvent.getType());
                }
                if(lockRelEvent.getType() != AR_EVENT_LOCK_RELEASE) {
                    fail("Unknown event after lock release: %d\n", lockRelEvent.getType());
                }

                p_stats->mutexQueue += lockAcqEvent.getTimestamp() - event.getTimestamp();
                p_stats->inMutex    += lockRelEvent.getTimestamp() - lockAcqEvent.getTimestamp();
                eid += 3;
                break;
            }
            case AR_EVENT_ACTIVEFB_WAIT_BEGIN: {
                // Wait Events
                if(eid + 1 >= events.size()) {
                    fail("wait end event is not found\n");
                }
                const AtomicRegionEvents& endEvent = events.at(eid + 1);
                if(endEvent.getType() != AR_EVENT_WAIT_END) {
                    fail("Unknown event after wait begin: %d\n", endEvent.getType());
                }
                p_stats->activeFBWait   +=  endEvent.getTimestamp() - event.getTimestamp();
                eid += 2;
                break;
            }
            case AR_EVENT_BACKOFF_BEGIN: {
                // Wait Events
                if(eid + 1 >= events.size()) {
                    fail("wait end event is not found\n");
                }
                const AtomicRegionEvents& endEvent = events.at(eid + 1);
                if(endEvent.getType() != AR_EVENT_WAIT_END) {
                    fail("Unknown event after backoff begin: %d\n", endEvent.getType());
                }
                p_stats->backoffWait +=  endEvent.getTimestamp() - event.getTimestamp();
                eid += 2;
                break;
            }
            default:
                fail("Unknown event: %d\n", event.getType());
        }
    }

    // Compute total length
    p_stats->duration += endAt - startAt;

    if(p_stats->totalAccounted() > p_stats->duration) {
        printEvents(events.at(0));
        fail("[%d] Accounted cycles is too high", pid);
    }
}
