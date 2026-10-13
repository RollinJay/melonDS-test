# Rollback netplay: deterministic multi-console machine, GGPO session and the
# peer-to-peer transport. Independent of the Qt frontend (see docs/NETPLAY.md).

# GGPO (MIT, see ggpo/LICENSE and ggpo/README.md), built on its own so that its
# headers (types.h, log.h, platform.h) never shadow melonDS's.
add_library(ggpo STATIC
    ggpo/lib/ggpo/bitvector.cpp
    ggpo/lib/ggpo/game_input.cpp
    ggpo/lib/ggpo/input_queue.cpp
    ggpo/lib/ggpo/log.cpp
    ggpo/lib/ggpo/main.cpp
    ggpo/lib/ggpo/platform.cpp
    ggpo/lib/ggpo/poll.cpp
    ggpo/lib/ggpo/sync.cpp
    ggpo/lib/ggpo/timesync.cpp
    ggpo/lib/ggpo/backends/p2p.cpp
    ggpo/lib/ggpo/network/udp.cpp
    ggpo/lib/ggpo/network/udp_proto.cpp
)
target_include_directories(ggpo
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ggpo/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ggpo/lib/ggpo")
if (WIN32)
    target_link_libraries(ggpo PUBLIC ws2_32)
endif()
if (NOT MSVC)
    # upstream code; its warnings are not ours to fix
    target_compile_options(ggpo PRIVATE -w)
endif()

add_library(rollback-netplay STATIC
    NetplayBus.cpp
    NetplayMachine.cpp
    RollbackSession.cpp
    NetSocket.cpp
    NetJson.cpp
    P2PLink.cpp
    OnlineSession.cpp
    kcp/ikcp.c
)

target_include_directories(rollback-netplay PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rollback-netplay PUBLIC core ggpo)
if (WIN32)
    target_link_libraries(rollback-netplay PUBLIC ws2_32 iphlpapi)
endif()
