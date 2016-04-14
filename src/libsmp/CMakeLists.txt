PROJECT(smp)

SET(smp_SOURCES
    MESIProtocol.cpp
    SMemorySystem.cpp
    SMPCache.cpp
    smp.cpp
    SMPMemRequest.cpp
    SMPProtocol.cpp
    SMPSystemBus.cpp
)
SET(smp_HEADERS
    MESIProtocol.h
    SMemorySystem.h
    SMPCache.h
    SMPCacheState.h
    SMPDebug.h
    SMPMemRequest.h
    SMPProtocol.h
    SMPSystemBus.h
)

ADD_EXECUTABLE(sesc ${smp_SOURCES} ${smp_HEADERS})
TARGET_LINK_LIBRARIES(sesc ll TM core emul suc mem)
