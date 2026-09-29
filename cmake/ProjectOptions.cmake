option(KEYFORGE_ENABLE_TESTS "Build KeyForge tests" ON)
option(KEYFORGE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(KEYFORGE_ENABLE_CLANG_TIDY "Run clang-tidy while compiling" OFF)
option(KEYFORGE_ENABLE_FUZZING "Build the libFuzzer RESP parser target" OFF)

function(keyforge_apply_project_options target)
    set_property(TARGET ${target} PROPERTY CXX_EXTENSIONS OFF)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
    )

    if(KEYFORGE_ENABLE_SANITIZERS)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()

    if(KEYFORGE_ENABLE_CLANG_TIDY)
        find_program(KEYFORGE_CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-15 REQUIRED)
        set_property(TARGET ${target} PROPERTY CXX_CLANG_TIDY "${KEYFORGE_CLANG_TIDY_EXE}")
    endif()
endfunction()
