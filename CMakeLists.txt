cmake_minimum_required(VERSION 3.0)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release")
endif()

project(tilemaker)

OPTION(TILEMAKER_BUILD_STATIC "Attempt to link dependencies static" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if("${PROJECT_SOURCE_DIR}" STREQUAL "${PROJECT_BINARY_DIR}")
  message(FATAL_ERROR "In-source builds are not allowed, please use a separate build directory.")
endif()

if (POLICY CMP0074)
  cmake_policy(SET CMP0074 NEW)
endif()

include_directories(include)
include_directories(${CMAKE_BINARY_DIR}) # for generated files

IF (TILEMAKER_BUILD_STATIC)
    MESSAGE (STATUS "Staticly linking with Boost")
    SET (Boost_USE_STATIC_LIBS TRUE)
	SET (CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_STATIC_LIBRARY_SUFFIX})
ELSE ()
    MESSAGE (STATUS "Dynamically linking with Boost")
    SET (Boost_USE_STATIC_LIBS FALSE)
ENDIF ()

IF ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
    LINK_DIRECTORIES(${Boost_LIBRARY_DIRS})
ENDIF ()

find_package(Boost 1.66 REQUIRED COMPONENTS system filesystem program_options iostreams)
include_directories(SYSTEM ${Boost_INCLUDE_DIRS})

find_package(Protobuf REQUIRED)
include_directories(${PROTOBUF_INCLUDE_DIR})

find_package(libshp REQUIRED)
include_directories(${LIBSHP_INCLUDE_DIR})

find_package(Lua)

if(LUA_FOUND)
	include_directories(${LUA_INCLUDE_DIR})
else()
	find_package(LuaJIT REQUIRED)
	add_definitions(-DLUAJIT)
	include_directories(${LUAJIT_INCLUDE_DIR})
endif()

find_package(ZLIB REQUIRED)
include_directories(${ZLIB_INCLUDE_DIR})

set(CMAKE_CXX_STANDARD 14)

execute_process(
	COMMAND git describe --tags --abbrev=0
	OUTPUT_VARIABLE tm_version)
add_compile_definitions(TM_VERSION=${tm_version})

if(MSVC)
 	find_package(unofficial-sqlite3 CONFIG REQUIRED)
	add_definitions(-D_USE_MATH_DEFINES -DWIN32_LEAN_AND_MEAN -DNOGDI)
  	set(THREAD_LIB "")
else()
	find_package(SQLite3 REQUIRED)
	include_directories(${SQLITE3_INCLUDE_DIRS})
  	set(THREAD_LIB pthread)
endif()

ADD_CUSTOM_COMMAND(OUTPUT vector_tile.pb.cc vector_tile.pb.h
                   COMMAND ${PROTOBUF_PROTOC_EXECUTABLE}
		   ARGS --cpp_out ${CMAKE_BINARY_DIR} -I ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/include/vector_tile.proto)

ADD_CUSTOM_COMMAND(OUTPUT osmformat.pb.cc osmformat.pb.h
                   COMMAND ${PROTOBUF_PROTOC_EXECUTABLE}
		   ARGS --cpp_out ${CMAKE_BINARY_DIR} -I ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/include/osmformat.proto)

file(GLOB tilemaker_src_files
	src/attribute_store.cpp
	src/geom.cpp
	src/mbtiles.cpp
	src/osm_mem_tiles.cpp
	src/output_object.cpp
	src/read_pbf.cpp
	src/shared_data.cpp
	src/tile_data.cpp
	src/tile_worker.cpp
	src/coordinates.cpp
	src/helpers.cpp
	src/osm_lua_processing.cpp
	src/osm_store.cpp
	src/pbf_blocks.cpp
	src/read_shp.cpp
	src/shp_mem_tiles.cpp
	src/tilemaker.cpp
	src/write_geometry.cpp
  )
add_executable(tilemaker vector_tile.pb.cc osmformat.pb.cc ${tilemaker_src_files})
target_link_libraries(tilemaker ${PROTOBUF_LIBRARY} ${LIBSHP_LIBRARIES} ${SQLITE3_LIBRARIES} ${LUAJIT_LIBRARY} ${LUA_LIBRARIES} ${ZLIB_LIBRARY} ${THREAD_LIB} ${CMAKE_DL_LIBS}
	Boost::system Boost::filesystem Boost::program_options Boost::iostreams)

if(MSVC)
    target_link_libraries(tilemaker unofficial::sqlite3::sqlite3)
endif()

install(TARGETS tilemaker RUNTIME DESTINATION bin)
