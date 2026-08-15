# Daemon<->CLI wire protocol: hand-rolled JSON + Unix domain socket helpers.
add_library(sc_ipc STATIC
    Json.h
    Json.cpp
    UnixSocket.h
    UnixSocket.cpp
)
target_include_directories(sc_ipc PUBLIC ${CMAKE_SOURCE_DIR}/src)
sc_target_defaults(sc_ipc)
