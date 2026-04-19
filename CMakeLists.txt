cmake_minimum_required(VERSION 3.16)
project(BarsikTerm)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Core)

add_executable(BarsikTerm
    src/main.cpp
    src/MainWindow.cpp
    src/TerminalWidget.cpp
)

target_link_libraries(BarsikTerm Qt6::Widgets Qt6::Core)