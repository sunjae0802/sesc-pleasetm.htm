#ifndef THREADCONTEXT_H
#define THREADCONTEXT_H

#include <stdint.h>
#include <cstring>
#include <vector>
#include <set>
#include "Snippets.h"
#include "libemul/AddressSpace.h"
#include "libemul/SignalHandling.h"
#include "libemul/FileSys.h"
#include "libemul/InstDesc.h"
#include "libemul/LinuxSys.h"

// Use this define to debug the simulated application
// It enables call stack tracking
//#define DEBUG_BENCH

class ThreadContext : public GCObject {
public:
    typedef SmartPtr<ThreadContext> pointer;
    static bool ff;
    static bool simDone;
    static Time_t resetTS;
private:
    void initVar();
    typedef std::vector<pointer> ContextVector;
    // Static variables
    static ContextVector pid2context;

    // Memory Mapping

    // Lower and upper bound for stack addresses in this thread
    VAddr myStackAddrLb;
    VAddr myStackAddrUb;

    // Local Variables
private:
    int32_t pid;		// process id

    // Execution mode of this thread
    ExecMode execMode;
    // Register file(s)
    RegVal regs[NumOfRegs];
    // Address space for this thread
    AddressSpace::pointer addressSpace;
    // Instruction pointer
    VAddr     iAddr;
    // Instruction descriptor
    InstDesc *iDesc;
    // Virtual address generated by the last memory access instruction
    VAddr     dAddr;
    size_t    nDInsts;

private:

public:
    static inline int32_t getPidUb(void) {
        return pid2context.size();
    }
    void setMode(ExecMode mode);
    inline ExecMode getMode(void) const {
        return execMode;
    }

    inline const void *getReg(RegName name) const {
        return &(regs[name]);
    }
    inline void *getReg(RegName name) {
        return &(regs[name]);
    }
    void clearRegs(void) {
        memset(regs,0,sizeof(regs));
    }
    void save(ChkWriter &out) const;

    // Returns the pid of the context
    Pid_t getPid(void) const {
        return pid;
    }

    void copy(const ThreadContext *src);

    static ThreadContext *getContext(Pid_t pid);

    static ThreadContext *getMainThreadContext(void) {
        return &(*(pid2context[0]));
    }

    // BEGIN Memory Mapping
    bool isValidDataVAddr(VAddr vaddr) const {
        return canRead(vaddr,1)||canWrite(vaddr,1);
    }

    ThreadContext(FileSys::FileSys *fileSys);
    ThreadContext(ThreadContext &parent, bool cloneParent,
                  bool cloneFileSys, bool newNameSpace,
                  bool cloneFiles, bool cloneSighand,
                  bool cloneVm, bool cloneThread,
                  SignalID sig, VAddr clearChildTid);
    ThreadContext(ChkReader &in);
    ~ThreadContext();

    ThreadContext *createChild(bool shareAddrSpace, bool shareSigTable, bool shareOpenFiles, SignalID sig);
    void setAddressSpace(AddressSpace *newAddressSpace);
    AddressSpace *getAddressSpace(void) const {
        I(addressSpace);
        return addressSpace;
    }
    inline void setStack(VAddr stackLb, VAddr stackUb) {
        myStackAddrLb=stackLb;
        myStackAddrUb=stackUb;
    }
    inline VAddr getStackAddr(void) const {
        return myStackAddrLb;
    }
    inline VAddr getStackSize(void) const {
        return myStackAddrUb-myStackAddrLb;
    }

    inline InstDesc *virt2inst(VAddr vaddr) {
        InstDesc *inst=addressSpace->virtToInst(vaddr);
        if(!inst) {
            addressSpace->createTrace(this,vaddr);
            inst=addressSpace->virtToInst(vaddr);
        }
        return inst;
    }

    bool isLocalStackData(VAddr addr) const {
        return (addr>=myStackAddrLb)&&(addr<myStackAddrUb);
    }

    VAddr getStackTop() const {
        return myStackAddrLb;
    }
    // END Memory Mapping

