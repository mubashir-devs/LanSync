add_library(lansync_options INTERFACE)

if(MSVC)
    target_compile_options(lansync_options INTERFACE /W4 /permissive-)
    if(LANSYNC_WARNINGS_AS_ERRORS)
        target_compile_options(lansync_options INTERFACE /WX)
    endif()
else()
    target_compile_options(lansync_options INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor
        -Woverloaded-virtual)
    if(LANSYNC_WARNINGS_AS_ERRORS)
        target_compile_options(lansync_options INTERFACE -Werror)
    endif()
    if(LANSYNC_ENABLE_TSAN)
        target_compile_options(lansync_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(lansync_options INTERFACE -fsanitize=thread)
    endif()
    if(LANSYNC_ENABLE_SANITIZERS)
        target_compile_options(lansync_options INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(lansync_options INTERFACE -fsanitize=address,undefined)
    endif()
endif()
