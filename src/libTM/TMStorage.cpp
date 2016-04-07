
#include "TMStorage.h"
#include "libll/ThreadContext.h"

using namespace std;

TMStorage::TMStorage()
{
}

/**
 * @ingroup transCache
 * @brief Default destructor
 */
TMStorage::~TMStorage()
{
}


uint8_t TMStorage::load8(VAddr addr, bool *found)
{
    bool myF = false;
    uint8_t myRV = 0;
    VAddr cAddr = computeCAddr(addr);
    VAddr cOff = computeCOffset(addr);
    TnxStorage::iterator iStorage = tnxStorage.find(cAddr);
    if(iStorage != tnxStorage.end()) {
        CacheLine& line = iStorage->second;
        myRV = line.data[cOff];
        myF = true;
    }

	(*found) = false;
	int z = 0;
	VAddr oaddr = addr;

	while(addr%4 != 0){ 
		addr--;
		z++;
	}

	map<VAddr, MEMUNIT>::iterator it; 
	it = memMap.find(addr);
	if(it != memMap.end()){

		MEMUNIT mem = it->second;
		uint8_t retVal = (uint8_t)((mem >> (8 * z)) & 0xFF);
		(*found) = true;
        if(myF == false) { fail("Find fail in load8(0x%lx)\n", addr); }
        if(myRV != retVal) { fail("Bug in load8(0x%lx)\n", addr); }
		return retVal;
	}
	return 0;
}

uint16_t TMStorage::load16(VAddr addr, bool *found)
{
    bool myF = false;
    uint16_t myRV = 0;
    VAddr cAddr = computeCAddr(addr);
    VAddr cOff = computeCOffset(addr);
    TnxStorage::iterator iStorage = tnxStorage.find(cAddr);
    if(iStorage != tnxStorage.end()) {
        CacheLine& line = iStorage->second;
        myRV = *((uint16_t*)(line.data + cOff));
        myF = true;
    }

	(*found) = false;

	VAddr oaddr = addr;
	int z = 0;

	while(addr%4 != 0){
		addr--;
		z++;
	}

	if(z > 2) {
		fail("Potential Memory LHW Issue: %#10lx\n",addr);
    }

	map<VAddr, MEMUNIT>::iterator it;
	it = memMap.find(addr);
	if(it != memMap.end()){
		MEMUNIT mem = it->second;
		uint16_t val = (uint16_t)((mem >> (8 * z)) & 0xFFFF);
		(*found) = true;
        if(myF == false) { fail("Find fail in load16(0x%lx)\n", addr); }
        if(myRV != val) { fail("Bug in load16(0x%lx)\n", addr); }
		return val;
	}
	return 0;
}

uint32_t TMStorage::load32(VAddr addr, bool *found)
{
    bool myF = false;
    uint32_t myRV = 0;
    VAddr cAddr = computeCAddr(addr);
    VAddr cOff = computeCOffset(addr);
    TnxStorage::iterator iStorage = tnxStorage.find(cAddr);
    if(iStorage != tnxStorage.end()) {
        CacheLine& line = iStorage->second;
        myRV = *((uint32_t*)(line.data + cOff));
        myF = true;
    }

	(*found) = false;
	if(addr % 4 != 0) {
		fail("Potential Memory LW Issue: %#10lx\n",addr);
    }

	map<VAddr, MEMUNIT>::iterator it; 
	it = memMap.find(addr);
	if(it != memMap.end()){
		(*found) = true;
		uint32_t val = (uint32_t)(it->second & 0xFFFFFFFF);
        if(myF == false) { fail("Find fail in load32(0x%lx)\n", addr); }
        if(myRV != val) { fail("Bug in load32(0x%lx=0x%lx)\n", myRV, val); }
		return val;
	}   
	return 0;
}

