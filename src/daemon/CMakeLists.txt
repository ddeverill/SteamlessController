add_executable(steamlesscontrollerd
    main.cpp
    Daemon.h
    Daemon.cpp
    IpcServer.h
    IpcServer.cpp
    SdNotify.h
    SdNotify.cpp
)
target_link_libraries(steamlesscontrollerd PRIVATE sc_platform_linux sc_ipc)
sc_target_defaults(steamlesscontrollerd)
