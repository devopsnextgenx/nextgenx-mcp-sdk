#----------------------------------------------------------------
# Generated CMake target import file.
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "cppmcp::core" for configuration ""
set_property(TARGET cppmcp::core APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(cppmcp::core PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_NOCONFIG "CXX"
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libcppmcp_core.a"
  )

list(APPEND _cmake_import_check_targets cppmcp::core )
list(APPEND _cmake_import_check_files_for_cppmcp::core "${_IMPORT_PREFIX}/lib/libcppmcp_core.a" )

# Import target "cppmcp::server" for configuration ""
set_property(TARGET cppmcp::server APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(cppmcp::server PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_NOCONFIG "CXX"
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libcppmcp_server.a"
  )

list(APPEND _cmake_import_check_targets cppmcp::server )
list(APPEND _cmake_import_check_files_for_cppmcp::server "${_IMPORT_PREFIX}/lib/libcppmcp_server.a" )

# Import target "cppmcp::client" for configuration ""
set_property(TARGET cppmcp::client APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(cppmcp::client PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_NOCONFIG "CXX"
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libcppmcp_client.a"
  )

list(APPEND _cmake_import_check_targets cppmcp::client )
list(APPEND _cmake_import_check_files_for_cppmcp::client "${_IMPORT_PREFIX}/lib/libcppmcp_client.a" )

# Import target "cppmcp::transport_stdio" for configuration ""
set_property(TARGET cppmcp::transport_stdio APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(cppmcp::transport_stdio PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_NOCONFIG "CXX"
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libcppmcp_transport_stdio.a"
  )

list(APPEND _cmake_import_check_targets cppmcp::transport_stdio )
list(APPEND _cmake_import_check_files_for_cppmcp::transport_stdio "${_IMPORT_PREFIX}/lib/libcppmcp_transport_stdio.a" )

# Import target "cppmcp::transport_http" for configuration ""
set_property(TARGET cppmcp::transport_http APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(cppmcp::transport_http PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_NOCONFIG "CXX"
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libcppmcp_transport_http.a"
  )

list(APPEND _cmake_import_check_targets cppmcp::transport_http )
list(APPEND _cmake_import_check_files_for_cppmcp::transport_http "${_IMPORT_PREFIX}/lib/libcppmcp_transport_http.a" )

# Import target "cppmcp::core_shared" for configuration ""
set_property(TARGET cppmcp::core_shared APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(cppmcp::core_shared PROPERTIES
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libcppmcp_core_shared.so"
  IMPORTED_SONAME_NOCONFIG "libcppmcp_core_shared.so"
  )

list(APPEND _cmake_import_check_targets cppmcp::core_shared )
list(APPEND _cmake_import_check_files_for_cppmcp::core_shared "${_IMPORT_PREFIX}/lib/libcppmcp_core_shared.so" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
