#include <cmath>
#include <algorithm>
#include <iostream>
#include "libsuc/nanassert.h"
#include "SescConf.h"
#include "TMState.h"
#include "TMCoherence.h"

using namespace std;

TransState::TransState(Pid_t pid):
        myPid(pid), state(TM_INVALID), timestamp(INVALID_TIMESTAMP),
        utid(INVALID_UTID), restartPending(false) {
}

void TransState::begin(uint64_t newUtid) {
    timestamp   = globalClock;
    utid        = newUtid;
    state       = TM_RUNNING;

    abortState.clear();
    restartPending = false;
}
void TransState::startAborting() {
    state       = TM_ABORTING;
}
void TransState::suspend() {
    state       = TM_SUSPENDED;
}
void TransState::resume() {
    I(state == TM_SUSPENDED);
    state       = TM_RUNNING;
}
void TransState::completeAbort() {
    I(state == TM_ABORTING);
    timestamp   = INVALID_TIMESTAMP;
    utid        = INVALID_UTID;
    state       = TM_INVALID;
    restartPending = true;
}
void TransState::completeFallback() {
    restartPending = false;
    abortState.clear();
}
void TransState::markAbort(Pid_t byPid, uint64_t byUtid, VAddr byCaddr, TMAbortType_e abortType) {
    state       = TM_MARKABORT;
    abortState.markAbort(byPid, byUtid, byCaddr, abortType);
}
void TransState::commit() {
    timestamp   = INVALID_TIMESTAMP;
    utid        = INVALID_UTID;
    state       = TM_INVALID;
    abortState.clear();
}
void TransState::print() const {
    std::cout << myPid << " ";
    switch(state) {
        case TM_INVALID:    cout << "INVALID";  break;
        case TM_RUNNING:    cout << "RUNNING";  break;
        case TM_SUSPENDED:  cout << "SUSPENDED";   break;
        case TM_ABORTING:   cout << "ABORING";  break;
        case TM_MARKABORT:  cout << "MARK_ABORT"; break;
        default:            cout << "??????";   break;
    };
    std::cout << " \n";
}


