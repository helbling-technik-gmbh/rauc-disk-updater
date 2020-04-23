cmake_minimum_required(VERSION 3.0.2)
project( rauc-disk-updater )

option(BUILD_DOC    "Build documentation" OFF)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBGUDEV REQUIRED gudev-1.0>=233)
include_directories(${LIBGUDEV_INCLUDE_DIRS})

pkg_check_modules(GIO REQUIRED gio-2.0>=2.26.0)
include_directories(${GIO_INCLUDE_DIRS})

pkg_check_modules(GIOUNIX REQUIRED gio-unix-2.0>=2.26.0)
include_directories(${GIOUNIX_INCLUDE_DIRS})

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

if (BUILD_DOC)
	# check if Doxygen is installed
	find_package(Doxygen)
	if (DOXYGEN_FOUND)
	    # set input and output files
	    set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in)
	    set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)

	    # request to configure the file
	    configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)
	    message("Doxygen build started")

	    # note the option ALL which allows to build the docs together with the application
	    add_custom_target( doc_doxygen ALL
	        COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
	        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	        COMMENT "Generating documentation with Doxygen"
	        VERBATIM )
	else (DOXYGEN_FOUND)
	  	message("Doxygen need to be installed to generate the doxygen documentation")
	endif (DOXYGEN_FOUND)
endif (BUILD_DOC)

set(DISK_SRCS
  src/rauc-disk-updater.c
  src/udev.c
)

set(DBUS_RAUC_PREFIX de-pengutronix-rauc-gen)
set(DBUS_RAUC_COMMAND
    gdbus-codegen
      --generate-c-code ${DBUS_RAUC_PREFIX}
      --interface-prefix de.pengutronix.rauc.
		  --c-namespace Rauc
		  ${CMAKE_CURRENT_SOURCE_DIR}/src/de.pengutronix.rauc.xml
)


set(DBUS_DISK_PREFIX de-helbling-disk-updater-gen)
set(DBUS_DISK_COMMAND
    gdbus-codegen
      --generate-c-code ${DBUS_DISK_PREFIX}
      --interface-prefix de.helbling.
      ${CMAKE_CURRENT_SOURCE_DIR}/src/de.helbling.DiskUpdater.xml
)


add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/${DBUS_RAUC_PREFIX}.c
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/include/${DBUS_RAUC_PREFIX}.h
		COMMAND ${DBUS_RAUC_COMMAND}
		COMMAND mv ${DBUS_RAUC_PREFIX}.c ${CMAKE_CURRENT_SOURCE_DIR}/src/
		COMMAND mv ${DBUS_RAUC_PREFIX}.h ${CMAKE_CURRENT_SOURCE_DIR}/include/

    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/${DBUS_DISK_PREFIX}.c
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/include/${DBUS_DISK_PREFIX}.h
		COMMAND ${DBUS_DISK_COMMAND}
		COMMAND mv ${DBUS_DISK_PREFIX}.c ${CMAKE_CURRENT_SOURCE_DIR}/src/
		COMMAND mv ${DBUS_DISK_PREFIX}.h ${CMAKE_CURRENT_SOURCE_DIR}/include/
)


set(DISK_SRCS
    ${DISK_SRCS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/${DBUS_RAUC_PREFIX}.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/${DBUS_DISK_PREFIX}.c    
)


add_executable( rauc-disk-updater ${DISK_SRCS} )
set_target_properties(rauc-disk-updater PROPERTIES COMPILE_FLAGS "")

target_link_libraries(rauc-disk-updater
  LINK_PUBLIC
  ${GIO_LIBRARIES}
  ${LIBGUDEV_LIBRARIES}
)

install (TARGETS rauc-disk-updater DESTINATION /usr/bin/)

if (SYSTEMD_SYSTEM_UNITDIR)
  install (FILES ${CMAKE_SOURCE_DIR}/data/rauc-disk-updater.service
  	  DESTINATION ${SYSTEMD_SYSTEM_UNITDIR}/)
endif (SYSTEMD_SYSTEM_UNITDIR)

if (DBUS_POLICY_DIR)
  install (FILES ${CMAKE_SOURCE_DIR}/data/de.helbling.DiskUpdater.conf
  	  DESTINATION ${DBUS_POLICY_DIR}/)
endif (DBUS_POLICY_DIR)

if (DBUS_SYSTEM_UNITDIR)
  install (FILES ${CMAKE_SOURCE_DIR}/data/de.helbling.DiskUpdater.service
  	  DESTINATION ${DBUS_SYSTEM_UNITDIR}/)
endif (DBUS_SYSTEM_UNITDIR)

