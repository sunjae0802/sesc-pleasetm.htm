PROJECT(emul)

SET(emul_SOURCES
    AddressSpace.cpp
    ElfObject.cpp
    EmulInit.cpp
    FileSys.cpp
    InstDesc.cpp
    LinuxSys.cpp
    SignalHandling.cpp
)
SET(emul_HEADERS
    ABIDefs.h
    ABIDefsMips32.h
    ABIDefsMips64.h
    ABIDefsMipsel32.h
    ABIDefsMipsel64.h
    ABIDefsMipselN32.h
    ABIDefsMipsN32.h
    Addressing.h
    AddressSpace.h
    ArchDefs.h
    Checkpoint.h
    ElfObject.h
    EmulInit.h
    EndianDefs.h
    ExecMode.h
    FileSys.h
    InstDesc.h
    LinuxSys.h
    MemState.h
    MipsRegs.h
    Regs.h
    SignalHandling.h
)

ADD_LIBRARY(emul ${emul_SOURCES} ${emul_HEADERS})
