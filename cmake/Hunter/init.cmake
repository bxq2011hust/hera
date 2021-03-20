
set(HUNTER_CONFIGURATION_TYPES Release
    CACHE STRING "Build type of the Hunter packages")

include(HunterGate)

HunterGate(
    URL "https://github.com/cpp-pm/hunter/archive/v0.23.296.tar.gz"
    SHA1 "232f5022ee1d45955a7e8d3e1720f31bac1bb534"
)
