#ifndef HW_GATE
#define HW_GATE

#include <map>
#include <set>
#include <algorithm>
#include <list>
class TMCoherence;
#include "TMCoherence.h"

struct HWGate {
    HWGate(Pid_t o): owner(o), readOnly(false) {}

    bool isOwner(Pid_t pid) const { return owner == pid; }
    Pid_t getOwner() const { return owner; }

    bool isMember(Pid_t pid) const;
    void addMember(Pid_t pid);
    void removeMember(Pid_t pid);

    bool isReader(Pid_t pid) const;
    void addReader(Pid_t pid);
    void removeReader(Pid_t pid);
    void setReadOnly() { readOnly = true; }
    bool getReadOnly() const { return readOnly; }
    void stopReads();

    size_t size() const {
        if(owner == -1) {
            return 0;
        } else {
            return 1 + members.size() + readers.size();
        }
    }
    void print() const;
    void remove(Pid_t pid);
    void setOwner(Pid_t o);

    Pid_t owner;
    bool readOnly;
    std::list<Pid_t> members;
    std::list<Pid_t> readers;
};

#endif
