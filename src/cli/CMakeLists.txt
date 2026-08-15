add_executable(steamlessctl
    main.cpp
    IpcClient.h
    IpcClient.cpp
)
# sc_core only for its binding/key-name catalogs (BindingText, KeyNames) used
# to render/parse binding strings — no platform backend link needed here,
# the CLI never touches hardware directly.
target_link_libraries(steamlessctl PRIVATE sc_ipc sc_core)
sc_target_defaults(steamlessctl)
