/*
   SESC: Super ESCalar simulator
   Copyright (C) 2003 University of Illinois.

   Contributed by Karin Strauss

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

#include <stdlib.h>

#include <vector>

// sesc internal stuff
#include "ReportGen.h"
#include "SescConf.h"
#include "callback.h"

// sesc OS model
#include "libcore/OSSim.h"

// hardware models
#include "libcore/Processor.h"
#include "libcore/SMTProcessor.h"
#include "SMemorySystem.h"

// debugging defines
#include "SMPDebug.h"

#if (defined TM)
#include "libTM/TMCoherence.h"
#endif

void initTMCoherence(int32_t nProcs);

void initTMCoherence(int32_t nProcs)
{
#if (defined TM)
    string method = SescConf->getCharPtr("TransactionalMemory","method");
    int cacheLineSize = SescConf->getInt("TransactionalMemory","cacheLineSize");
    int numLines = SescConf->getInt("TransactionalMemory","numLines");
	int returnArgType = SescConf->getInt("TransactionalMemory","returnArgType");
    if(method == "EE") {
        tmCohManager = new TMEECoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LL") {
        tmCohManager = new TMLLCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE") {
        tmCohManager = new TMLECoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-Hourglass") {
        tmCohManager = new TMLEHourglassCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-SOK") {
        tmCohManager = new TMLESOKCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-SOK-Queue") {
        tmCohManager = new TMLESOKQueueCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-SOA-Original") {
        tmCohManager = new TMLESOA0Coherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-SOA2") {
        tmCohManager = new TMLESOA2Coherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-Lock") {
        tmCohManager = new TMLELockCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-Lock0") {
        tmCohManager = new TMLELock0Coherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-WAR") {
        tmCohManager = new TMLEWARCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-ATS") {
        tmCohManager = new TMLEATSCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-ASet") {
        tmCohManager = new TMLEAsetCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "LE-Snoop") {
        tmCohManager = new TMLESnoopCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "First") {
        tmCohManager = new TMFirstWinsCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "Older") {
        tmCohManager = new TMOlderCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "OlderAll") {
        tmCohManager = new TMOlderAllCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "More") {
        tmCohManager = new TMMoreCoherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else if(method == "First2") {
        tmCohManager = new TMFirstWins2Coherence(nProcs, cacheLineSize, numLines, returnArgType);
    } else {
        MSG("unknown TM method, using EE");
        tmCohManager = new TMEECoherence(nProcs, cacheLineSize, numLines, returnArgType);
    }
#endif
}

int32_t main(int32_t argc, char**argv, char **envp)
{
    srand(1);
    osSim = new OSSim(argc, argv, envp);

    int32_t nProcs = SescConf->getRecordSize("","cpucore");

    GLOG(SMPDBG_CONSTR, "Number of Processors: %d", nProcs);

#if (defined TM)
    initTMCoherence(nProcs);
#endif

    // processor and memory build
    std::vector<GProcessor *>    pr(nProcs);
    std::vector<GMemorySystem *> ms(nProcs);

    for(int32_t i = 0; i < nProcs; i++) {
        GLOG(SMPDBG_CONSTR, "Building processor %d and its memory subsystem", i);
        GMemorySystem *gms = new SMemorySystem(i);
        gms->buildMemorySystem();
        ms[i] = gms;
        pr[i] = 0;
        if(SescConf->checkInt("cpucore","smtContexts",i)) {
            if( SescConf->getInt("cpucore","smtContexts",i) > 1 )
                pr[i] =new SMTProcessor(ms[i], i);
        }
        if (pr[i] == 0)
            pr[i] =new Processor(ms[i], i);
    }

    GLOG(SMPDBG_CONSTR, "I am booting now");
    osSim->boot();
    GLOG(SMPDBG_CONSTR, "Terminating simulation");

    for(int32_t i = 0; i < nProcs; i++) {
        delete pr[i];
        delete ms[i];
    }

    delete osSim;

    return 0;
}
