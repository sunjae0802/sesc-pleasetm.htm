PROJECT(ll)

SET(ll_SOURCES
    ExecutionFlow.cpp
    GFlow.cpp
    Instruction.cpp
    ThreadContext.cpp
    ThreadStats.cpp
)
SET(ll_HEADERS
    Events.h
    ExecutionFlow.h
    GFlow.h
    Instruction.h
    InstType.h
    ThreadContext.h
    ThreadStats.h
)

ADD_LIBRARY(ll ${ll_SOURCES} ${ll_HEADERS})
