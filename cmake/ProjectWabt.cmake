if(ProjectWabtIncluded)
    return()
endif()
set(ProjectWabtIncluded TRUE)

include(ExternalProject)

set(prefix ${CMAKE_BINARY_DIR}/deps)
set(source_dir ${prefix}/src/wabt)
set(binary_dir ${prefix}/src/wabt-build)
set(include_dir ${source_dir})
set(wabt_library ${binary_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}wabt${CMAKE_STATIC_LIBRARY_SUFFIX})

ExternalProject_Add(wabt
    PREFIX ${prefix}
    DOWNLOAD_NAME wabt-1.0.8.tar.gz
    DOWNLOAD_DIR ${prefix}/downloads
    SOURCE_DIR ${source_dir}
    BINARY_DIR ${binary_dir}
    URL https://github.com/WebAssembly/wabt/archive/1.0.8.tar.gz
    URL_HASH SHA256=ffaad6de5cfbc5be0c7e78ccd4c0b44bbd1e59eaa38cf50f4245ca04dbda883e
    CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
    -DCMAKE_BUILD_TYPE=Release
    -DWITH_EXCEPTIONS=ON
    -DBUILD_TESTS=OFF
    -DBUILD_TOOLS=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${wabt_library}
)

add_library(wabt::wabt STATIC IMPORTED)

set_target_properties(
    wabt::wabt
    PROPERTIES
    IMPORTED_CONFIGURATIONS Release
    IMPORTED_LOCATION_RELEASE ${wabt_library}
    INTERFACE_INCLUDE_DIRECTORIES "${include_dir};${binary_dir}"
)

add_dependencies(wabt::wabt wabt)
