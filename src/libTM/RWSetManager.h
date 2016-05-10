#ifndef HTM_RWSET_MANAGER
#define HTM_RWSET_MANAGER

#include <vector>
#include <map>
#include <set>
#include "Snippets.h"
#include "libemul/Addressing.h"

///
// Class that maintains the read/write set of the entire system.
class RWSetManager {
public:
    RWSetManager() {}

    void initialize(size_t nThreads);
    void read(Pid_t pid, VAddr caddr);
    void write(Pid_t pid, VAddr caddr);
    void clear(Pid_t pid);

    // Various getters/setters
    size_t getNumReads(Pid_t pid)   const { return linesRead.at(pid).size(); }
    size_t getNumWrites(Pid_t pid)  const { return linesWritten.at(pid).size(); }
    size_t numReaders(VAddr caddr) const;
    size_t numWriters(VAddr caddr) const;
    bool hadRead(Pid_t pid, VAddr caddr) const {
        return linesRead.at(pid).find(caddr) != linesRead.at(pid).end();
    }
    bool hadWrote(Pid_t pid, VAddr caddr) const {
        return linesWritten.at(pid).find(caddr) != linesWritten.at(pid).end();
    }
    // Return set of threads that read/wrote to given caddr
    void getReaders(VAddr caddr, std::set<Pid_t>& r) const;
    void getWriters(VAddr caddr, std::set<Pid_t>& w) const;
private:
    std::vector<std::set<VAddr> >       linesRead;
    std::vector<std::set<VAddr> >       linesWritten;
    std::map<VAddr, std::set<Pid_t> >   writers;
    std::map<VAddr, std::set<Pid_t> >   readers;
};

#endif
