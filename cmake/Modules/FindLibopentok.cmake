# Once done these will be defined:
#
#  LIBOPENTOK_FOUND
#  LIBOPENTOK_INCLUDE_DIRS
#  LIBOPENTOK_LIBRARIES

find_package(PkgConfig QUIET)
if (PKG_CONFIG_FOUND)
	pkg_check_modules(_OPENTOK QUIET libopentok)
endif()

find_path(OPENTOK_INCLUDE_DIR
	NAMES opentok.h
	HINTS
		${_OPENTOK_INCLUDE_DIRS}
	PATHS
		/usr/include /usr/local/include /opt/local/include)

find_library(OPENTOK_LIB
	NAMES opentok
	HINTS
		${_OPENTOK_LIBRARY_DIRS}
	PATHS
		/usr/lib /usr/local/lib /opt/local/lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libopentok DEFAULT_MSG OPENTOK_LIB OPENTOK_INCLUDE_DIR)
mark_as_advanced(OPENTOK_INCLUDE_DIR OPENTOK_LIB)

if(LIBOPENTOK_FOUND)
	set(LIBOPENTOK_INCLUDE_DIRS ${OPENTOK_INCLUDE_DIR})
	set(LIBOPENTOK_LIBRARIES ${OPENTOK_LIB})
endif()
