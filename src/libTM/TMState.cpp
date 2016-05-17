#include <iostream>
#include "libemul/EmulInit.h"
#include "TMState.h"

using namespace std;

TMStateEngine::TMStateEngine(Pid_t pid): myPid(pid) {
    myState       = TM_INVALID;
}
const char* TMStateEngine::getStateStr(State_e st) {
    const char* str = "??????";
    switch(st) {
        case TM_INVALID:    str = "INVALID";    break;
        case TM_RUNNING:    str = "RUNNING";    break;
        case TM_ABORTING:   str = "ABORING";    break;
        case TM_MARKABORT:  str = "MARK_ABORT"; break;
    };
    return str;
}
void TMStateEngine::print() const {
    std::cout << myPid << " " << getStateStr(myState) << " \n";
}
void TMStateEngine::triggerFail(State_e nextState) {
    fail("[%d] Invalid state transition(%s->%s)\n",
        myPid, getStateStr(), getStateStr(nextState));
}
void TMStateEngine::begin() {
    State_e nextState = TM_RUNNING;
    switch(myState) {
        case TM_INVALID:
            myState = nextState;
            break;
        default:
            triggerFail(nextState);
    }
}
void TMStateEngine::markAbort() {
    State_e nextState = TM_MARKABORT;
    switch(myState) {
        case TM_RUNNING:
            myState = nextState;
            break;
        default:
            triggerFail(nextState);
    }
}
void TMStateEngine::startAborting() {
    State_e nextState = TM_ABORTING;
    switch(myState) {
        case TM_MARKABORT:
            myState = nextState;
            break;
        default:
            triggerFail(nextState);
    }
}
void TMStateEngine::clear() {
    State_e nextState = TM_INVALID;
    switch(myState) {
        case TM_RUNNING:
        case TM_ABORTING:
            myState = nextState;
            break;
        default:
            triggerFail(nextState);
    }
}

