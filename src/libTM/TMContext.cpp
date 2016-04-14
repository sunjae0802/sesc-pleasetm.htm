
#include "libll/ThreadContext.h"
#include "libll/Instruction.h"
#include "TMContext.h"
#include "SescConf.h"

using namespace std;

TMContext::TMContext(ThreadContext* c, InstDesc* inst, uint64_t tmUtid): context(c), utid(tmUtid) {
    pid         = context->getPid();
    tmBeginCode = inst;
    beginIAddr  = inst->getSescInst()->getAddr();

    if(context->isInTM()) {
        parent  = context->getTMContext();
    } else {
        parent  = NULL;
    }
}

void TMContext::saveContext() {
	for(size_t r=0; r<NumOfRegs; r++) {
		regs[r] = *((RegVal *)context->getReg(r));
	}
}
void TMContext::restoreContext() {
	for(size_t r=0; r<NumOfRegs; r++) {
		context->setReg(r, regs[r]);
	}
}

