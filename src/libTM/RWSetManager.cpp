#include <cmath>
#include <algorithm>
#include <iostream>
#include "RWSetManager.h"

using namespace std;

void RWSetManager::initialize(size_t nThreads) {
    linesRead.resize(nThreads);
    linesWritten.resize(nThreads);
}
void RWSetManager::read(Pid_t pid, VAddr caddr) {
    readers[caddr].insert(pid);
    linesRead.at(pid).insert(caddr);
}
void RWSetManager::write(Pid_t pid, VAddr caddr) {
    writers[caddr].insert(pid);
    linesWritten.at(pid).insert(caddr);
}
void RWSetManager::clear(Pid_t pid) {
    // First step through addresses I accessed and clear them from readers/writers
    for(VAddr caddr:  linesRead.at(pid)) {
        auto i_line = readers.find(caddr);
        i_line->second.erase(pid);
        if(i_line->second.empty()) {
            readers.erase(i_line);
        }
    }
    for(VAddr caddr:  linesWritten.at(pid)) {
        auto i_line = writers.find(caddr);
        i_line->second.erase(pid);
        if(i_line->second.empty()) {
            writers.erase(i_line);
        }
    }
    // Then clear my own address set
    linesRead.at(pid).clear();
    linesWritten.at(pid).clear();
}

size_t RWSetManager::numReaders(VAddr caddr) const {
    auto i_line = readers.find(caddr);
    if(i_line == readers.end()) {
        return 0;
    } else {
        return i_line->second.size();
    }
}
size_t RWSetManager::numWriters(VAddr caddr) const {
    auto i_line = writers.find(caddr);
    if(i_line == writers.end()) {
        return 0;
    } else {
        return i_line->second.size();
    }
}
void RWSetManager::getReaders(VAddr caddr, std::set<Pid_t>& r) const {
    auto i_line = readers.find(caddr);
    if(i_line != readers.end()) {
        r.insert(i_line->second.begin(), i_line->second.end());
    }
}
void RWSetManager::getWriters(VAddr caddr, std::set<Pid_t>& w) const {
    auto i_line = writers.find(caddr);
    if(i_line != writers.end()) {
        w.insert(i_line->second.begin(), i_line->second.end());
    }
}