    inline InstDesc *getIDesc(void) const {
        return iDesc;
    }
    inline void updIDesc(ssize_t ddiff) {
        I((ddiff>=-1)&&(ddiff<4));
        iDesc+=ddiff;
    }
    inline VAddr getIAddr(void) const {
        return iAddr;
    }
    inline void setIAddr(VAddr addr) {
        iAddr=addr;
        iDesc=iAddr?virt2inst(addr):0;
    }
    inline void updIAddr(ssize_t adiff, ssize_t ddiff) {
        I((ddiff>=-1)&&(ddiff<4));
        I((adiff>=-4)&&(adiff<=8));
        iAddr+=adiff;
        iDesc+=ddiff;
    }
    inline VAddr getDAddr(void) const {
        return dAddr;
    }
    inline void setDAddr(VAddr addr) {
        dAddr=addr;
    }
    inline void addDInst(void) {
        nDInsts++;
    }
    inline void delDInst(void) {
        nDInsts--;
    }
    inline size_t getNDInsts(void) {
        return nDInsts;
    }
    static inline int32_t nextReady(int32_t startPid) {
        int32_t foundPid=startPid;
        do {
            if(foundPid==(int)(pid2context.size()))
                foundPid=0;
            ThreadContext *context=pid2context[foundPid];
            if(context&&(!context->isSuspended())&&(!context->isExited()))
                return foundPid;
            foundPid++;
        } while(foundPid!=startPid);
        return -1;
    }
    inline bool skipInst(void);
    static int64_t skipInsts(int64_t skipCount);
#if (defined HAS_MEM_STATE)
    inline const MemState &getState(VAddr addr) const {
        return addressSpace->getState(addr);
    }
    inline MemState &getState(VAddr addr) {
        return addressSpace->getState(addr);
    }
#endif
    inline bool canRead(VAddr addr, size_t len) const {
        return addressSpace->canRead(addr,len);
    }
    inline bool canWrite(VAddr addr, size_t len) const {
        return addressSpace->canWrite(addr,len);
    }
    void    writeMemFromBuf(VAddr addr, size_t len, const void *buf);
//  ssize_t writeMemFromFile(VAddr addr, size_t len, int32_t fd, bool natFile, bool usePread=false, off_t offs=0);
    void    writeMemWithByte(VAddr addr, size_t len, uint8_t c);
    void    readMemToBuf(VAddr addr, size_t len, void *buf);
//  ssize_t readMemToFile(VAddr addr, size_t len, int32_t fd, bool natFile);
    ssize_t readMemString(VAddr stringVAddr, size_t maxSize, char *dstStr);
    template<class T>
    inline T readMemRaw(VAddr addr) {
        if(sizeof(T)>sizeof(MemAlignType)) {
            fail("ThreadContext:writeMemRaw with a too-large type\n");
//      T tmp;
//      I(canRead(addr,sizeof(T)));
//      readMemToBuf(addr,sizeof(T),&tmp);
//      return tmp;
        }
//    for(size_t i=0;i<(sizeof(T)+MemState::Granularity-1)/MemState::Granularity;i++)
//      if(getState(addr+i*MemState::Granularity).st==0)
//        fail("Uninitialized read found\n");
        return addressSpace->read<T>(addr);
    }
    template<class T>
    inline void writeMemRaw(VAddr addr, const T &val) {
        //   if((addr>=0x4d565c)&&(addr<0x4d565c+12)){
        //     I(0);
        //     I(iAddr!=0x004bb428);
        //     I(iAddr!=0x004c8604);
        //     const char *fname="Unknown";
        //     if(iAddr)
        //       fname=getAddressSpace()->getFuncName(getAddressSpace()->getFuncAddr(iAddr));
        //     printf("Write 0x%08x to 0x%08x at 0x%08x in %s\n",
        //       val,addr,iAddr,fname);
        //   }
        if(sizeof(T)>sizeof(MemAlignType)) {
            fail("ThreadContext:writeMemRaw with a too-large type\n");
//      if(!canWrite(addr,sizeof(val)))
//	return false;
//      writeMemFromBuf(addr,sizeof(val),&val);
//      return true;
        }
//    for(size_t i=0;i<(sizeof(T)+MemState::Granularity-1)/MemState::Granularity;i++)
//      getState(addr+i*MemState::Granularity).st=1;
        addressSpace->write<T>(addr,val);
    }
#if (defined DEBUG_BENCH)
    VAddr readMemWord(VAddr addr);
#endif

    //
    // File system
    //
private:
    FileSys::FileSys::pointer fileSys;
    FileSys::OpenFiles::pointer openFiles;
public:
    FileSys::FileSys *getFileSys(void) const {
        return fileSys;
    }
    FileSys::OpenFiles *getOpenFiles(void) const {
        return openFiles;
    }

