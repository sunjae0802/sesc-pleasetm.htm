#include <cmath>
#include <algorithm>
#include <iostream>
#include "libsuc/nanassert.h"
#include "SescConf.h"
#include "TMState.h"
#include "TMCoherence.h"

using namespace std;

TransState::TransState(Pid_t pid):
        myPid(pid), state(TM_INVALID),
        utid(INVALID_UTID), restartPending(false) {
}

void TransState::begin(uint64_t newUtid) {
    utid        = newUtid;
    state       = TM_RUNNING;
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
    utid        = INVALID_UTID;
    state       = TM_INVALID;
    restartPending = true;
}
void TransState::completeFallback() {
    restartPending = false;
}
void TransState::markAbort() {
    state       = TM_MARKABORT;
}
void TransState::commit() {
    utid        = INVALID_UTID;
    state       = TM_INVALID;
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


