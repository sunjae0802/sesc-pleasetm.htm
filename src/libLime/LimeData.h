#ifndef LIMEDATA_H
#define LIMEDATA_H

#include "Snippets.h"
#include "libemul/Addressing.h"
#include "libcore/DInst.h"
#include "pool.h"

#include <map>
#include <vector>
#include <set>
#include <fstream>

#include "LimeType.h"

class LimeData {
	public:
		LimeData(const char *outFile, const char *bench);
		~LimeData();

		void threadBegin(Pid_t pid);
		void threadEnd(Pid_t pid);
		
		void processCall(DInst *dinst);
		void processInst(DInst *dinst);

		void processFFInst(ThreadContext *context);

		void finalReport();
	protected:
	private:
		bool joinCalled;
		int curPar;

		std::vector<Time_t> tsPar;
		std::vector<VAddr> pBr;

		std::map<Pid_t, std::vector<Time_t> > tsThreadBeginEnd;
		std::pair<Time_t, Time_t> tsProgramBeginEnd;

		const char *outFileName;

		std::map<VAddr, int64_t> parSeqMap;
		std::map<Pid_t, int64_t> seqPerThread;

		std::vector<std::map<std::pair<VAddr, VAddr>, int64_t> > edgeCntMap;
		std::vector<std::map<VAddr, int64_t> > memCntMap;
		std::vector<std::map<VAddr, int64_t> > memL1MissCntMap;
		std::vector<std::map<VAddr, int64_t> > memL2MissCntMap;

		std::ofstream outfile;

		static int64_t seq;

		class ParInfo {
			public:
				ParInfo();
				~ParInfo();

				void set(VAddr bid, int64_t s);
				void dump();
				void report(std::ofstream &out);
				void destroy();
				
				void addSite(VAddr ra);
				void addThread(Pid_t pid, Time_t start, Time_t end);
				void addEdges(Pid_t pid, std::map<std::pair<VAddr, VAddr>, int64_t> &edges);
				void addMems(Pid_t pid,	std::map<VAddr, int64_t> &mem, 
										std::map<VAddr, int64_t> &L1,
										std::map<VAddr, int64_t> &L2);
    	
				static pool<ParInfo> parInfoPool;
			private:
				VAddr bid;
				int64_t id;

				std::set<VAddr> sites;
				std::set<Pid_t> threads;
				std::map<Pid_t, std::pair<Time_t, Time_t> > tsThreads;

				std::map<Pid_t, std::map<std::pair<VAddr, VAddr>, int64_t>* > edgeCnts;
				
				std::map<Pid_t, std::map<VAddr, int64_t>* > memCnts;
				std::map<Pid_t, std::map<VAddr, int64_t>* > L1Cnts;
				std::map<Pid_t, std::map<VAddr, int64_t>* > L2Cnts;
				
				void clear();
		};

		std::map<VAddr, ParInfo *> parInfo;
		
		void barCall(Pid_t pid, VAddr ra, VAddr a0);
		void barRet(Pid_t pid, VAddr ra, VAddr a0);
		
		void joinCall(Pid_t pid);
		void joinRet(Pid_t pid);

		void processBr(Pid_t pid, VAddr ia);
		void processMem(Pid_t pid, VAddr ia, bool l1m, bool l2m);

		void registerPar(VAddr a0, VAddr ra, Pid_t pid);
		void deregisterPar(VAddr a0, VAddr ra, Pid_t pid);

		void setupBegin(Pid_t pid);
		void setupThreadEnd(Pid_t pid);
};
	
extern LimeData *limeData;

#endif