    //
    // Signal handling
    //
private:
    SignalTable::pointer sigTable;
    SignalSet   sigMask;
    SignalQueue maskedSig;
    SignalQueue readySig;
    bool        suspSig;
public:
    void setSignalTable(SignalTable *newSigTable) {
        sigTable=newSigTable;
    }
    SignalTable *getSignalTable(void) const {
        return sigTable;
    }
    void suspend(void);
    void signal(SigInfo *sigInfo);
    void resume(void);
    const SignalSet &getSignalMask(void) const {
        return sigMask;
    }
    void setSignalMask(const SignalSet &newMask) {
        sigMask=newMask;
        for(size_t i=0; i<maskedSig.size(); i++) {
            SignalID sig=maskedSig[i]->signo;
            if(!sigMask.test(sig)) {
                readySig.push_back(maskedSig[i]);
                maskedSig[i]=maskedSig.back();
                maskedSig.pop_back();
            }
        }
        for(size_t i=0; i<readySig.size(); i++) {
            SignalID sig=readySig[i]->signo;
            if(sigMask.test(sig)) {
                maskedSig.push_back(readySig[i]);
                readySig[i]=readySig.back();
                readySig.pop_back();
            }
        }
        if((!readySig.empty())&&suspSig)
            resume();
    }
    bool hasReadySignal(void) const {
        return !readySig.empty();
    }
    SigInfo *nextReadySignal(void) {
        I(hasReadySignal());
        SigInfo *sigInfo=readySig.back();
        readySig.pop_back();
        return sigInfo;
    }

    // System state

    LinuxSys *mySystem;
    LinuxSys *getSystem(void) const {
        return mySystem;
    }

    // Parent/Child relationships
private:
    typedef std::set<int> IntSet;
    // Thread id of this thread
    int32_t tid;
    // tid of the thread group leader
    int32_t tgid;
    // This set is empty for threads that are not thread group leader
    // In a thread group leader, this set contains the other members of the thread group
    IntSet tgtids;

    // Process group Id is the PId of the process group leader
    int32_t pgid;

    int parentID;
    IntSet childIDs;
    // Signal sent to parent when this thread dies/exits
    SignalID  exitSig;
    // Futex to clear when this thread dies/exits
    VAddr clear_child_tid;
    // Robust list head pointer
    VAddr robust_list;
public:
    int32_t gettgid(void) const {
        return tgid;
    }
    size_t gettgtids(int tids[], size_t slots) const {
        IntSet::const_iterator it=tgtids.begin();
        for(size_t i=0; i<slots; i++,it++)
            tids[i]=*it;
        return tgtids.size();
    }
    int32_t gettid(void) const {
        return tid;
    }
    int32_t getpgid(void) const {
        return pgid;
    }
    int getppid(void) const {
        return parentID;
    }
    void setRobustList(VAddr headptr) {
        robust_list=headptr;
    }
    void setTidAddress(VAddr tidptr) {
        clear_child_tid=tidptr;
    }
    int32_t  getParentID(void) const {
        return parentID;
    }
    bool hasChildren(void) const {
        return !childIDs.empty();
    }
    bool isChildID(int32_t id) const {
        return (childIDs.find(id)!=childIDs.end());
    }
    int32_t findZombieChild(void) const;
    SignalID getExitSig(void) {
        return exitSig;
    }
private:
    bool     exited;
    int32_t      exitCode;
    SignalID killSignal;
public:
    bool isSuspended(void) const {
        return suspSig;
    }
    bool isExited(void) const {
        return exited;
    }
    int32_t getExitCode(void) const {
        return exitCode;
    }
    bool isKilled(void) const {
        return (killSignal!=SigNone);
    }
    SignalID getKillSignal(void) const {
        return killSignal;
    }
    // Exit this process
    // Returns: true if exit complete, false if process is now zombie
    bool exit(int32_t code);
    // Reap an exited process
    void reap();
    void doKill(SignalID sig) {
        I(!isExited());
        I(!isKilled());
        I(sig!=SigNone);
        killSignal=sig;
    }

    // Debugging

    class CallStackEntry {
    public:
        VAddr entry;
        VAddr ra;
        VAddr sp;
        bool  tailr;
        CallStackEntry(VAddr entry, VAddr  ra, VAddr sp, bool tailr)
            : entry(entry), ra(ra), sp(sp), tailr(tailr) {
        }
    };
    typedef std::vector<CallStackEntry> CallStack;
    CallStack callStack;

    void execCall(VAddr entry, VAddr  ra, VAddr sp);
    void execRet(VAddr entry, VAddr ra, VAddr sp);
    void dumpCallStack(void);
    void clearCallStack(void);

public:
    int numThreads;
    int maxThreads;

    void incParallel() {
        std::cout<<"["<<globalClock<<"]   Thread "<<numThreads<<" Create"<<std::endl<<std::flush;
        numThreads++;
        maxThreads = numThreads;
    }

    void decParallel() {
        numThreads--;
        std::cout<<"["<<globalClock<<"]   Thread "<<numThreads<<" Exit"<<std::endl<<std::flush;
        //std::cout<<"\t[ Thread dec "<<numThreads<<" ]"<<std::endl<<std::flush;
    }

};

#endif // THREADCONTEXT_H
