#ifndef DRAM_H
#define DRAM_H

#include "libcore/MemObj.h"
#include "libmem/MemorySystem.h"
#include "libcore/MemRequest.h"
#include "GStats.h"

#include "DRAMSim.h"


#include <stdint.h>

#include <map>
#include <list>

class DRAM : public MemObj {
private:
	int blockSize;
	int dramSize;
	uint64_t cpuClock;

	DRAMSim::MultiChannelMemorySystem *dram;

	GStatsCntr readHit;
    GStatsCntr writeHit;
    GStatsCntr linePush;

    // interface with upper level
    void readwrite(MemRequest *mreq, bool write);
    void specialOp(MemRequest *mreq);

	static DRAM *self;

	void printStat();
	void doEveryCycle();

	std::map<VAddr, std::list<MemRequest *> > dramReqs;
    
	void doReadWrite(MemRequest *mreq, bool write);
   
	typedef CallbackMember2<DRAM, MemRequest *, bool, 
            &DRAM::doReadWrite> doReadWriteCB;
    
protected:

public:
    DRAM(MemorySystem *gms, const char *section, const char *name);
    ~DRAM();

    // BEGIN MemObj interface

    // port usage accounting
    Time_t getNextFreeCycle() const { return globalClock; };

    // interface with upper level
    bool canAcceptStore(PAddr addr) { return true; };
    void access(MemRequest *mreq);

    // interface with lower level
    void returnAccess(MemRequest *mreq) { IJ(0); };
    void invalidate(PAddr addr, ushort size, MemObj *oc) { IJ(0); }

    // END MemObj interface
	
	void access_complete(unsigned id, uint64_t address, uint64_t clock_cycle);

	static void PrintStat();
	static void update();
};
	
void power_callback(double a, double b, double c, double d);

#endif
