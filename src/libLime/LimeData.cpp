#include <iostream>
#include <iterator> //for std::ostream_iterator
#include <algorithm> //for std::copy

#include "nanassert.h"
#include "LimeData.h"


LimeData *limeData = NULL;
		
int64_t LimeData::seq = 0;

pool<LimeData::ParInfo> LimeData::ParInfo::parInfoPool(256, "LimeParInfo");

LimeData::LimeData(const char *outFile, const char *bench) :
	joinCalled(false),
	curPar(0),
	tsPar(256, 0),
	pBr(256, 0),
	outFileName(outFile)
{
	I(outFileName!=NULL);
	outfile.open(outFileName);
	if(!outfile.is_open()) {
		I(0);
	}

	outfile<<LM_PROG_NAME<<std::endl;
	outfile<<bench<<std::endl;

	edgeCntMap.resize(256);
	memCntMap.resize(256);
	memL1MissCntMap.resize(256);
	memL2MissCntMap.resize(256);

	//std::cout<<"Out: "<<outFileName<<std::endl;
}

LimeData::~LimeData() 
{
}

void LimeData::finalReport()
{
	if(outfile.is_open()) {
		outfile<<LM_PER_THREAD_TIME<<std::endl;

		outfile<<tsProgramBeginEnd.first<<std::endl;
		outfile<<tsProgramBeginEnd.second<<std::endl;

		for(std::map<Pid_t, std::vector<Time_t> >::iterator it = tsThreadBeginEnd.begin();
				it!=tsThreadBeginEnd.end();it++) {
			Pid_t t = it->first;
			outfile<<t<<" "<<it->second.size()<<std::endl;
			std::copy(it->second.begin(), it->second.end(), std::ostream_iterator<Time_t>(outfile, "\n"));			
		}
		outfile.close();
	}
}

void LimeData::setupBegin(Pid_t pid)
{
	tsPar[pid] = globalClock;
	if(tsThreadBeginEnd.find(pid)==tsThreadBeginEnd.end()) {
		tsThreadBeginEnd[pid].clear();
	}
	tsThreadBeginEnd[pid].push_back(globalClock);
}

void LimeData::threadBegin(Pid_t pid)
{

	if(tsPar.size()<(pid+1)) {
		tsPar.resize((pid+1)*2);
		pBr.resize((pid+1)*2);
		edgeCntMap.resize((pid+1)*2);
		memCntMap.resize((pid+1)*2);
		memL1MissCntMap.resize((pid+1)*2);
		memL2MissCntMap.resize((pid+1)*2);
	}

	if(pid==0) {
		tsProgramBeginEnd.first = globalClock;
	} else {
		setupBegin(pid);

		if(curPar==1) {
			setupBegin(0);
		}
	}

	curPar++;

}

void LimeData::setupThreadEnd(Pid_t pid)
{
	if(parInfo.find(0)==parInfo.end() || parInfo[0] == NULL) {
		ParInfo *pi = ParInfo::parInfoPool.out();
		pi->set(0, -1);
		parInfo[0] = pi;
	}

	I(parInfo[0]!=NULL);

	parInfo[0]->addSite(0);
	parInfo[0]->addThread(pid, tsPar[pid], globalClock);
	parInfo[0]->addEdges(pid, edgeCntMap[pid]);
	parInfo[0]->addMems(pid, memCntMap[pid], memL1MissCntMap[pid], memL2MissCntMap[pid]);
}

void LimeData::threadEnd(Pid_t pid)
{
	I(curPar>0);
	curPar--;

	if(pid==0) {
		tsProgramBeginEnd.second = globalClock;
		if(!joinCalled) {
			I(tsThreadBeginEnd.find(pid)!=tsThreadBeginEnd.end());
			tsThreadBeginEnd[pid].push_back(globalClock); 
		}
	} else {
		I(tsThreadBeginEnd.find(pid)!=tsThreadBeginEnd.end());
		tsThreadBeginEnd[pid].push_back(globalClock); 
		
		setupThreadEnd(pid);
	}

}

