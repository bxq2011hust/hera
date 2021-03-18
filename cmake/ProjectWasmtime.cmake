if(ProjectWasmtimeIncluded)
    return()
endif()
set(ProjectWasmtimeIncluded TRUE)
include(ExternalProject)
include(GNUInstallDirs)

set(WASMTIME_BUILD_COMMAND COMMAND cargo build --release -p wasmtime-c-api)
# set(WASMTIME_BUILD_COMMAND cargo clean COMMAND cargo update COMMAND make build-capi)
ExternalProject_Add(wasmtime
        PREFIX ${CMAKE_SOURCE_DIR}/deps
        DOWNLOAD_NO_PROGRESS 1
        GIT_REPOSITORY https://github.com/bytecodealliance/wasmtime.git
        GIT_SHALLOW true
        GIT_TAG 5fecdfa49150e3304c1b949aab73bd4a0a02dbac
        BUILD_IN_SOURCE 1
        PATCH_COMMAND COMMAND git checkout Cargo.lock
        CONFIGURE_COMMAND COMMAND git checkout Cargo.toml COMMAND echo "[profile.release]" >> Cargo.toml COMMAND echo "lto=false" >> Cargo.toml
        BUILD_COMMAND ${WASMTIME_BUILD_COMMAND}
        INSTALL_COMMAND COMMAND git checkout Cargo.toml
        # must not log configure
        LOG_CONFIGURE 0
        LOG_BUILD 1
        LOG_INSTALL 1
        BUILD_BYPRODUCTS <SOURCE_DIR>/target/release/libwasmtime.a
)

ExternalProject_Get_Property(wasmtime SOURCE_DIR)
set(WASMTIME_INCLUDE_DIRS ${SOURCE_DIR}/crates/c-api/include/ ${SOURCE_DIR}/crates/c-api/wasm-c-api/include/)
file(MAKE_DIRECTORY ${WASMTIME_INCLUDE_DIRS})  # Must exist.
add_library(WASMTIME::runtime STATIC IMPORTED)
set(WASMTIME_RUNTIME_LIBRARIES ${SOURCE_DIR}/target/release/libwasmtime.a)
set_property(TARGET WASMTIME::runtime PROPERTY IMPORTED_LOCATION ${WASMTIME_RUNTIME_LIBRARIES})
set_property(TARGET WASMTIME::runtime PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${WASMTIME_INCLUDE_DIRS})
install(FILES ${WASMTIME_RUNTIME_LIBRARIES} DESTINATION ${CMAKE_INSTALL_LIBDIR})
add_dependencies(WASMTIME::runtime WASMTIME)
