#include <cmath>
#include <algorithm>
#include <iostream>
#include "libsuc/nanassert.h"
#include "SescConf.h"
#include "TMState.h"
#include "TMCoherence.h"

using namespace std;

uint64_t TransState::nextUtid = 0;

TransState::TransState(Pid_t pid): myPid(pid) {
    clear();
}
void TransState::clear() {
    utid        = INVALID_UTID;
    state       = TM_INVALID;
}
void TransState::begin() {
    utid        = TransState::nextUtid;
    TransState::nextUtid += 1;

    state       = TM_RUNNING;
}
void TransState::startAborting() {
    state       = TM_ABORTING;
}
void TransState::markAbort() {
    state       = TM_MARKABORT;
}
void TransState::print() const {
    std::cout << myPid << " ";
    switch(state) {
        case TM_INVALID:    cout << "INVALID";  break;
        case TM_RUNNING:    cout << "RUNNING";  break;
        case TM_ABORTING:   cout << "ABORING";  break;
        case TM_MARKABORT:  cout << "MARK_ABORT"; break;
        default:            cout << "??????";   break;
    };
    std::cout << " \n";
}