void LimeData::barCall(Pid_t pid, VAddr ra, VAddr a0)
{

	registerPar(a0, ra, pid);

	//std::cout<<"BAR "<<pid<<" "<<hex<<ra<<" "<<a0<<dec<<" "<<globalClock<<endl;
}

void LimeData::barRet(Pid_t pid, VAddr ra, VAddr a0)
{
	//std::cout<<"BRE "<<pid<<" "<<hex<<ra<<" "<<a0<<dec<<" "<<globalClock<<endl;

	deregisterPar(a0, ra, pid);
	
	tsPar[pid] = globalClock;
}

void LimeData::joinCall(Pid_t pid) {
	//std::cout<<"JOINCALL "<<pid<<" "<<globalClock<<endl;

	if(!joinCalled) {
		I(tsThreadBeginEnd.find(pid)!=tsThreadBeginEnd.end());
		tsThreadBeginEnd[pid].push_back(globalClock); 
		
		setupThreadEnd(pid);
		joinCalled = true;
	}
}

void LimeData::joinRet(Pid_t pid) {
	//std::cout<<"JOINRET "<<pid<<" "<<globalClock<<endl;

	if(curPar==1 && parInfo[0]!=NULL) {
		parInfo[0]->report(outfile);
		parInfo[0]->destroy();
		parInfo[0] = NULL;
	}
}

void LimeData::processBr(Pid_t pid, VAddr ia)
{
	//std::cout<<"T "<<pid<<" BR "<<std::hex<<ia<<std::dec<<" "<<globalClock<<std::endl;

	std::pair<VAddr, VAddr> edge = std::make_pair(pBr[pid], ia);

	if(edgeCntMap[pid].find(edge)==edgeCntMap[pid].end()) {
		edgeCntMap[pid][edge] = 1;
	} else {
		edgeCntMap[pid][edge] += 1;
	}

	pBr[pid] = ia;
}

void LimeData::processMem(Pid_t pid, VAddr ia, bool l1m, bool l2m)
{
	if(memCntMap[pid].find(ia) == memCntMap[pid].end()) {
		memCntMap[pid][ia] = 1;
	} else {
		memCntMap[pid][ia] += 1;
	}
	if(l1m) {
		if(memL1MissCntMap[pid].find(ia) == memL1MissCntMap[pid].end()) {
			memL1MissCntMap[pid][ia] = 1;
		} else {
			memL1MissCntMap[pid][ia] += 1;
		}
	}
	if(l2m) {
		I(l1m);
		if(memL2MissCntMap[pid].find(ia) == memL2MissCntMap[pid].end()) {
			memL2MissCntMap[pid][ia] = 1;
		} else {
			memL2MissCntMap[pid][ia] += 1;
		}
	}
}

void LimeData::processCall(DInst *dinst)
{
	switch(dinst->callInfo) {
		case LIME_BAR_CALL:
			barCall(dinst->context->getPid(), dinst->barRA, dinst->barA0);
			break;
		case LIME_BAR_RET:
			barRet(dinst->context->getPid(), dinst->barRA, dinst->barA0);
			break;
		
		case LIME_JOIN_CALL:
			joinCall(dinst->context->getPid());
			break;
		case LIME_JOIN_RET:
			joinRet(dinst->context->getPid());
			break;
		default:
			I(0);
			break;
	};
}

void LimeData::processInst(DInst *dinst)
{
	switch(dinst->instInfo) {
		case LIME_BR:
			processBr(dinst->context->getPid(), dinst->getIAddr());
			break;
		case LIME_MEM:
			processMem(dinst->context->getPid(), dinst->getIAddr(), dinst->l1miss, dinst->l2miss);
			break;
		default:
			I(0);
			break;
	};
}

