cmake_minimum_required(VERSION 3.16)
project(game-session LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(game-session game-session.cpp)
add_executable(game-session-helper game-session-helper.cpp)

install(TARGETS game-session game-session-helper
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

set(BINDIR "${CMAKE_INSTALL_FULL_BINDIR}")
set(USERNAME "$ENV{USER}")
configure_file(
    sudo/game-session.sudoers.in
    sudo/game-session.sudoers
    @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sudo/game-session.sudoers"
        DESTINATION /etc/sudoers.d
        COMPONENT system)
