cmake_minimum_required(VERSION 3.24)

project(dui VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

option(DUI_BUILD_EXAMPLES "Build DUI examples" ON)
option(DUI_BUILD_TESTS "Build DUI tests" ON)
option(DUI_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

function(dui_configure_target target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
        if(DUI_ENABLE_SANITIZERS)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endfunction()

add_library(dui src/animation.cpp src/backend.cpp src/input.cpp src/rendering.cpp src/task.cpp src/ui.cpp)
add_library(dui::dui ALIAS dui)

target_include_directories(dui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(dui PUBLIC cxx_std_23)
target_link_libraries(dui PUBLIC Threads::Threads)
dui_configure_target(dui)

if(WIN32)
    add_library(dui_win32 STATIC src/platform/win32_accessibility.cpp src/platform/win32_text_input.cpp)
    add_library(dui::win32 ALIAS dui_win32)
    target_link_libraries(dui_win32 PUBLIC dui::dui PRIVATE imm32 oleaut32 uiautomationcore user32)
    target_include_directories(dui_win32 PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_features(dui_win32 PUBLIC cxx_std_23)
    dui_configure_target(dui_win32)
endif()

if(DUI_BUILD_EXAMPLES)
    add_executable(dui_counter examples/counter.cpp)
    target_link_libraries(dui_counter PRIVATE dui::dui)
    dui_configure_target(dui_counter)
endif()

if(DUI_BUILD_TESTS)
    enable_testing()
    add_executable(dui_core_tests tests/core_tests.cpp)
    target_link_libraries(dui_core_tests PRIVATE dui::dui)
    dui_configure_target(dui_core_tests)
    add_test(NAME dui_core_tests COMMAND dui_core_tests)

    add_executable(dui_rendering_tests tests/rendering_tests.cpp)
    target_link_libraries(dui_rendering_tests PRIVATE dui::dui)
    dui_configure_target(dui_rendering_tests)
    add_test(NAME dui_rendering_tests COMMAND dui_rendering_tests)

    add_executable(dui_lazy_stabilization_tests tests/lazy_stabilization_tests.cpp)
    target_link_libraries(dui_lazy_stabilization_tests PRIVATE dui::dui)
    dui_configure_target(dui_lazy_stabilization_tests)
    add_test(NAME dui_lazy_stabilization_tests COMMAND dui_lazy_stabilization_tests)

    add_executable(dui_semantics_tests tests/semantics_tests.cpp)
    target_link_libraries(dui_semantics_tests PRIVATE dui::dui)
    dui_configure_target(dui_semantics_tests)
    add_test(NAME dui_semantics_tests COMMAND dui_semantics_tests)

    add_executable(dui_input_tests tests/input_tests.cpp)
    target_link_libraries(dui_input_tests PRIVATE dui::dui)
    dui_configure_target(dui_input_tests)
    add_test(NAME dui_input_tests COMMAND dui_input_tests)

    add_executable(dui_lifecycle_tests tests/lifecycle_tests.cpp)
    target_link_libraries(dui_lifecycle_tests PRIVATE dui::dui)
    dui_configure_target(dui_lifecycle_tests)
    add_test(NAME dui_lifecycle_tests COMMAND dui_lifecycle_tests)

    add_executable(dui_backend_tests tests/backend_tests.cpp)
    target_link_libraries(dui_backend_tests PRIVATE dui::dui)
    dui_configure_target(dui_backend_tests)
    add_test(NAME dui_backend_tests COMMAND dui_backend_tests)

    if(WIN32)
        add_executable(dui_win32_accessibility_tests tests/win32_accessibility_tests.cpp)
        target_link_libraries(dui_win32_accessibility_tests PRIVATE dui::win32)
        dui_configure_target(dui_win32_accessibility_tests)
        add_test(NAME dui_win32_accessibility_tests COMMAND dui_win32_accessibility_tests)

        add_executable(dui_win32_text_input_tests tests/win32_text_input_tests.cpp)
        target_link_libraries(dui_win32_text_input_tests PRIVATE dui::win32)
        dui_configure_target(dui_win32_text_input_tests)
        add_test(NAME dui_win32_text_input_tests COMMAND dui_win32_text_input_tests)
    endif()
endif()
