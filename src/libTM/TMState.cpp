#include <cmath>
#include <algorithm>
#include <iostream>
#include "libsuc/nanassert.h"
#include "SescConf.h"
#include "TMState.h"
#include "TMCoherence.h"

using namespace std;

TMStateEngine::TMStateEngine(Pid_t pid): myPid(pid) {
    state       = TM_INVALID;
}
const char* TMStateEngine::getStateStr(State_e st) {
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
void TMStateEngine::print() const {
    std::cout << myPid << " " << getStateStr(state) << " \n";
}
void TMStateEngine::triggerFail(State_e next) {
    fail("[%d] Invalid state transition(%s->%s)\n",
        myPid, getStateStr(state), getStateStr(next));
}
void TMStateEngine::begin() {
    State_e next = TM_RUNNING;
    switch(state) {
        case TM_INVALID:
            state = next;
            break;
        default:
            triggerFail(next);
    }
}
void TMStateEngine::markAbort() {
    State_e next = TM_MARKABORT;
    switch(state) {
        case TM_RUNNING:
            state = next;
            break;
        default:
            triggerFail(next);
    }
}
void TMStateEngine::startAborting() {
    State_e next = TM_ABORTING;
    switch(state) {
        case TM_MARKABORT:
            state = next;
            break;
        default:
            triggerFail(next);
    }
}
void TMStateEngine::clear() {
    State_e next = TM_INVALID;
    switch(state) {
        case TM_RUNNING:
        case TM_ABORTING:
            state = next;
            break;
        default:
            triggerFail(next);
    }
}

