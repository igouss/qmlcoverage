cmake_minimum_required(VERSION 3.21)
project(qmlcoverage LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Network Qml QmlPrivate QmlDebugPrivate Test)

# Laid out as <build>/plugins/qmltooling so the directory can go on QT_PLUGIN_PATH.
add_library(qmldbg_coverage MODULE
    plugin/qqmlcoverageservice.cpp plugin/qqmlcoverageservice.h
    plugin/qqmlcoverageservicefactory.cpp plugin/qqmlcoverageservicefactory.h
    plugin/coverageprotocol.h)
target_link_libraries(qmldbg_coverage PRIVATE Qt6::Core Qt6::QmlPrivate)
set_target_properties(qmldbg_coverage PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/qmltooling)

add_executable(qmlcoverage client/main.cpp plugin/coverageprotocol.h)
target_include_directories(qmlcoverage PRIVATE plugin)
target_link_libraries(qmlcoverage PRIVATE Qt6::Core Qt6::Network Qt6::QmlDebugPrivate)

# Test support: the plugin built as if for an older and a newer Qt, to check the version guard
# refuses both. Each lands in <build>/<name>/plugins.
function(add_plugin_built_for name version)
    add_library(qmldbg_coverage_${name} MODULE
        plugin/qqmlcoverageservice.cpp plugin/qqmlcoverageservice.h
        plugin/qqmlcoverageservicefactory.cpp plugin/qqmlcoverageservicefactory.h
        plugin/coverageprotocol.h)
    target_compile_definitions(qmldbg_coverage_${name} PRIVATE QMLCOVERAGE_BUILT_FOR_QT="${version}")
    target_link_libraries(qmldbg_coverage_${name} PRIVATE Qt6::Core Qt6::QmlPrivate)
    set_target_properties(qmldbg_coverage_${name} PROPERTIES
        OUTPUT_NAME qmldbg_coverage
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${name}/plugins/qmltooling)
endfunction()
add_plugin_built_for(olderqt 0.0.0)
add_plugin_built_for(newerqt 99.0.0)

# Test support: two QML engines alive at once.
add_executable(twoapp tests/twoapp/main.cpp)
target_link_libraries(twoapp PRIVATE Qt6::Core Qt6::Qml)

enable_testing()
add_executable(tst_protocol tests/protocol/tst_protocol.cpp plugin/coverageprotocol.h)
target_include_directories(tst_protocol PRIVATE plugin)
target_link_libraries(tst_protocol PRIVATE Qt6::Core Qt6::Test)
add_test(NAME protocol COMMAND tst_protocol)
add_test(NAME fixtures
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/check.sh ${CMAKE_BINARY_DIR})
