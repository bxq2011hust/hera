if(ProjectWasmerIncluded)
    return()
endif()
set(ProjectWasmerIncluded TRUE)
include(ExternalProject)
include(GNUInstallDirs)

# TODO: if find LLVM 8.0+ use make capi-llvm to get better permformance
set(ENGINE "jit")
set(BACKEND "cranelift")
if(HERA_WASMER_NATIVE_ENGINE)
    set(ENGINE "native")
endif()
if(HERA_WASMER_LLVM_BACKEND)
    set(BACKEND "llvm")
endif()
set(WASMER_BUILD_COMMAND COMMAND make build-capi-${BACKEND}-${ENGINE})
# set(WASMER_BUILD_COMMAND cargo clean COMMAND cargo update COMMAND make build-capi)
ExternalProject_Add(wasmer
        PREFIX ${CMAKE_SOURCE_DIR}/deps
        DOWNLOAD_NO_PROGRESS 1
        GIT_REPOSITORY https://github.com/wasmerio/wasmer.git
        GIT_SHALLOW true
        GIT_TAG 8b2c3852b66e4eaf0267762632aeb78c42632449
        BUILD_IN_SOURCE 1
        PATCH_COMMAND COMMAND git checkout Cargo.lock
        CONFIGURE_COMMAND COMMAND git checkout Cargo.toml COMMAND echo "[profile.release]" >> Cargo.toml COMMAND echo "lto=false" >> Cargo.toml
        BUILD_COMMAND ${WASMER_BUILD_COMMAND}
        INSTALL_COMMAND COMMAND git checkout Cargo.toml
        # must not log configure
        LOG_CONFIGURE 0
        LOG_BUILD 1
        LOG_INSTALL 1
        BUILD_BYPRODUCTS <SOURCE_DIR>/target/release/libwasmer_c_api.a
)

ExternalProject_Get_Property(wasmer SOURCE_DIR)
set(WASMER_INCLUDE_DIRS ${SOURCE_DIR}/lib/)
file(MAKE_DIRECTORY ${WASMER_INCLUDE_DIRS})  # Must exist.
add_library(WASMER::runtim STATIC IMPORTED)
set(WASMER_RUNTIME_LIBRARIES ${SOURCE_DIR}/target/release/libwasmer_c_api.a)
set_property(TARGET WASMER::runtim PROPERTY IMPORTED_LOCATION ${WASMER_RUNTIME_LIBRARIES})
set_property(TARGET WASMER::runtim PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${WASMER_INCLUDE_DIRS})
install(FILES ${WASMER_RUNTIME_LIBRARIES} DESTINATION ${CMAKE_INSTALL_LIBDIR})
add_dependencies(WASMER::runtim wasmer)
