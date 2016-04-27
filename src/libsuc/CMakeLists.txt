PROJECT(suc)

INCLUDE_DIRECTORIES(${suc_SOURCE_DIR})

FLEX_TARGET(conflex conflex.l ${CMAKE_CURRENT_BINARY_DIR}/conflex.cpp COMPILE_FLAGS "-Cemr")
BISON_TARGET(conflex_tab conflex.y ${CMAKE_CURRENT_BINARY_DIR}/conflex.tab.cpp COMPILE_FLAGS "-p yyConf")

SET(suc_SOURCES
    BloomFilter.cpp
    CacheCore.cpp
    callback.cpp
    Config.cpp
    GStats.cpp
    EventTrace.cpp
    MSHR.cpp
    nanassert.cpp
    Port.cpp
    ReportGen.cpp
    SCTable.cpp
    SescConf.cpp
    Snippets.cpp
    ThermTrace.cpp
    TQueue.cpp
    TraceGen.cpp

)
set(suc_HEADERS
    alloca.h
    BloomFilter.h
    CacheCore.h
    callback.h
    Config.h
    DbgObject.h
    estl.h
    EventTrace.h
    FastQueue.h
    fpu_control.h
    GCObject.h
    GEnergy.h
    GStats.h
    MSHR.h
    nanassert.h
    pool.h
    Port.h
    ReportGen.h
    ReportTherm.h
    SCTable.h
    SescConf.h
    SizedTypes.h
    Snippets.h
    ThermTrace.h
    TQueue.h
    TraceGen.h
)

ADD_LIBRARY(suc ${suc_SOURCES} ${suc_HEADERS} ${BISON_conflex_tab_OUTPUTS} ${FLEX_conflex_OUTPUTS})
TARGET_LINK_LIBRARIES(suc core)