void LimeData::processFFInst(ThreadContext *context)
{
	switch(context->callInfo) {
		case LIME_BAR_CALL:
			barCall(context->getPid(), context->barRA, context->barA0);
			break;
		case LIME_BAR_RET:
			barRet(context->getPid(), context->barRA, context->barA0);
			break;
		
		case LIME_JOIN_CALL:
			joinCall(context->getPid());
			break;
		case LIME_JOIN_RET:
			joinRet(context->getPid());
			break;

		default:
			I(0);
			break;
	}
}

void LimeData::registerPar(VAddr a0, VAddr ra, Pid_t pid)
{
	if(parSeqMap.find(a0)==parSeqMap.end() || parSeqMap[a0]<0) {
		parSeqMap[a0] = LimeData::seq;
		ParInfo *pi = ParInfo::parInfoPool.out();
		pi->set(a0, LimeData::seq);
		parInfo[a0] = pi;

		LimeData::seq++;
	}

	seqPerThread[pid] = parSeqMap[a0];

	I(parInfo[a0]!=NULL);

	parInfo[a0]->addSite(ra);
	parInfo[a0]->addThread(pid, tsPar[pid], globalClock);
	parInfo[a0]->addEdges(pid, edgeCntMap[pid]);
	parInfo[a0]->addMems(pid, memCntMap[pid], memL1MissCntMap[pid], memL2MissCntMap[pid]);
}

void LimeData::deregisterPar(VAddr a0, VAddr ra, Pid_t pid)
{
	int64_t mySeq = seqPerThread[pid];
	if(parSeqMap[a0] == mySeq) {
		//I(parInfo.find(a0)!=parInfo.end());
		parSeqMap[a0] = -1;
		//parInfo[a0]->dump();
		parInfo[a0]->report(outfile);
		parInfo[a0]->destroy();
		parInfo[a0] = NULL;
	}
}


// ----------------------------------------------------------------
//

LimeData::ParInfo::ParInfo()
{
}

LimeData::ParInfo::~ParInfo()
{
}

void LimeData::ParInfo::set(VAddr a0, int64_t s)
{
	bid = a0;
	id = s;
}

void LimeData::ParInfo::addSite(VAddr ra)
{
	sites.insert(ra);
}

void LimeData::ParInfo::addThread(Pid_t pid, Time_t start, Time_t end)
{
	threads.insert(pid);

	tsThreads[pid] = std::make_pair(start, end);
}
				
void LimeData::ParInfo::addEdges(Pid_t pid, std::map<std::pair<VAddr, VAddr>, int64_t> &edges)
{
	edgeCnts[pid] = &edges;
}

void LimeData::ParInfo::addMems(Pid_t pid, 	std::map<VAddr, int64_t> &mem, 
											std::map<VAddr, int64_t> &L1,
											std::map<VAddr, int64_t> &L2)
{
	memCnts[pid] = &mem;
	L1Cnts[pid] = &L1;
	L2Cnts[pid] = &L2;
}

void LimeData::ParInfo::destroy()
{
	clear();
	parInfoPool.in(this);
}

void LimeData::ParInfo::clear()
{
	bid = 0;
	id = -1;

	for(std::map<Pid_t, std::map<std::pair<VAddr, VAddr>, int64_t>* >::iterator it=edgeCnts.begin();
			it!=edgeCnts.end();it++) {
		I(it->second!=NULL);
		it->second->clear();
	}
	for(std::map<Pid_t, std::map<VAddr, int64_t>* >::iterator it=memCnts.begin();it!=memCnts.end();it++) {
		I(it->second!=NULL);
		it->second->clear();
	}
	for(std::map<Pid_t, std::map<VAddr, int64_t>* >::iterator it=L1Cnts.begin();it!=L1Cnts.end();it++) {
		I(it->second!=NULL);
		it->second->clear();
	}
	for(std::map<Pid_t, std::map<VAddr, int64_t>* >::iterator it=L2Cnts.begin();it!=L2Cnts.end();it++) {
		I(it->second!=NULL);
		it->second->clear();
	}

	sites.clear();
	threads.clear();
	tsThreads.clear();
	edgeCnts.clear();

	memCnts.clear();
	L1Cnts.clear();
	L2Cnts.clear();
}

