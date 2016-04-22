PROJECT(TM)

SET(TM_SOURCES
    PrivateCache.cpp
    HTMManager.cpp
    RWSetManager.cpp
    TMContext.cpp
    TMState.cpp
    TMStorage.cpp
    TSXManager.cpp
    IdealTSXManager.cpp
    LogTMManager.cpp
    FasTMManager.cpp
    PleaseTMManager.cpp
)
SET(TM_HEADERS
    PrivateCache.h
    HTMManager.h
    RWSetManager.h
    TMContext.h
    TMState.h
    TMStorage.h
    TSXManager.h
    IdealTSXManager.h
    LogTMManager.h
    FasTMManager.h
    PleaseTMManager.h
)

ADD_LIBRARY(TM ${TM_SOURCES} ${TM_HEADERS})
