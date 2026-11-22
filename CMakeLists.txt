cmake_minimum_required(VERSION 3.20)

project(hyd VERSION 0.7.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SIMDJSON REQUIRED IMPORTED_TARGET simdjson)

add_library(hydcore STATIC
    src/common/log.cpp
    src/common/util.cpp
    src/common/config.cpp
    src/hydrus/http.cpp
    src/hydrus/downloader.cpp
    src/hydrus/repository.cpp
    src/hydrus/update.cpp
    src/stats.cpp
    src/store/store.cpp
    src/store/delta.cpp
    src/store/builder.cpp
    src/api/server.cpp
    src/api/handlers.cpp
)

target_include_directories(hydcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(hydcore PUBLIC
    CURL::libcurl
    OpenSSL::Crypto
    ZLIB::ZLIB
    nlohmann_json::nlohmann_json
    PkgConfig::SIMDJSON
)

target_compile_options(hydcore PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
)

add_executable(hydd src/main.cpp)
target_link_libraries(hydd PRIVATE hydcore)
target_compile_definitions(hydd PRIVATE HYDD_VERSION="${PROJECT_VERSION}")
target_compile_options(hydd PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)

include(GNUInstallDirs)

install(TARGETS hydd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS contrib/hyd DESTINATION ${CMAKE_INSTALL_BINDIR})
# Linux daemons use /etc, not <prefix>/etc, even when the prefix is /usr.
install(FILES contrib/hydd.conf DESTINATION /etc RENAME hydd.conf
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ)
install(FILES contrib/systemd/hydd.service DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
install(FILES contrib/systemd/hyd.sysusers DESTINATION ${CMAKE_INSTALL_LIBDIR}/sysusers.d RENAME hyd.conf)
install(FILES contrib/systemd/hyd.tmpfiles DESTINATION ${CMAKE_INSTALL_LIBDIR}/tmpfiles.d RENAME hyd.conf)
install(FILES contrib/man/hydd.8 DESTINATION ${CMAKE_INSTALL_MANDIR}/man8)
install(FILES contrib/man/hydd.conf.5 DESTINATION ${CMAKE_INSTALL_MANDIR}/man5)
install(FILES contrib/completions/hydd.bash DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions RENAME hydd)
install(FILES contrib/completions/hydd.zsh DESTINATION ${CMAKE_INSTALL_DATADIR}/zsh/site-functions RENAME _hydd)
install(FILES contrib/completions/hydd.fish DESTINATION ${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d RENAME hydd.fish)
install(FILES contrib/completions/hyd.bash DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions RENAME hyd)
install(FILES contrib/completions/hyd.zsh DESTINATION ${CMAKE_INSTALL_DATADIR}/zsh/site-functions RENAME _hyd)
install(FILES contrib/completions/hyd.fish DESTINATION ${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d RENAME hyd.fish)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/hyd)

enable_testing()

add_executable(hyd_unit_tests tests/unit_tests.cpp)
target_link_libraries(hyd_unit_tests PRIVATE hydcore)
add_test(NAME unit COMMAND hyd_unit_tests)

find_program(BASH_PROGRAM bash)
if (BASH_PROGRAM)
    add_test(NAME integration
        COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.sh $<TARGET_FILE:hydd>)
    set_tests_properties(integration PROPERTIES TIMEOUT 120)

    add_test(NAME integration_future
        COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_future.sh $<TARGET_FILE:hydd>)
    set_tests_properties(integration_future PROPERTIES TIMEOUT 120)

endif()
