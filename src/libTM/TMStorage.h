#ifndef TM_CACHE
#define TM_CACHE

#include <map>
#include "libemul/Addressing.h"
class ThreadContext;

class TMStorage2 {
  const static size_t CACHE_SIZE = 64;
  struct CacheLine {
    CacheLine();
    uint8_t data[CACHE_SIZE];
    bool dirty;
  };
  VAddr computeCAddr(VAddr addr) {
      return ((addr / CACHE_SIZE) * CACHE_SIZE);
  }
  size_t computeCOffset(VAddr addr) {
    return addr % CACHE_SIZE;
  }
  VAddr recompAddr(VAddr addr) {
      return computeCAddr(addr) + computeCOffset(addr);
  }

  public:
    /* Contructor */
    TMStorage2() {}

    bool inTnxStorage(VAddr addr) {
        VAddr cAddr = computeCAddr(addr);
        TnxStorage::iterator iStorage = tnxStorage.find(cAddr);
        return iStorage != tnxStorage.end();
    }
	template<class T>
    T load(VAddr addr);
	template<class T>
    void store(ThreadContext *context, VAddr addr, T val);
   
    void loadLine(ThreadContext* context, VAddr addr);
	void flush(ThreadContext* context);

    /* Deconstructor */
    ~TMStorage2() {}

  private:

     typedef std::map<VAddr, CacheLine> TnxStorage;
     TnxStorage tnxStorage; //!< Speculative storage
};

template<class T>
T TMStorage2::load(VAddr addr)
{
    T myRV = 0;
    VAddr cAddr = computeCAddr(addr);
    VAddr cOff = computeCOffset(addr);
    TnxStorage::iterator iStorage = tnxStorage.find(cAddr);
    if(iStorage != tnxStorage.end()) {
        CacheLine& line = iStorage->second;
        myRV = *(reinterpret_cast<T*>(line.data + cOff));
        return myRV;
    } else {
        return 0xCC;
    }
}

template<class T>
void TMStorage2::store(ThreadContext *context, VAddr addr, T val) {
    VAddr cAddr = computeCAddr(addr);
    VAddr cOff = computeCOffset(addr);
    TnxStorage::iterator iStorage = tnxStorage.find(cAddr);
    if(iStorage != tnxStorage.end()) {
        CacheLine& line = iStorage->second;
        *(reinterpret_cast<T*>(line.data + cOff)) = val;
        line.dirty = true;
    }
}

#endif
