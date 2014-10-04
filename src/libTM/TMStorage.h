#ifndef TM_CACHE
#define TM_CACHE

#include <map>
#include "SescConf.h"
#include "libemul/Addressing.h"
//#include "mendian.h"
#include "libemul/Regs.h"
class ThreadContext;

/*
 * #define SWAP_WORD(X) (((((unsigned int)(X)) >> 24) & 0x000000ff) | \
                ((((unsigned int)(X)) >>  8) & 0x0000ff00) | \
                             ((((unsigned int)(X)) <<  8) & 0x00ff0000) | \
                             ((((unsigned int)(X)) << 24) & 0xff000000))


#define SWAP_SHORT(X) ( ((((unsigned short)X)& 0xff00) >> 8) | ((((unsigned short)X)& 0x00ff) << 8) )
*/

//typedef int32_t RegVal;


typedef uint32_t MEMUNIT;

class TMStorage {
  public:
    /* Contructor */
    TMStorage();

	uint8_t  load8(VAddr addr, bool *found);
	uint16_t load16(VAddr addr, bool *found);
	uint32_t load32(VAddr addr, bool *found);
	uint64_t load64(VAddr addr, bool *found);

	void store(ThreadContext *context, VAddr addr, int8_t val);
	void store(ThreadContext *context, VAddr addr, uint8_t val);
	void store(ThreadContext *context, VAddr addr, int16_t val);
	void store(ThreadContext *context, VAddr addr, uint16_t val);
	void store(ThreadContext *context, VAddr addr, int32_t val);
	void store(ThreadContext *context, VAddr addr, uint32_t val);
	void store(ThreadContext *context, VAddr addr, int64_t val);
	void store(ThreadContext *context, VAddr addr, uint64_t val);
    
	void flush(ThreadContext* context);

    /* Deconstructor */
    ~TMStorage();

  private:
    uint32_t lower32(uint64_t val) {
        return  (val & 0x00000000FFFFFFFFLL);
    }
    uint32_t upper32(uint64_t val) {
        return ((val & 0xFFFFFFFF00000000LL) >> 32);
    }
     std::map<VAddr, MEMUNIT>     memMap; //!< The Memory Map
};

#endif
