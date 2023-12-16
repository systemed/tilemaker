cmake_minimum_required(VERSION 3.18)

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

set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)
find_package(Protobuf REQUIRED)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG OFF)

find_package(libshp REQUIRED)

find_package(Rapidjson REQUIRED)

find_package(Lua)

if(LUA_FOUND)
	include_directories(${LUA_INCLUDE_DIR})
else()
	find_package(LuaJIT REQUIRED)
	add_definitions(-DLUAJIT)
	include_directories(${LUAJIT_INCLUDE_DIR})
endif()

find_package(ZLIB REQUIRED)

set(CMAKE_CXX_STANDARD 17)

if(!TM_VERSION)
	execute_process(
		COMMAND git describe --tags --abbrev=0
		OUTPUT_VARIABLE tm_version)
	add_compile_definitions(TM_VERSION=${tm_version})
endif()

if(MSVC)
 	find_package(unofficial-sqlite3 CONFIG REQUIRED)
	add_library(SQLite::SQLite3 ALIAS unofficial::sqlite3::sqlite3)
	add_definitions(-D_USE_MATH_DEFINES -DWIN32_LEAN_AND_MEAN -DNOGDI)
  	set(THREAD_LIB "")
else()
	find_package(SQLite3 REQUIRED)
  	set(THREAD_LIB pthread)
endif()

if(NOT PROTOBUF_PROTOC_EXECUTABLE)
	set (PROTOBUF_PROTOC_EXECUTABLE "protobuf::protoc")
endif()


ADD_CUSTOM_COMMAND(OUTPUT vector_tile.pb.cc vector_tile.pb.h
                   COMMAND ${PROTOBUF_PROTOC_EXECUTABLE}
		   ARGS --cpp_out ${CMAKE_BINARY_DIR} -I ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/include/vector_tile.proto)

ADD_CUSTOM_COMMAND(OUTPUT osmformat.pb.cc osmformat.pb.h
                   COMMAND ${PROTOBUF_PROTOC_EXECUTABLE}
		   ARGS --cpp_out ${CMAKE_BINARY_DIR} -I ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/include/osmformat.proto)

file(GLOB tilemaker_src_files
	src/attribute_store.cpp
	src/coordinates.cpp
	src/coordinates_geom.cpp
	src/external/streamvbyte_decode.cc
	src/external/streamvbyte_encode.cc
	src/external/streamvbyte_zigzag.cc
	src/geom.cpp
	src/helpers.cpp
	src/mbtiles.cpp
	src/mmap_allocator.cpp
	src/node_stores.cpp
	src/osm_lua_processing.cpp
	src/osm_mem_tiles.cpp
	src/osm_store.cpp
	src/output_object.cpp
	src/pbf_blocks.cpp
	src/pmtiles.cpp
	src/read_pbf.cpp
	src/read_shp.cpp
	src/shared_data.cpp
	src/shp_mem_tiles.cpp
	src/sorted_node_store.cpp
	src/sorted_way_store.cpp
	src/tile_data.cpp
	src/tilemaker.cpp
	src/tile_worker.cpp
	src/way_stores.cpp
	src/write_geometry.cpp
  )
add_executable(tilemaker vector_tile.pb.cc osmformat.pb.cc ${tilemaker_src_files})
target_include_directories(tilemaker PRIVATE include)
target_include_directories(tilemaker PRIVATE ${CMAKE_BINARY_DIR}) # for generated files
target_link_libraries(tilemaker
		${THREAD_LIB} ${CMAKE_DL_LIBS}
		${LUA_LIBRARIES}
		protobuf::libprotobuf
		shapelib::shp
		SQLite::SQLite3
		ZLIB::ZLIB
		Rapidjson::rapidjson
		Boost::system Boost::filesystem Boost::program_options Boost::iostreams)

include(CheckCxxAtomic)
if(NOT HAVE_CXX11_ATOMIC)
	string(APPEND CMAKE_CXX_STANDARD_LIBRARIES
	" ${LIBATOMIC_LINK_FLAGS}")
endif()

install(FILES docs/man/tilemaker.1 DESTINATION share/man/man1)

install(TARGETS tilemaker RUNTIME DESTINATION bin)
