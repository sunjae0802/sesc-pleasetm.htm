
#include "TMStorage.h"
#include "libll/ThreadContext.h"

using namespace std;

TMStorage2::CacheLine::CacheLine(): dirty(false) {
    std::fill(data, data + CACHE_SIZE, 0);
}
void TMStorage2::loadLine(ThreadContext* context, VAddr addr) {
    if(inTnxStorage(addr) == false) {
        CacheLine line;
        VAddr cAddr = computeCAddr(addr);
        for(uint32_t offset = 0; offset < CACHE_SIZE; offset += sizeof(uint32_t)) {
            uint32_t word  = context->readMemRaw<uint32_t>(cAddr + offset);
            *(reinterpret_cast<uint32_t*>(line.data + offset)) = word;
        }
        tnxStorage.insert(make_pair(cAddr, line));
    }
}
void TMStorage2::flush(ThreadContext* context) {
    TnxStorage::iterator iStorage = tnxStorage.begin();
    for(iStorage; iStorage != tnxStorage.end(); ++iStorage) {
        VAddr cAddr = iStorage->first;
        CacheLine& line = iStorage->second;
        if(line.dirty) {
            for(uint32_t offset = 0; offset < CACHE_SIZE; offset += sizeof(uint32_t)) {
                uint32_t word = *(reinterpret_cast<uint32_t*>(line.data + offset));
                context->writeMemRaw<uint32_t>(cAddr + offset, word);
            }
        }
    }
	tnxStorage.clear();
}