void LimeData::ParInfo::dump()
{
	std::cout<<"Barrier "<<std::hex<<bid<<std::dec<<" id "<<id<<std::endl;
	std::cout<<"Sites: ";

	std::cout<<std::hex;
	std::copy(sites.begin(), sites.end(), std::ostream_iterator<VAddr>(std::cout, " "));
	std::cout<<std::dec<<std::endl;

	std::cout<<"Threads: ";
	std::copy(threads.begin(), threads.end(), std::ostream_iterator<Pid_t>(std::cout, " "));
	std::cout<<std::endl;

	for(std::map<Pid_t, std::pair<Time_t, Time_t> >::iterator it = tsThreads.begin();it!=tsThreads.end();it++) {
		Pid_t t = it->first;
		Time_t st = it->second.first;
		Time_t en = it->second.second;
		std::cout<<"\tThread "<<t<<" S "<<st<<"   E "<<en<<"  N "<<globalClock<<std::endl;
	}
	
}

void LimeData::ParInfo::report(std::ofstream &out)
{
	out<<LM_PER_INS_TIME<<std::endl;
	out<<id<<std::endl;
	out<<bid<<std::endl;
	out<<globalClock<<std::endl;

	out<<sites.size()<<std::endl;
	std::copy(sites.begin(), sites.end(), std::ostream_iterator<VAddr>(out, "\n"));

	out<<tsThreads.size()<<std::endl;

	for(std::map<Pid_t, std::pair<Time_t, Time_t> >::iterator it = tsThreads.begin();it!=tsThreads.end();it++) {
		Pid_t t = it->first;
		Time_t st = it->second.first;
		Time_t en = it->second.second;
		out<<t<<" "<<st<<" "<<en<<" "<<(en-st)<<std::endl;
		//out<<t<<" "<<st<<" "<<en<<std::endl;
	}
	
	out<<LM_PER_INS_DATA<<std::endl;
	out<<edgeCnts.size()<<std::endl;
	for(std::map<Pid_t, std::map<std::pair<VAddr, VAddr>, int64_t>* >::iterator it=edgeCnts.begin();
			it!=edgeCnts.end();it++) {
		Pid_t t = it->first;
		out<<t<<" "<<it->second->size()<<std::endl;
		for(std::map<std::pair<VAddr, VAddr>, int64_t>::iterator jt=it->second->begin();
				jt!=it->second->end();jt++) {
			VAddr from = jt->first.first;
			VAddr to = jt->first.second;
			int64_t cnt = jt->second;
			out<<from<<" "<<to<<" "<<cnt<<std::endl;
		}
	}



	out<<LM_PER_INS_MEM_DATA<<std::endl;
	out<<memCnts.size()<<std::endl;
	for(std::map<Pid_t, std::map<VAddr, int64_t>* >::iterator it=memCnts.begin();it!=memCnts.end();it++) {
		Pid_t t = it->first;

		I(L1Cnts.find(t)!=L1Cnts.end());
		I(L2Cnts.find(t)!=L2Cnts.end());

		out<<t<<" "<<it->second->size()<<std::endl;
		for(std::map<VAddr, int64_t>::iterator jt=it->second->begin();
				jt!=it->second->end();jt++) {
			VAddr ia = jt->first;
			int64_t cnt = jt->second;
			int64_t l1cnt = 0;
			int64_t l2cnt = 0;

			if(L1Cnts[t]->find(ia)!=L1Cnts[t]->end()) {
				l1cnt = L1Cnts[t]->at(ia);
			}
			if(L2Cnts[t]->find(ia)!=L2Cnts[t]->end()) {
				l2cnt = L2Cnts[t]->at(ia);
			}
			out<<ia<<" "<<cnt<<" "<<l1cnt<<" "<<l2cnt<<std::endl;
		}
	}

}


