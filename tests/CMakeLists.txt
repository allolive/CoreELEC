# The yacer test project.
#
# Tests live with the feature they cover, in patches-yacer/<group>/<pkg>_tests/,
# and are never copied into the build tree. They compile against a Kodi tree
# that already has every patch applied, so a test failure means our stack
# regressed rather than that a reconstruction drifted.
#
#   python3 tests/prepare-tree.py --out /tmp/kodi
#   cmake -S tests -B /tmp/build -DKODI_SOURCE=/tmp/kodi
#   cmake --build /tmp/build -j"$(nproc)"
#   ctest --test-dir /tmp/build --output-on-failure

cmake_minimum_required(VERSION 3.24)
project(yacer_tests C CXX)

if(NOT KODI_SOURCE)
  message(FATAL_ERROR "set -DKODI_SOURCE=<applied kodi tree>; tests/prepare-tree.py makes one")
endif()
if(NOT EXISTS "${KODI_SOURCE}/xbmc/cores/VideoPlayer/VideoPlayer.cpp")
  message(FATAL_ERROR "KODI_SOURCE does not look like a Kodi tree: ${KODI_SOURCE}")
endif()

option(YACER_SANITIZE "build the tests with the address and undefined-behaviour sanitizers" OFF)
option(YACER_COVERAGE "instrument the tests and the production code they compile, for gcov" OFF)

# These cases run in milliseconds. A generous timeout only means a deadlocked
# case holds the run for that long before failing.
if(YACER_SANITIZE)
  set(YACER_TEST_TIMEOUT 120)
else()
  set(YACER_TEST_TIMEOUT 30)
endif()

# Sanitizer flags belong to the whole directory, and must be set before
# googletest is configured: a sanitized test binary linked against an
# unsanitized googletest share std::vector and std::string across the
# boundary, which is what produces phantom container-overflow reports.
if(YACER_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                      -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

include(FetchContent)
include(GoogleTest)
# gmock is how a suite stands in for a collaborator it needs to control -
# ordering and cardinality are what those tests are about, and hand-rolled
# recorders express neither.
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
)
FetchContent_MakeAvailable(googletest)

enable_testing()

# Stand-ins a suite may put ahead of the tree, for a dependency that is built
# rather than carried in the source (the logger, today). Pass with
# INCLUDES ${YACER_SHIMS}.
set(YACER_SHIMS "${CMAKE_CURRENT_SOURCE_DIR}/shims")

