project(obs-bytertc)

find_package(PkgConfig)

FIND_PACKAGE(Libcurl)

message( STATUS "Looking for libwebrtc" )

# CoSMo libwebrtc package has versioning and COMPONENTS
find_package(LibWebRTC 95 QUIET COMPONENTS H264 )
if( NOT libwebrtc_FOUND )
	message( STATUS "No CoSMo package was found, searching for any webrtc package.")
	# A FindLibWebRTC package is also provided in the repository for self-compiled libwebrtc
	# this time we MUST find it, or fail, hence the REQUIRED
	find_package(LibWebRTC REQUIRED)
endif()
message( STATUS "Looking for libwebrtc - found" )

if(MSVC)
	add_compile_options("$<IF:$<CONFIG:Debug>,/MTd,/MT>")
endif()

set(obs-bytertc_HEADERS
		AudioDeviceModuleWrapper.h
		ByteWebRTCStream.h
		obsWebrtcAudioSource.h
		SDPModif.h
		VideoCapturer.h)
set(obs-bytertc_SOURCES
		AudioDeviceModuleWrapper.cpp
		bytertc-stream.cpp
		ByteWebRTCStream.cpp
		obs-bytertc.c
		obsWebrtcAudioSource.cpp
		VideoCapturer.cpp)

if(WIN32)
	set(MODULE_DESCRIPTION "OBS bytertc streaming module")
	configure_file(${CMAKE_SOURCE_DIR}/cmake/winrc/obs-module.rc.in obs-bytertc.rc)
	list(APPEND obs-bytertc_SOURCES
		obs-bytertc.rc)
endif()

add_library(obs-bytertc MODULE
	${obs-bytertc_SOURCES}
	${obs-bytertc_HEADERS})

target_include_directories(obs-bytertc
		PUBLIC  "${LIBWEBRTC_INCLUDE_DIRS}" "${LIBCURL_INCLUDE_DIRS}")

target_link_libraries(obs-bytertc
	libobs
	${obs-bytertc_PLATFORM_DEPS}
	${WEBRTC_LIBRARIES}
	${LIBCURL_LIBRARIES})

If (WIN32)
	Target_Compile_Definitions(${PROJECT_NAME}
			PRIVATE
			_HAS_ITERATOR_DEBUGGING=0
			_ITERATOR_DEBUG_LEVEL=0
			)
EndIf()

target_compile_features(obs-bytertc PRIVATE cxx_std_14)
set(_CXX_STANDARD 17)
set_target_properties(obs-bytertc PROPERTIES FOLDER "plugins" CXX_STANDARD 14)

install_obs_plugin_with_data(obs-bytertc data)