cmake_minimum_required ( VERSION 2.8 )

#set ( CMAKE_MACOSX_RPATH ${CMAKE_MACOSX_RPATH} ${CMAKE_INSTALL_PREFIX}/lib )
set ( CMAKE_INCLUDE_CURRENT_DIR ON )
set ( CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} "/usr/local/Qt5" )

find_package ( Boost COMPONENTS system filesystem REQUIRED )
find_package ( OpenCV REQUIRED )
find_package ( Qt5Widgets REQUIRED )
find_package ( Qt5Concurrent REQUIRED )
find_package ( Qt5OpenGL REQUIRED )

include_directories ( ${OpenCV_INCLUDE_DIRS} ${Boost_INCLUDE_DIR} ${NiS_INCLUDE_DIR} )

set ( CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib" )

add_executable ( NiSMapCreatorTool
	MapCreator.cpp )

target_link_libraries ( NiSMapCreatorTool
	NiSCore
	NiSHandler
	NiSSLAM
	NiSMapCreator
	${OpenCV_LIBS}
	${Boost_LIBRARIES}
	Qt5::Widgets
	Qt5::Concurrent
	Qt5::OpenGL )

add_executable ( NiSViewer Viewer.cpp )

add_executable ( NiSFrameViewer FrameViewer.cpp )
target_link_libraries ( NiSFrameViewer
	NiSHandler
	NiSSLAM
	NiSMapCreator
	${OpenCV_LIBS}
	${Boost_LIBRARIES}
	Qt5::Widgets
	Qt5::Concurrent
	Qt5::OpenGL )


install ( TARGETS
	NiSMapCreatorTool
	NiSViewer
	DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" )