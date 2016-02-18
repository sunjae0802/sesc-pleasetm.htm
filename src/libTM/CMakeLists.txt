PROJECT(TM)

SET(TM_SOURCES
    PrivateCache.cpp
    TMCoherence.cpp
    TMContext.cpp
    TMState.cpp
    TMStorage.cpp
)
SET(TM_HEADERS
    PrivateCache.h
    TMCoherence.h
    TMContext.h
    TMState.h
    TMStorage.h
)

ADD_LIBRARY(TM ${TM_SOURCES} ${TM_HEADERS})
