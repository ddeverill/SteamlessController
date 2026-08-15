# Shared compiler-flag helpers, replacing the copy-pasted `if(MSVC) ... endif()`
# blocks the original flat CMakeLists.txt had on every target.

function(sc_target_defaults tgt)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

# Same as sc_target_defaults, but silences the ViGEmClient/WebView2-adjacent
# warnings those targets can't fix themselves (MSVC C4828 BOM warning).
function(sc_target_defaults_relaxed tgt)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /WX /utf-8 /wd4828)
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

# The debug-console-subsystem dance every Windows probe target used to repeat.
function(sc_console_target tgt)
    if(MSVC)
        set_target_properties(${tgt} PROPERTIES LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE")
    endif()
endfunction()
