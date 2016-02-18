PROJECT(mem)

SET(mem_SOURCES
    AddressPrefetcher.cpp
    AlwaysPrefetch.cpp
    Bank.cpp
    Bus.cpp
    Cache.cpp
    MarkovPrefetcher.cpp
    MemCtrl.cpp
    MemoryOS.cpp
    MemorySystem.cpp
    PriorityBus.cpp
    StridePrefetcher.cpp
    TLB.cpp
)
SET(mem_HEADERS
    AddressPrefetcher.h
    AlwaysPrefetch.h
    Bank.h
    Bus.h
    Cache.h
    MarkovPrefetcher.h
    MemCtrl.h
    MemoryOS.h
    MemorySystem.h
    PriorityBus.h
    StridePrefetcher.h
    TLB.h
)

ADD_LIBRARY(mem ${mem_SOURCES} ${mem_HEADERS})
TARGET_LINK_LIBRARIES(mem core suc)
