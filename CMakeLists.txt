cmake_minimum_required(VERSION 3.15)
project(MyProject LANGUAGES CXX)

option(BUILD_TESTS "Build tests" ON)
option(CODE_COVERAGE "Enable code coverage reporting" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# === Compiler flags for coverage ===
if(CODE_COVERAGE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(STATUS "Enabling code coverage flags")
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
  else()
    message(WARNING "Code coverage not supported for this compiler")
  endif()
endif()

# === Find Boost ===
find_package(Boost 1.71 REQUIRED)

# === Find pkg-config and gstreamer-1.0 ===
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0)

# === Add executable ===
add_executable(test_exe main.cpp)

# === Include dirs ===
target_include_directories(test_exe PRIVATE
  ${GST_INCLUDE_DIRS}
  ${Boost_INCLUDE_DIRS}
)

# === Compile flags from pkg-config ===
target_compile_options(test_exe PRIVATE ${GST_CFLAGS_OTHER})

# === Link libraries ===
target_link_libraries(test_exe PRIVATE
  ${GST_LIBRARIES}
  ${Boost_LIBRARIES}
  pthread
)

# === doctest ===
include(FetchContent)
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.9
)
FetchContent_MakeAvailable(doctest)
target_include_directories(test_exe PRIVATE ${doctest_SOURCE_DIR}/doctest)
# Uncomment this if you define tests in the same file as main() and you do not directy #define DOCTEST_CONFIG_IMPLEMENT:
# target_compile_definitions(test_exe PRIVATE DOCTEST_CONFIG_IMPLEMENT)

# === Tests ===
if(BUILD_TESTS)
  enable_testing()
  add_test(NAME doctest COMMAND test_exe)
endif()

# === Coverage target ===
if(CODE_COVERAGE)
  find_program(GCOVR_PATH gcovr)
  if(GCOVR_PATH)
    add_custom_target(coverage
      COMMAND ${GCOVR_PATH} -r ${CMAKE_SOURCE_DIR} --html --html-details -o coverage.html
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      COMMENT "Generating code coverage report"
      VERBATIM
    )
  else()
    message(WARNING "gcovr not found, coverage target will do nothing")
    add_custom_target(coverage
      COMMENT "gcovr not found, coverage target skipped"
    )
  endif()
endif()
