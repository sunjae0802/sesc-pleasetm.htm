
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
		return retVal;
	}
	return 0;
}

uint16_t TMStorage::load16(VAddr addr, bool *found)
{
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
		return val;
	}
	return 0;
}

uint32_t TMStorage::load32(VAddr addr, bool *found)
{
	(*found) = false;
	if(addr % 4 != 0) {
		fail("Potential Memory LW Issue: %#10lx\n",addr);
    }

	map<VAddr, MEMUNIT>::iterator it; 
	it = memMap.find(addr);
	if(it != memMap.end()){
		(*found) = true;
		return (uint32_t)(it->second & 0xFFFFFFFF);
	}   
	return 0;
}

uint64_t TMStorage::load64(VAddr addr, bool *found)
{
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
		return retval;
	}
	return 0;
}

void TMStorage::store(ThreadContext *context, VAddr addr, int8_t val) {
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
	if(addr % 4 != 0) {
		fail("Potential Memory SW Issue: %#10lx\n",addr);
    }

	memMap[addr]=val;
}
void TMStorage::store(ThreadContext *context, VAddr addr, uint32_t val) {
	if(addr % 4 != 0) {
		fail("Potential Memory SW Issue: %#10lx\n",addr);
    }

	memMap[addr]=val;
}
void TMStorage::store(ThreadContext *context, VAddr addr, int64_t val) {
	if(addr % 4 != 0) {
		fail("Potential Memory SDFP Issue: %#10lx\n",addr);
    }

    memMap[addr] = lower32(val);
    memMap[addr + 4] = upper32(val);
}
void TMStorage::store(ThreadContext *context, VAddr addr, uint64_t val) {
	if(addr % 4 != 0) {
		fail("Potential Memory SDFP Issue: %#10lx\n",addr);
    }

    memMap[addr] = lower32(val);
    memMap[addr + 4] = upper32(val);
}

void TMStorage::flush(ThreadContext* context) {
	map<VAddr, MEMUNIT>::iterator begin = memMap.begin();
	map<VAddr, MEMUNIT>::iterator end = memMap.end();
	for(; begin != end; ++begin) {
		context->getAddressSpace()->write<MEMUNIT>(begin->first, begin->second);
	}
}