uint64_t TMStorage::load64(VAddr addr, bool *found)
{
    bool myF = false;
    uint64_t myRV = 0;
    VAddr cAddr = computeCAddr(addr);
    VAddr cOff = computeCOffset(addr);
    TnxStorage::iterator iStorage = tnxStorage.find(cAddr);
    if(iStorage != tnxStorage.end()) {
        CacheLine& line = iStorage->second;
        myRV = *((uint64_t*)(line.data + cOff));
        myF = true;
    }

	if(addr % 4 != 0) {
		fail("Potential Memory LDFP Issue: %#10lx\n",addr);
    }

	(*found) = false;

	bool r1, r2;
	uint32_t upper = load32(addr+4, &r1);
	uint32_t lower = load32(addr, &r2);
	if(r1 && r2) {
		(*found) = true;
		uint64_t retval = (uint64_t) ((((uint64_t)upper) << 32 )+lower);
        if(myF == false) { fail("Find fail in load64(0x%lx)\n", addr); }
        if(myRV != retval) { fail("Bug in load64(0x%lx=0x%lx)\n", myRV, retval); }
		return retval;
	}
	return 0;
}

void TMStorage::store(ThreadContext *context, VAddr addr, int8_t val) {
    VAddr cAddr = computeCAddr(addr);
    VAddr cOff = computeCOffset(addr);
    TnxStorage::iterator iStorage = tnxStorage.find(cAddr);
    if(iStorage != tnxStorage.end()) {
        CacheLine& line = iStorage->second;
        *((int8_t*)(line.data + cOff)) = val;
    }

	int z = 0;
	MEMUNIT curMemValue;

	VAddr oaddr = addr;

	while(addr%4 != 0){
		addr--;
		z++;
	}

	map<VAddr, MEMUNIT>::iterator it;
	it = memMap.find(addr);
	if(it != memMap.end())
		curMemValue = it->second;
	else {
        curMemValue = context->getAddressSpace()->read<MEMUNIT>(addr);
	}

	memMap[addr] = (val << (8 * z)) | (curMemValue & ~(0xff << (z * 8)));
}
void TMStorage::store(ThreadContext *context, VAddr addr, uint8_t val) {
    VAddr cAddr = computeCAddr(addr);
    VAddr cOff = computeCOffset(addr);
    TnxStorage::iterator iStorage = tnxStorage.find(cAddr);
    if(iStorage != tnxStorage.end()) {
        CacheLine& line = iStorage->second;
        *((uint8_t*)(line.data + cOff)) = val;
    }
	int z = 0;
	MEMUNIT curMemValue;

	VAddr oaddr = addr;

	while(addr%4 != 0){
		addr--;
		z++;
	}

	map<VAddr, MEMUNIT>::iterator it;
	it = memMap.find(addr);
	if(it != memMap.end())
		curMemValue = it->second;
	else {
        curMemValue = context->getAddressSpace()->read<MEMUNIT>(addr);
	}

	memMap[addr] = (val << (8 * z)) | (curMemValue & ~(0xff << (z * 8)));
}