# One binary per suite, labelled with the patch group it covers so the report
# names the feature that broke.
#
#   NAME          what the suite is called, unique within its group
#   SOURCES       test sources, relative to the group's own directory
#   KODI_SOURCES  production translation units to compile in, relative to the tree
#   INCLUDES      extra include directories
#   STANDARD      C++ standard, default 17
function(yacer_add_test)
  cmake_parse_arguments(TEST "" "NAME;STANDARD" "SOURCES;KODI_SOURCES;INCLUDES;DEFINES" ${ARGN})
  if(NOT TEST_NAME OR NOT TEST_SOURCES)
    message(FATAL_ERROR "yacer_add_test needs NAME and SOURCES")
  endif()
  # A misspelled keyword would otherwise be dropped in silence, and a suite
  # that meant to compile production code would build and pass without it.
  if(TEST_UNPARSED_ARGUMENTS OR TEST_KEYWORDS_MISSING_VALUES)
    message(FATAL_ERROR "yacer_add_test: unrecognised arguments "
                        "'${TEST_UNPARSED_ARGUMENTS}${TEST_KEYWORDS_MISSING_VALUES}'")
  endif()
  if(NOT YACER_GROUP)
    message(FATAL_ERROR "yacer_add_test called outside a patch group")
  endif()
  if(NOT TEST_STANDARD)
    set(TEST_STANDARD 17)
  endif()

  set(case "${YACER_GROUP}.${TEST_NAME}")
  string(MAKE_C_IDENTIFIER "test_${case}" target)

  set(sources "")
  foreach(source ${TEST_SOURCES})
    # SOURCES is multi-valued, so a misspelled keyword after it is absorbed as
    # another source rather than left unparsed. Naming the file that does not
    # exist says which word was wrong.
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
      message(FATAL_ERROR "${case}: no such test source: ${source}")
    endif()
    list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
  endforeach()
  foreach(source ${TEST_KODI_SOURCES})
    if(NOT EXISTS "${KODI_SOURCE}/${source}")
      message(FATAL_ERROR "${case}: no such file in the patched tree: ${source}")
    endif()
    list(APPEND sources "${KODI_SOURCE}/${source}")
  endforeach()

  add_executable(${target} ${sources})
  set_target_properties(${target} PROPERTIES
    CXX_STANDARD ${TEST_STANDARD} CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  # A suite's own include directories come first so a shim can stand in for a
  # header the tree has - that is the only reason to pass one. The tree itself
  # is a system include: upstream's headers are no more ours to make
  # warning-clean than upstream's translation units are, and -Werror on our own
  # code should answer for our own code.
  target_include_directories(${target} PRIVATE ${TEST_INCLUDES})
  target_include_directories(${target} SYSTEM PRIVATE "${KODI_SOURCE}/xbmc")
  # FFmpeg declaration headers, staged from the pinned tarball by
  # prepare-tree.py so nothing depends on what the host has installed.
  if(EXISTS "${KODI_SOURCE}/.yacer-include")
    target_include_directories(${target} SYSTEM PRIVATE "${KODI_SOURCE}/.yacer-include")
  endif()

  target_link_libraries(${target} PRIVATE GTest::gmock_main)
  # fmt is staged as headers, so anything that reaches Kodi's string helpers
  # uses its header-only form rather than linking a built library.
  # TARGET_POSIX is what the tree compiles with; without it Kodi's own
  # CriticalSection declares nothing and half the headers stop parsing. It is a
  # platform define, not a stand-in.
  target_compile_definitions(${target} PRIVATE FMT_HEADER_ONLY TARGET_POSIX ${TEST_DEFINES})
  # Instrument the test target only: googletest is a separate target and its
  # coverage would say nothing about ours.
  if(YACER_COVERAGE)
    target_compile_options(${target} PRIVATE --coverage -O0 -g)
    target_link_options(${target} PRIVATE --coverage)
  endif()
  # Our own test code is held to the strict set. Upstream's translation units
  # are not ours to make warning-clean, and -Werror on them would block a
  # suite over a warning nobody here introduced.
  foreach(source ${TEST_SOURCES})
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/${source}" APPEND
                 PROPERTY COMPILE_OPTIONS -Wall -Wextra -Werror -pedantic)
  endforeach()
  foreach(source ${TEST_KODI_SOURCES})
    set_property(SOURCE "${KODI_SOURCE}/${source}" APPEND
                 PROPERTY COMPILE_OPTIONS -Wall -Wextra)
    # An ffmpeg source compiled in reaches its siblings and its generated
    # config by quote-include. Those directories go on that file's command
    # line only: libavutil carries a time.h, and putting it on the target's
    # include path shadows the system header for everything else.
    if(source MATCHES "^\\.yacer-src/")
      set_property(SOURCE "${KODI_SOURCE}/${source}" APPEND PROPERTY COMPILE_OPTIONS
                   "-I${KODI_SOURCE}/.yacer-src"
                   "-I${KODI_SOURCE}/.yacer-src/libavutil"
                   "-I${KODI_SOURCE}/.yacer-include/libavutil")
    endif()
  endforeach()

  # Register each gtest case as its own ctest test, so the report names the
  # case that failed rather than the binary that contained it.
  gtest_discover_tests(${target}
    TEST_PREFIX "${YACER_GROUP}."
    PROPERTIES LABELS "${YACER_GROUP}" TIMEOUT ${YACER_TEST_TIMEOUT}
    DISCOVERY_MODE PRE_TEST)
endfunction()

# Every group that ships tests. A group is a directory, so adding one is making
# a directory - the same rule the patches follow.
file(GLOB test_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/../patches-yacer/*/*_tests")
list(SORT test_dirs)
set(found 0)
foreach(dir ${test_dirs})
  get_filename_component(leaf "${dir}" NAME)
  get_filename_component(parent "${dir}" DIRECTORY)
  get_filename_component(YACER_GROUP "${parent}" NAME)
  if(NOT EXISTS "${dir}/CMakeLists.txt")
    # Skipping it quietly would drop a whole group's tests from the run and
    # still report success.
    file(GLOB stray "${dir}/*.cpp" "${dir}/*.h")
    if(stray)
      message(FATAL_ERROR "${YACER_GROUP}/${leaf} has test sources but no CMakeLists.txt")
    endif()
    continue()
  endif()
  add_subdirectory("${dir}" "${CMAKE_BINARY_DIR}/groups/${YACER_GROUP}/${leaf}")
  math(EXPR found "${found} + 1")
endforeach()
# Behaviour we require of the tree, whoever provides it. A suite moves here
# when the patch it covered is merged upstream: the patch goes, the guarantee
# does not, and a later upstream change that regresses it must still turn this
# red. Suites here are also where we pin an upstream contract our own patches
# depend on.
file(GLOB guard_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/guards/*")
list(SORT guard_dirs)
foreach(dir ${guard_dirs})
  if(IS_DIRECTORY "${dir}" AND EXISTS "${dir}/CMakeLists.txt")
    get_filename_component(leaf "${dir}" NAME)
    set(YACER_GROUP "guard-${leaf}")
    add_subdirectory("${dir}" "${CMAKE_BINARY_DIR}/guards/${leaf}")
    math(EXPR found "${found} + 1")
  endif()
endforeach()

if(found EQUAL 0)
  message(FATAL_ERROR "no patch group ships tests; expected patches-yacer/<group>/<pkg>_tests")
endif()
message(STATUS "yacer tests: ${found} group(s) against ${KODI_SOURCE}")
