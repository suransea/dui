cmake_minimum_required(VERSION 3.24)

project(dui VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

option(DUI_BUILD_EXAMPLES "Build DUI examples" ON)
option(DUI_BUILD_TESTS "Build DUI tests" ON)
option(DUI_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(DUI_ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)
option(DUI_REQUIRE_WAYLAND_NATIVE "Fail configuration unless the native Wayland target can be built" OFF)

if(DUI_ENABLE_SANITIZERS AND DUI_ENABLE_THREAD_SANITIZER)
    message(FATAL_ERROR "Address/UndefinedBehaviorSanitizer and ThreadSanitizer are mutually exclusive")
endif()

if(DUI_ENABLE_THREAD_SANITIZER AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "ThreadSanitizer requires a Clang or GNU C++ compiler")
endif()

function(dui_configure_target target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
        if(DUI_ENABLE_SANITIZERS)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
        if(DUI_ENABLE_THREAD_SANITIZER)
            target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=thread)
        endif()
    endif()
endfunction()

add_library(dui src/animation.cpp src/backend.cpp src/host.cpp src/input.cpp src/platform/wayland.cpp src/rendering.cpp src/task.cpp src/ui.cpp)
add_library(dui::dui ALIAS dui)

target_include_directories(dui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(dui PUBLIC cxx_std_23)
target_link_libraries(dui PUBLIC Threads::Threads)
dui_configure_target(dui)

if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client>=1.22)
        pkg_check_modules(WAYLAND_PROTOCOLS QUIET wayland-protocols>=1.31)
        if(WAYLAND_PROTOCOLS_FOUND)
            pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
        endif()
    endif()
    find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner)
    set(DUI_WAYLAND_PROTOCOL_XML_AVAILABLE OFF)
    if(EXISTS "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml" AND
       EXISTS "${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml" AND
       EXISTS "${WAYLAND_PROTOCOLS_DIR}/staging/fractional-scale/fractional-scale-v1.xml")
        set(DUI_WAYLAND_PROTOCOL_XML_AVAILABLE ON)
    endif()

    if(WAYLAND_CLIENT_FOUND AND WAYLAND_PROTOCOLS_FOUND AND WAYLAND_SCANNER_EXECUTABLE AND DUI_WAYLAND_PROTOCOL_XML_AVAILABLE)
        enable_language(C)
        set(DUI_WAYLAND_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/wayland")

        function(dui_generate_wayland_protocol name xml)
            set(header "${DUI_WAYLAND_GENERATED_DIR}/${name}-client-protocol.h")
            set(code "${DUI_WAYLAND_GENERATED_DIR}/${name}-protocol.c")
            add_custom_command(
                OUTPUT "${header}" "${code}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${DUI_WAYLAND_GENERATED_DIR}"
                COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${xml}" "${header}"
                COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${xml}" "${code}"
                DEPENDS "${xml}"
                VERBATIM
            )
            set(DUI_WAYLAND_PROTOCOL_SOURCES ${DUI_WAYLAND_PROTOCOL_SOURCES} "${header}" "${code}" PARENT_SCOPE)
        endfunction()

        dui_generate_wayland_protocol(xdg-shell "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
        dui_generate_wayland_protocol(viewporter "${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml")
        dui_generate_wayland_protocol(fractional-scale-v1 "${WAYLAND_PROTOCOLS_DIR}/staging/fractional-scale/fractional-scale-v1.xml")

        add_library(dui_wayland_protocols STATIC ${DUI_WAYLAND_PROTOCOL_SOURCES})
        target_include_directories(dui_wayland_protocols PUBLIC "${DUI_WAYLAND_GENERATED_DIR}")
        target_link_libraries(dui_wayland_protocols PUBLIC PkgConfig::WAYLAND_CLIENT)

        add_library(dui_wayland_native STATIC src/platform/wayland_native.cpp)
        add_library(dui::wayland_native ALIAS dui_wayland_native)
        target_link_libraries(dui_wayland_native PUBLIC dui::dui PRIVATE dui_wayland_protocols PkgConfig::WAYLAND_CLIENT)
        target_include_directories(dui_wayland_native PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        )
        target_compile_features(dui_wayland_native PUBLIC cxx_std_23)
        dui_configure_target(dui_wayland_native)

        if(DUI_BUILD_TESTS)
            add_executable(dui_wayland_native_link_tests tests/wayland_native_link_tests.cpp)
            target_link_libraries(dui_wayland_native_link_tests PRIVATE dui::wayland_native)
            dui_configure_target(dui_wayland_native_link_tests)
            add_test(NAME dui_wayland_native_link_tests COMMAND dui_wayland_native_link_tests)
        endif()
    elseif(DUI_REQUIRE_WAYLAND_NATIVE)
        message(FATAL_ERROR "Native Wayland requires wayland-client >= 1.22, wayland-protocols >= 1.31, and wayland-scanner")
    endif()
elseif(DUI_REQUIRE_WAYLAND_NATIVE)
    message(FATAL_ERROR "Native Wayland is only available on non-Apple Unix systems")
endif()

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

    add_executable(dui_tooling_report examples/tooling_report.cpp)
    target_link_libraries(dui_tooling_report PRIVATE dui::dui)
    dui_configure_target(dui_tooling_report)
endif()

if(DUI_BUILD_TESTS)
    enable_testing()
    add_executable(dui_core_tests tests/core_tests.cpp)
    target_link_libraries(dui_core_tests PRIVATE dui::dui)
    dui_configure_target(dui_core_tests)
    add_test(NAME dui_core_tests COMMAND dui_core_tests)

    add_executable(dui_restoration_tests tests/restoration_tests.cpp)
    target_link_libraries(dui_restoration_tests PRIVATE dui::dui)
    dui_configure_target(dui_restoration_tests)
    add_test(NAME dui_restoration_tests COMMAND dui_restoration_tests)

    add_executable(dui_host_tests tests/host_tests.cpp)
    target_link_libraries(dui_host_tests PRIVATE dui::dui)
    dui_configure_target(dui_host_tests)
    add_test(NAME dui_host_tests COMMAND dui_host_tests)

    add_executable(dui_wayland_state_tests tests/wayland_state_tests.cpp)
    target_link_libraries(dui_wayland_state_tests PRIVATE dui::dui)
    dui_configure_target(dui_wayland_state_tests)
    add_test(NAME dui_wayland_state_tests COMMAND dui_wayland_state_tests)

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
