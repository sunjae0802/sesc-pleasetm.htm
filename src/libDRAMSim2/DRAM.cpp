#include "DRAM.h"

#include <iostream>
#include <fstream>

using namespace std;
using namespace DRAMSim;

DRAM* DRAM::self = NULL;

DRAM::DRAM(MemorySystem *gms, const char *section, const char *name)
    : MemObj(section, name)
    ,readHit("%s:readHit", name)
    ,writeHit("%s:writeHit", name)
    ,linePush("%s:linePush", name)
{
	blockSize = SescConf->getInt(section, "Bsize");
	dramSize = SescConf->getInt(section, "size");
	cpuClock = (uint64_t)SescConf->getDouble("cpucore", "frequency",0);

	cout<<"[DRAM] Config block:"<<blockSize<<"B size:"<<(dramSize/1000.0)<<"G cpu:"<<(cpuClock/1000000)<<"MHz"<<endl;
	
	
	SescConf->isCharPtr(section, "dramsim2_dev_ini");
	SescConf->isCharPtr(section, "dramsim2_sys_ini");
	SescConf->isCharPtr(section, "dramsim2_output");

	const char *dram_dev_config = SescConf->getCharPtr(section, "dramsim2_dev_ini");
	const char *dram_sys_config = SescConf->getCharPtr(section, "dramsim2_sys_ini");
	const char *dram_output = SescConf->getCharPtr(section, "dramsim2_output");

	string fdir=SescConf->getConfDir();
	string dram_dev_conf(dram_dev_config);
	string dram_sys_conf(dram_sys_config);
	if(fdir.size()>0) {
		dram_dev_conf = fdir+"/"+dram_dev_conf;
		dram_sys_conf = fdir+"/"+dram_sys_conf;
	}

	//std::cout<<"Config: "<<dram_dev_conf<<"\n\t"<<dram_sys_conf<<endl;

	TransactionCompleteCB *access_cb = new Callback<DRAM, void, unsigned, uint64_t, uint64_t>(this, &DRAM::access_complete);

	dram = getMemorySystemInstance(dram_dev_conf.c_str(), dram_sys_conf.c_str(), "", dram_output, dramSize); 
	dram->RegisterCallbacks(access_cb, access_cb, power_callback);
	dram->setCPUClockSpeed(cpuClock);

	dramReqs.clear();

	self = this;
}

DRAM::~DRAM()
{
	delete dram;
    // do nothing
}

void power_callback(double a, double b, double c, double d) {
}

void DRAM::access(MemRequest *mreq)
{
    mreq->setClockStamp((Time_t) - 1);
    if(mreq->getPAddr() <= 1024) { // TODO: need to implement support for fences
        mreq->goUp(0);
        return;
    }

    switch(mreq->getMemOperation()) {
    case MemReadW:
    case MemRead:
    	readHit.inc();
		readwrite(mreq, false);
        //read(mreq);
        break;
    case MemWrite:
    	writeHit.inc();
		readwrite(mreq, true);
        //write(mreq);
        break;
    case MemPush:
    	linePush.inc();
		readwrite(mreq, true);
        //pushLine(mreq);
        break;
    default:
        specialOp(mreq);
        break;
    }
}

void DRAM::access_complete(unsigned id, uint64_t address, uint64_t clock_cycle)
{
	IJ(dramReqs.find(address)!=dramReqs.end());
	list<MemRequest *> &rq = dramReqs[address];
	for(list<MemRequest *>::iterator it = rq.begin();it!=rq.end();it++) {
		MemRequest *mreq = (*it);
    	mreq->goUp(1);
		//cout<<"read done "<<mreq->getPAddr()<<" at "<<globalClock<<endl;
	}
	rq.clear();
	dramReqs.erase(address);
}

void DRAM::readwrite(MemRequest *mreq, bool write)
{
	//cout<<"read "<<mreq->getPAddr()<<" at "<<globalClock<<endl;

	uint64_t addr = (uint64_t)mreq->getPAddr();

	if(dramReqs.find(addr)!=dramReqs.end()) {
		dramReqs[addr].push_back(mreq);
		return;
	}
	//IJ(dramReqs[addr].empty());
	dramReqs[addr].push_back(mreq);

	bool accepted = dram->addTransaction(write, addr);
	if(!accepted) {
    	doReadWriteCB::scheduleAbs(globalClock+1, this, mreq, write);
	}
}
	
void DRAM::doReadWrite(MemRequest *mreq, bool write) {
	uint64_t addr = (uint64_t)mreq->getPAddr();

	if(dramReqs.find(addr)!=dramReqs.end()) {
		dramReqs[addr].push_back(mreq);
		return;
	}
	dramReqs[addr].push_back(mreq);

	bool accepted = dram->addTransaction(write, addr);
	if(!accepted) {
    	doReadWriteCB::scheduleAbs(globalClock+1, this, mreq, write);
	}
}

void DRAM::specialOp(MemRequest *mreq)
{
	IJ(0);
}

void DRAM::PrintStat() {
	if(self) {
		self->printStat();
	}
}

void DRAM::update() {
	if(self) {
		self->doEveryCycle();
	}
}

void DRAM::printStat() {
	IJ(dram);
	dram->printStats(true);
}

void DRAM::doEveryCycle() {
	dram->update();
}

