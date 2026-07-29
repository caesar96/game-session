cmake_minimum_required(VERSION 3.16)
project(game-session LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra)

include(GNUInstallDirs)

add_executable(game-session game-session.cpp amdgpu_overdrive.cpp)
add_executable(game-session-helper game-session-helper.cpp amdgpu_overdrive.cpp)

find_package(Threads REQUIRED)
target_link_libraries(game-session PRIVATE Threads::Threads)

include(CTest)
if(BUILD_TESTING)
    add_executable(game-session-tests tests/game_session_tests.cpp amdgpu_overdrive.cpp)
    target_link_libraries(game-session-tests PRIVATE Threads::Threads)
    add_test(NAME game-session-tests COMMAND game-session-tests)
endif()

install(TARGETS game-session game-session-helper
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

set(BINDIR "${CMAKE_INSTALL_FULL_BINDIR}")
set(USERNAME "$ENV{SUDO_USER}")
if(USERNAME STREQUAL "")
    set(USERNAME "$ENV{USER}")
endif()
configure_file(
    sudo/game-session.in
    sudo/game-session
    @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sudo/game-session"
        DESTINATION /etc/sudoers.d
        COMPONENT system
        PERMISSIONS OWNER_READ GROUP_READ)

configure_file(cmake/uninstall.cmake.in uninstall.cmake @ONLY)
add_custom_target(uninstall
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake"
    COMMENT "Removing game-session files…")
