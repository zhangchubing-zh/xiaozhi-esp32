cmake_minimum_required(VERSION 3.16)
project(LiteCrab VERSION 1.0.0 LANGUAGES C)

execute_process(
  COMMAND git rev-parse --short=12 HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE LITECRAB_GIT_REVISION
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT LITECRAB_GIT_REVISION)
  set(LITECRAB_GIT_REVISION "unknown")
endif()
string(TIMESTAMP LITECRAB_BUILD_TIME_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
option(LITECRAB_BUILD_TESTS "Build LiteCrab tests" ON)
option(LITECRAB_ENABLE_STATIC_LINK "Link executables statically when the toolchain supports it" OFF)

if(WIN32 AND NOT CYGWIN AND NOT MSYS)
  message(FATAL_ERROR "LiteCrab uses POSIX sockets and pthreads. Build it inside WSL or another POSIX environment.")
endif()

find_package(Threads REQUIRED)

if(LITECRAB_ENABLE_STATIC_LINK)
  set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()
find_package(OpenSSL REQUIRED)

find_package(CURL REQUIRED)
if(LITECRAB_ENABLE_STATIC_LINK)
  find_package(ZLIB REQUIRED)
endif()

# libopus (decode-only) for the xiaozhi websocket gateway ASR path. Optional:
# when absent the gateway still builds and ASR falls back to stub text at
# runtime. Point LITECRAB_OPUS_PREFIX at a extracted prefix, e.g. the one
# created by scripts/fetch_opus_deb.sh.
set(LITECRAB_OPUS_PREFIX "$ENV{LITECRAB_OPUS_PREFIX}" CACHE PATH "Optional libopus install prefix")
find_path(OPUS_INCLUDE_DIR opus/opus.h
  HINTS ${LITECRAB_OPUS_PREFIX}/include /usr/include /usr/local/include)
# Prefer the static archive so the server binary stays self-contained
# (an out-of-tree shared libopus would embed an RPATH that breaks the
# deployment contract of a single relocatable binary).
set(LITECRAB_OPUS_SUFFIX_SAVE ${CMAKE_FIND_LIBRARY_SUFFIXES})
set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
find_library(OPUS_STATIC_LIBRARY NAMES opus
  HINTS ${LITECRAB_OPUS_PREFIX}/lib ${LITECRAB_OPUS_PREFIX}/lib/x86_64-linux-gnu
        /usr/lib /usr/local/lib)
set(CMAKE_FIND_LIBRARY_SUFFIXES ${LITECRAB_OPUS_SUFFIX_SAVE})
if(OPUS_STATIC_LIBRARY)
  set(OPUS_LIBRARY ${OPUS_STATIC_LIBRARY})
else()
  find_library(OPUS_LIBRARY NAMES opus
    HINTS ${LITECRAB_OPUS_PREFIX}/lib ${LITECRAB_OPUS_PREFIX}/lib/x86_64-linux-gnu
          /usr/lib /usr/local/lib)
endif()
if(OPUS_INCLUDE_DIR AND OPUS_LIBRARY)
  message(STATUS "libopus found: ${OPUS_LIBRARY}")
  set(LITECRAB_OPUS_FOUND ON)
else()
  message(WARNING "libopus not found: xiaozhi gateway ASR falls back to stub text (scripts/fetch_opus_deb.sh)")
  set(LITECRAB_OPUS_FOUND OFF)
endif()

add_library(litecrab
  src/util/json.c
  src/config/config.c
  src/runtime/runtime.c
  src/runtime/builtin_tools.c
  src/hub/hub.c
  src/session/session.c
  src/kernel/kernel.c
  src/kernel/llm.c
  src/kernel/skill.c
  src/kernel/working_memory.c
  src/observability/observability.c
  src/gateway/tcp_line.c
  src/gateway/ws_codec.c
  src/gateway/asr_sf.c
  src/gateway/tts_sf.c
  src/gateway/xiaozhi_ws.c
)
target_include_directories(litecrab PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(litecrab PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto m)
if(LITECRAB_OPUS_FOUND)
  target_include_directories(litecrab PRIVATE ${OPUS_INCLUDE_DIR})
  target_link_libraries(litecrab PUBLIC ${OPUS_LIBRARY} m)
  target_compile_definitions(litecrab PRIVATE LITECRAB_HAVE_OPUS=1)
endif()
target_compile_definitions(litecrab PRIVATE _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700 _GNU_SOURCE)
target_compile_options(litecrab PRIVATE -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration)

add_library(litecrab_alarm_core STATIC
  src/alarm/alarm_service.c
  src/alarm/alarm_notify.c
  src/alarm/polling/polling.c
  src/alarm/subscription/subscription.c
)
target_include_directories(litecrab_alarm_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/alarm
)
target_link_libraries(litecrab_alarm_core PUBLIC litecrab)
target_compile_definitions(litecrab_alarm_core PRIVATE _GNU_SOURCE)
target_compile_options(litecrab_alarm_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(litecrab_alarm_curl STATIC src/alarm/asp_client.c)
target_include_directories(litecrab_alarm_curl PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/alarm
)
target_link_libraries(litecrab_alarm_curl PUBLIC litecrab_alarm_core CURL::libcurl)
target_compile_definitions(litecrab_alarm_curl PRIVATE _GNU_SOURCE)
target_compile_options(litecrab_alarm_curl PRIVATE -Wall -Wextra -Wpedantic)

add_executable(litecrab_server src/main.c src/alarm/alarm_app.c)
target_link_libraries(litecrab_server PRIVATE litecrab litecrab_alarm_curl)
target_compile_definitions(litecrab_server PRIVATE
  LITECRAB_BUILD_REVISION="${LITECRAB_GIT_REVISION}"
  LITECRAB_BUILD_TIME_UTC="${LITECRAB_BUILD_TIME_UTC}")
if(LITECRAB_ENABLE_STATIC_LINK)
  # Static libcurl's package metadata in the SD5091 sysroot does not carry
  # all transitive libraries. Keep its dependencies after libcurl so the
  # one-pass GNU linker can resolve them.
  target_link_libraries(litecrab_server PRIVATE
    CURL::libcurl ZLIB::ZLIB OpenSSL::SSL OpenSSL::Crypto ${CMAKE_DL_LIBS} Threads::Threads)
endif()
if(LITECRAB_ENABLE_STATIC_LINK AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_link_options(litecrab_server PRIVATE -static)
endif()

include(CTest)
if(BUILD_TESTING AND LITECRAB_BUILD_TESTS)
  add_executable(litecrab_tests tests/test_main.c)
  target_link_libraries(litecrab_tests PRIVATE litecrab)
  add_test(NAME litecrab_tests COMMAND litecrab_tests)
  add_executable(litecrab_ws_tests tests/test_ws.c)
  target_link_libraries(litecrab_ws_tests PRIVATE litecrab)
  target_compile_options(litecrab_ws_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME litecrab_ws_tests COMMAND litecrab_ws_tests)
  if(LITECRAB_OPUS_FOUND)
    add_executable(litecrab_opusenc tests/opusenc_tool.c)
    target_link_libraries(litecrab_opusenc PRIVATE ${OPUS_LIBRARY} m)
    target_include_directories(litecrab_opusenc PRIVATE ${OPUS_INCLUDE_DIR})
    target_compile_options(litecrab_opusenc PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_executable(litecrab_security_tests tests/test_security.c)
  target_link_libraries(litecrab_security_tests PRIVATE litecrab)
  target_compile_options(litecrab_security_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME litecrab_security_tests COMMAND litecrab_security_tests)
  add_executable(litecrab_alarm_tests tests/test_alarm.c)
  target_link_libraries(litecrab_alarm_tests PRIVATE litecrab_alarm_core)
  target_compile_options(litecrab_alarm_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME litecrab_alarm_tests COMMAND litecrab_alarm_tests)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME litecrab_tcp_e2e
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tcp_e2e.py $<TARGET_FILE:litecrab_server> ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(litecrab_tcp_e2e PROPERTIES TIMEOUT 20)
    add_test(NAME litecrab_transfer_station
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_transfer_station.py)
    set_tests_properties(litecrab_transfer_station PROPERTIES TIMEOUT 20)
    add_test(NAME litecrab_agent_matrix
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_agent_matrix.py $<TARGET_FILE:litecrab_server> ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(litecrab_agent_matrix PROPERTIES TIMEOUT 45)
    add_test(NAME litecrab_resolver_modes
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_resolver_modes.py $<TARGET_FILE:litecrab_server> ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(litecrab_resolver_modes PROPERTIES TIMEOUT 30)
    add_test(NAME litecrab_session_restart
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_session_restart.py $<TARGET_FILE:litecrab_server>)
    set_tests_properties(litecrab_session_restart PROPERTIES TIMEOUT 30)
    add_test(NAME litecrab_ui_session_transport
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ui_session_transport.py ${CMAKE_CURRENT_SOURCE_DIR}/others/UI/app.py)
    set_tests_properties(litecrab_ui_session_transport PROPERTIES TIMEOUT 10)
    add_test(NAME litecrab_security_e2e
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_security_e2e.py $<TARGET_FILE:litecrab_server>)
    set_tests_properties(litecrab_security_e2e PROPERTIES TIMEOUT 30)
    add_test(NAME litecrab_alarm_e2e
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_alarm_e2e.py $<TARGET_FILE:litecrab_server> ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(litecrab_alarm_e2e PROPERTIES TIMEOUT 90)
    add_test(NAME litecrab_alarm_subscription_e2e
      COMMAND ${CMAKE_COMMAND} -E env ALARM_TEST_MODE=subscription ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_alarm_e2e.py $<TARGET_FILE:litecrab_server> ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(litecrab_alarm_subscription_e2e PROPERTIES TIMEOUT 60)
    add_test(NAME litecrab_long_running_stress
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_long_running_stress.py $<TARGET_FILE:litecrab_server> ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(litecrab_long_running_stress PROPERTIES TIMEOUT 90)
    add_test(NAME litecrab_deployment_contract
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_deployment_contract.py
              ${CMAKE_COMMAND} ${CMAKE_BINARY_DIR} $<TARGET_FILE:litecrab_server>)
    set_tests_properties(litecrab_deployment_contract PROPERTIES TIMEOUT 30)
  endif()
endif()

# The SD5091 formal artifact is one executable. Configuration and Skills are
# provisioned independently on the board and are intentionally not installed.
install(TARGETS litecrab_server RUNTIME DESTINATION bin)
