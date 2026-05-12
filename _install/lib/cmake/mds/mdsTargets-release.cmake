#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "mds::mds_core" for configuration "Release"
set_property(TARGET mds::mds_core APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(mds::mds_core PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libmds_core.a"
  )

list(APPEND _cmake_import_check_targets mds::mds_core )
list(APPEND _cmake_import_check_files_for_mds::mds_core "${_IMPORT_PREFIX}/lib/libmds_core.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
