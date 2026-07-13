# FindSodium.cmake
# -----------------
# Find the libsodium cryptographic library.
#
# Imported target:
#   Sodium::Sodium
#
# Variables:
#   Sodium_FOUND        - True if libsodium was found
#   Sodium_INCLUDE_DIR  - Include directory
#   Sodium_LIBRARY      - Library to link against
#
# Hints (optional):
#   SODIUM_ROOT  - Root directory of a libsodium installation

find_path(Sodium_INCLUDE_DIR
    NAMES sodium.h
    HINTS
        ${SODIUM_ROOT}
        ENV SODIUM_ROOT
    PATH_SUFFIXES include
)

find_library(Sodium_LIBRARY
    NAMES sodium libsodium
    HINTS
        ${SODIUM_ROOT}
        ENV SODIUM_ROOT
    PATH_SUFFIXES lib lib64
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Sodium
    REQUIRED_VARS Sodium_LIBRARY Sodium_INCLUDE_DIR
)

if(Sodium_FOUND AND NOT TARGET Sodium::Sodium)
    add_library(Sodium::Sodium UNKNOWN IMPORTED)
    set_target_properties(Sodium::Sodium PROPERTIES
        IMPORTED_LOCATION "${Sodium_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${Sodium_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(Sodium_INCLUDE_DIR Sodium_LIBRARY)