void TMStorage::store(ThreadContext *context, VAddr addr, int16_t val) {
    VAddr cAddr = computeCAddr(addr);
    VAddr cOff = computeCOffset(addr);
    TnxStorage::iterator iStorage = tnxStorage.find(cAddr);
    if(iStorage != tnxStorage.end()) {
        CacheLine& line = iStorage->second;
        *((int16_t*)(line.data + cOff)) = val;
    }
	int z = 0;
	MEMUNIT curMemValue;

	VAddr oaddr = addr;

	while(addr%4 != 0){
		addr--;
		z++;
	}

	if(z > 2) {
		fail("Potential Memory LDFP Issue: %#10lx\n",addr);
    }

	map<VAddr, MEMUNIT>::iterator it;
	it = memMap.find(addr);
	if(it != memMap.end())
		curMemValue = it->second;
	else
		curMemValue = context->getAddressSpace()->read<MEMUNIT>(addr);

	memMap[addr] = (val << (16 * z)) | (curMemValue & ~(0xff << (z * 16)));
}
void TMStorage::store(ThreadContext *context, VAddr addr, uint16_t val) {
    VAddr cAddr = computeCAddr(addr);
    VAddr cOff = computeCOffset(addr);
    TnxStorage::iterator iStorage = tnxStorage.find(cAddr);
    if(iStorage != tnxStorage.end()) {
        CacheLine& line = iStorage->second;
        *((uint16_t*)(line.data + cOff)) = val;
    }
	int z = 0;
	MEMUNIT curMemValue;

	VAddr oaddr = addr;

	while(addr%4 != 0){
		addr--;
		z++;
	}

	if(z > 2) {
		fail("Potential Memory LDFP Issue: %#10lx\n",addr);
    }

	map<VAddr, MEMUNIT>::iterator it;
	it = memMap.find(addr);
	if(it != memMap.end()) {
		curMemValue = it->second;
	} else {
		curMemValue = context->getAddressSpace()->read<MEMUNIT>(addr);
    }

	memMap[addr] = (val << (16 * z)) | (curMemValue & ~(0xff << (z * 16)));
}
void TMStorage::store(ThreadContext *context, VAddr addr, int32_t val) {
    VAddr cAddr = computeCAddr(addr);
    VAddr cOff = computeCOffset(addr);
    TnxStorage::iterator iStorage = tnxStorage.find(cAddr);
    if(iStorage != tnxStorage.end()) {
        CacheLine& line = iStorage->second;
        *((int32_t*)(line.data + cOff)) = val;
    }
	if(addr % 4 != 0) {
		fail("Potential Memory SW Issue: %#10lx\n",addr);
    }

	memMap[addr]=val;
}
void TMStorage::store(ThreadContext *context, VAddr addr, uint32_t val) {
    VAddr cAddr = computeCAddr(addr);
    VAddr cOff = computeCOffset(addr);
    TnxStorage::iterator iStorage = tnxStorage.find(cAddr);
    if(iStorage != tnxStorage.end()) {
        CacheLine& line = iStorage->second;
        *((uint32_t*)(line.data + cOff)) = val;
    }
	if(addr % 4 != 0) {
		fail("Potential Memory SW Issue: %#10lx\n",addr);
    }

	memMap[addr]=val;
}
void TMStorage::store(ThreadContext *context, VAddr addr, int64_t val) {
    VAddr cAddr = computeCAddr(addr);
    VAddr cOff = computeCOffset(addr);
    TnxStorage::iterator iStorage = tnxStorage.find(cAddr);
    if(iStorage != tnxStorage.end()) {
        CacheLine& line = iStorage->second;
        *((int64_t*)(line.data + cOff)) = val;
    }
	if(addr % 4 != 0) {
		fail("Potential Memory SDFP Issue: %#10lx\n",addr);
    }

    memMap[addr] = lower32(val);
    memMap[addr + 4] = upper32(val);
}
void TMStorage::store(ThreadContext *context, VAddr addr, uint64_t val) {
    VAddr cAddr = computeCAddr(addr);
    VAddr cOff = computeCOffset(addr);
    TnxStorage::iterator iStorage = tnxStorage.find(cAddr);
    if(iStorage != tnxStorage.end()) {
        CacheLine& line = iStorage->second;
        *((uint64_t*)(line.data + cOff)) = val;
    }
	if(addr % 4 != 0) {
		fail("Potential Memory SDFP Issue: %#10lx\n",addr);
    }

    memMap[addr] = lower32(val);
    memMap[addr + 4] = upper32(val);
}

void TMStorage::loadLine(ThreadContext* context, VAddr addr) {
    if(inTnxStorage(addr) == false) {
        CacheLine line;
        VAddr cAddr = computeCAddr(addr);
        for(uint32_t offset = 0; offset < CACHE_SIZE; offset += sizeof(uint32_t)) {
            uint32_t word  = context->getAddressSpace()->read<uint32_t>(cAddr + offset);
            *(uint32_t*)(line.data + offset) = word;
        }
        tnxStorage.insert(make_pair(cAddr, line));
    }
}
void TMStorage::flush(ThreadContext* context) {
	map<VAddr, MEMUNIT>::iterator begin = memMap.begin();
	map<VAddr, MEMUNIT>::iterator end = memMap.end();
	for(; begin != end; ++begin) {
		context->getAddressSpace()->write<MEMUNIT>(begin->first, begin->second);
	}
}

TMStorage2::CacheLine::CacheLine(): dirty(false) {
    std::fill(data, data + CACHE_SIZE, 0);
}
void TMStorage2::loadLine(ThreadContext* context, VAddr addr) {
    if(inTnxStorage(addr) == false) {
        CacheLine line;
        VAddr cAddr = computeCAddr(addr);
        for(uint32_t offset = 0; offset < CACHE_SIZE; offset += sizeof(uint32_t)) {
            uint32_t word  = context->getAddressSpace()->read<uint32_t>(cAddr + offset);
            *(uint32_t*)(line.data + offset) = word;
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
                context->getAddressSpace()->write<uint32_t>(cAddr + offset, word);
            }
        }
    }
	tnxStorage.clear();
}
