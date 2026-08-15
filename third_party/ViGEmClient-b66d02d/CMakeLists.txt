# Vendored ViGEmClient, built from source (static). Its own headers trip
# warnings under /W4 /WX that this project doesn't own, so it gets the relaxed
# flag set rather than a project-wide exception.
add_library(ViGEmClient STATIC
    src/ViGEmClient.cpp
)
target_include_directories(ViGEmClient PUBLIC include)
target_link_libraries(ViGEmClient PUBLIC setupapi)
if(MSVC)
    target_compile_options(ViGEmClient PRIVATE /W0 /utf-8 /wd4828)
endif()
