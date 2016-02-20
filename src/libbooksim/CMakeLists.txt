PROJECT(booksim)

FLEX_TARGET(configlex config.l ${CMAKE_CURRENT_BINARY_DIR}/configlex.cpp)
BISON_TARGET(config_tab config.y ${CMAKE_CURRENT_BINARY_DIR}/config.tab.cpp)

SET(booksim_SOURCES
    config_utils.cpp
    booksim_config.cpp
    module.cpp
    buffer.cpp
    vc.cpp
    routefunc.cpp
    traffic.cpp
    flitchannel.cpp
    trafficmanager.cpp
    packet_reply_info.cpp
    buffer_state.cpp
    stats.cpp
    credit.cpp
    outputset.cpp
    flit.cpp
    injection.cpp
    misc_utils.cpp
    rng_wrapper.cpp
    rng_double_wrapper.cpp

    networks/anynet.cpp
    networks/cmesh.cpp
    networks/dragonfly.cpp
    networks/fattree.cpp
    networks/flatfly_onchip.cpp
    networks/fly.cpp
    networks/kncube.cpp
    networks/network.cpp
    networks/qtree.cpp
    networks/tree4.cpp

    allocators/allocator.cpp
    allocators/islip.cpp
    allocators/loa.cpp
    allocators/maxsize.cpp
    allocators/pim.cpp
    allocators/selalloc.cpp
    allocators/separable.cpp
    allocators/separable_input_first.cpp
    allocators/separable_output_first.cpp
    allocators/wavefront.cpp

    arbiters/arbiter.cpp
    arbiters/matrix_arb.cpp
    arbiters/prio_arb.cpp
    arbiters/roundrobin_arb.cpp
    arbiters/tree_arb.cpp

    routers/chaos_router.cpp
    routers/event_router.cpp
    routers/iq_router.cpp
    routers/router.cpp

    power/buffer_monitor.cpp
    power/power_module.cpp
    power/switch_monitor.cpp
)
SET(booksim_HEADERS
    module.hpp
    flit.hpp
    globals.hpp
    trafficmanager.hpp
    outputset.hpp
    routefunc.hpp
    buffer_state.hpp
    packet_reply_info.hpp
    buffer.hpp
    booksim.hpp
    channel.hpp
    booksim_config.hpp
    vc.hpp
    stats.hpp
    traffic.hpp
    config_utils.hpp
    pipefifo.hpp
    credit.hpp
    injection.hpp
    random_utils.hpp
    timed_module.hpp
    misc_utils.hpp
    flitchannel.hpp

    arbiters/tree_arb.hpp
    arbiters/roundrobin_arb.hpp
    arbiters/matrix_arb.hpp
    arbiters/arbiter.hpp
    arbiters/prio_arb.hpp

    power/switch_monitor.hpp
    power/power_module.hpp
    power/buffer_monitor.hpp

    allocators/separable.hpp
    allocators/islip.hpp
    allocators/selalloc.hpp
    allocators/maxsize.hpp
    allocators/wavefront.hpp
    allocators/loa.hpp
    allocators/separable_output_first.hpp
    allocators/separable_input_first.hpp
    allocators/pim.hpp
    allocators/allocator.hpp

    networks/tree4.hpp
    networks/network.hpp
    networks/anynet.hpp
    networks/fly.hpp
    networks/cmesh.hpp
    networks/fattree.hpp
    networks/flatfly_onchip.hpp
    networks/qtree.hpp
    networks/kncube.hpp
    networks/dragonfly.hpp

    routers/event_router.hpp
    routers/iq_router.hpp
    routers/chaos_router.hpp
    routers/router.hpp
)
INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/header-deps.cmake)

ADD_LIBRARY(booksim ${booksim_SOURCES} ${booksim_HEADERS} ${BISON_config_tab_OUTPUTS} ${FLEX_configlex_OUTPUTS})
