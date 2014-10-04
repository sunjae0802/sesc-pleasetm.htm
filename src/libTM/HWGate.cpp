#include <cassert>
#include <iostream>
#include "TMCoherence.h"
#include "HWGate.h"

bool HWGate::isMember(Pid_t pid) const {
    std::list<Pid_t>::const_iterator i_member;
    i_member = std::find(members.begin(), members.end(), pid);
    return i_member != members.end();
}
bool HWGate::isReader(Pid_t pid) const {
    std::list<Pid_t>::const_iterator i_reader;
    i_reader = std::find(readers.begin(), readers.end(), pid);
    return i_reader != readers.end();
}
void HWGate::stopReads() {
    if(readOnly == false) {
        fail("Readers only valid for read only gates\n");
    }
    readOnly = false;
    readers.clear();
}
void HWGate::addMember(Pid_t pid) {
    assert(owner != pid);
    assert(!isReader(pid));
    assert(!isMember(pid));
    members.push_back(pid);
}
void HWGate::removeReader(Pid_t pid) {
    readers.remove(pid);
}
void HWGate::removeMember(Pid_t pid) {
    members.remove(pid);
}
void HWGate::addReader(Pid_t pid) {
    assert(owner != pid);
    assert(readOnly);       // Readers only valid in readOnly
    assert(!isReader(pid));
    assert(!isMember(pid));
    readers.push_back(pid);
}
void HWGate::remove(Pid_t pid) {
    if(pid == owner) {
        if(size() == 1) {
            owner = -1;
        } else if(readOnly && readers.size() > 0) {
            owner = readers.front();
            readers.pop_front();
        } else {
            owner = members.front();
            members.pop_front();
        }
    } else if(readOnly) {
        removeReader(pid);
    } else {
        removeMember(pid);
    }
}
void HWGate::setOwner(Pid_t o) {
    if(owner == o) { fail("Cannot reset to same owner\n"); }
    if(readOnly && isReader(o)) {
        removeReader(o);
    }
    if(isMember(o)) {
        removeMember(o);
    }
    owner = o;
}

void HWGate::print() const {
    std::cout << "Gate " << owner << ':';

    std::list<Pid_t>::const_iterator i_reader;
    for(i_reader = readers.begin();
        i_reader != readers.end(); ++i_reader) {
        std::cout << (*i_reader) << '$';
    }
    std::list<Pid_t>::const_iterator i_member;
    for(i_member = members.begin();
        i_member != members.end(); ++i_member) {
        std::cout << (*i_member) << ' ';
    }
    std::cout << std::endl;
}

