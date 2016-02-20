#include <cmath>
#include <algorithm>
#include <iostream>
#include "libsuc/nanassert.h"
#include "SescConf.h"
#include "TMState.h"
#include "TMCoherence.h"

using namespace std;

TransState::TransState(Pid_t pid): myPid(pid) {
    state       = TM_INVALID;
}
const char* TransState::getStateStr(TMState_e st) {
    const char* str = "??????";
    switch(st) {
        case TM_INVALID:    str = "INVALID";    break;
        case TM_RUNNING:    str = "RUNNING";    break;
        case TM_ABORTING:   str = "ABORING";    break;
        case TM_MARKABORT:  str = "MARK_ABORT"; break;
        default:                                break;
    };
    return str;
}
void TransState::print() const {
    std::cout << myPid << " " << getStateStr(state) << " \n";
}
void TransState::triggerFail(TMState_e next) {
    fail("[%d] Invalid state transition(%s->%s)\n",
        myPid, getStateStr(state), getStateStr(next));
}
void TransState::begin() {
    TMState_e next = TM_RUNNING;
    switch(state) {
        case TM_INVALID:
            state = next;
            break;
        default:
            triggerFail(next);
    }
}
void TransState::markAbort() {
    TMState_e next = TM_MARKABORT;
    switch(state) {
        case TM_RUNNING:
            state = next;
            break;
        default:
            triggerFail(next);
    }
}
void TransState::startAborting() {
    TMState_e next = TM_ABORTING;
    switch(state) {
        case TM_MARKABORT:
            state = next;
            break;
        default:
            triggerFail(next);
    }
}
void TransState::clear() {
    TMState_e next = TM_INVALID;
    switch(state) {
        case TM_RUNNING:
        case TM_ABORTING:
            state = next;
            break;
        default:
            triggerFail(next);
    }
}

