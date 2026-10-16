# OpenLCB/LCC node library. Plain C11 with no platform dependencies.
add_library(lcc STATIC
    src/node.c
    src/events.c
    src/datagram.c
    src/memconfig.c
    src/gridconnect.c
)

target_include_directories(lcc PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
