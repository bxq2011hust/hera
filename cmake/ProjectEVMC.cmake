include(ExternalProject)
include(GNUInstallDirs)

if(NOT EVMC_ROOT)
        ExternalProject_Add(evmc
                PREFIX ${CMAKE_SOURCE_DIR}/deps
                DOWNLOAD_NO_PROGRESS 1
                DOWNLOAD_NAME evmc-e0bd9d5d.tar.gz
                # URL https://github.com/FISCO-BCOS/evmc/archive/e0bd9d5dc68ec3a00fe9a3c5e81c98946449a20d.tar.gz
                # URL_HASH SHA256=a9983643415c8473cd696ff4aa93e014308f0292057bce740bc0b4ca84a40462
                GIT_REPOSITORY https://github.com/bxq2011hust/evmc.git
                GIT_TAG 8b6b585baf3c364f979e2029eb21c72465950112
                CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
                BUILD_IN_SOURCE 1
                LOG_CONFIGURE 1
                LOG_BUILD 1
                LOG_INSTALL 1
                BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libevmc-instructions.a <INSTALL_DIR>/lib/libevmc-loader.a
        )

        ExternalProject_Get_Property(evmc INSTALL_DIR)

        set(EVMC_INCLUDE_DIRS ${INSTALL_DIR}/include)
        file(MAKE_DIRECTORY ${EVMC_INCLUDE_DIRS})  # Must exist.
        set(EVMC_LOADER_LIBRARIES ${INSTALL_DIR}/lib/libevmc-loader.a)
        set(EVMC_INSTRUCTIONS_LIBRARIES ${INSTALL_DIR}/lib/libevmc-instructions.a)
else()
        set(EVMC_INCLUDE_DIRS ${EVMC_ROOT}/include)
        set(EVMC_LOADER_LIBRARIES ${EVMC_ROOT}/lib/libevmc-loader.a)
        set(EVMC_INSTRUCTIONS_LIBRARIES ${EVMC_ROOT}/lib/libevmc-instructions.a)
endif()

add_library(EVMC::Loader STATIC IMPORTED)
set_property(TARGET EVMC::Loader PROPERTY IMPORTED_LOCATION ${EVMC_LOADER_LIBRARIES})
set_property(TARGET EVMC::Loader PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${EVMC_INCLUDE_DIRS})

add_library(EVMC::Instructions STATIC IMPORTED)
set_property(TARGET EVMC::Instructions PROPERTY IMPORTED_LOCATION ${EVMC_INSTRUCTIONS_LIBRARIES})
set_property(TARGET EVMC::Instructions PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${EVMC_INCLUDE_DIRS})

add_library(EVMC INTERFACE IMPORTED)
set_property(TARGET EVMC PROPERTY INTERFACE_LINK_LIBRARIES ${EVMC_INSTRUCTIONS_LIBRARIES} ${EVMC_LOADER_LIBRARIES})
set_property(TARGET EVMC PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${EVMC_INCLUDE_DIRS})
add_dependencies(EVMC evmc)
