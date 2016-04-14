PROJECT(cmp)

SET(cmp_SOURCES
    cmp.cpp
    DMESIProtocol.cpp
    SMemorySystem.cpp
    SMPCache.cpp
    SMPMemCtrl.cpp
    SMPMemRequest.cpp
    SMPNOC.cpp
    SMPProtocol.cpp
    SMPRouter.cpp
    SMPSliceCache.cpp
    SMPSystemBus.cpp
)
SET(cmp_HEADERS
    DMESIProtocol.h
    SMemorySystem.h
    SMPCache.h
    SMPCacheState.h
    SMPDebug.h
    SMPDirectory.h
    SMPMemCtrl.h
    SMPMemRequest.h
    SMPNOC.h
    SMPProtocol.h
    SMPRouter.h
    SMPSliceCache.h
    SMPSystemBus.h
)

ADD_EXECUTABLE(sesc ${cmp_SOURCES} ${cmp_HEADERS})
TARGET_LINK_LIBRARIES(sesc ll TM booksim core emul suc mem)
