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

# Try to find Boost 1.89+ first (system is header-only, no need to link)
# Fall back to older Boost with system component if not found
find_package(Boost 1.89 QUIET COMPONENTS filesystem program_options)
if(NOT Boost_FOUND)
    find_package(Boost 1.66 REQUIRED COMPONENTS system filesystem program_options)
    set(BOOST_HAS_SYSTEM TRUE)
else()
    set(BOOST_HAS_SYSTEM FALSE)
endif()

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
	add_definitions(-D_USE_MATH_DEFINES -DWIN32_LEAN_AND_MEAN -DNOGDI -D__restrict__=__restrict)
  	set(THREAD_LIB "")
else()
	find_package(SQLite3 REQUIRED)
  	set(THREAD_LIB pthread)
endif()

file(GLOB tilemaker_src_files
	src/attribute_store.cpp
	src/coordinates.cpp
	src/coordinates_geom.cpp
	src/external/streamvbyte_decode.c
	src/external/streamvbyte_encode.c
	src/external/streamvbyte_zigzag.c
	src/external/libdeflate/lib/adler32.c
	src/external/libdeflate/lib/arm/cpu_features.c
	src/external/libdeflate/lib/crc32.c
	src/external/libdeflate/lib/deflate_compress.c
	src/external/libdeflate/lib/deflate_decompress.c
	src/external/libdeflate/lib/gzip_compress.c
	src/external/libdeflate/lib/gzip_decompress.c
	src/external/libdeflate/lib/utils.c
	src/external/libdeflate/lib/x86/cpu_features.c
	src/external/libdeflate/lib/zlib_compress.c
	src/external/libdeflate/lib/zlib_decompress.c
	src/geojson_processor.cpp
	src/geom.cpp
	src/helpers.cpp
	src/mbtiles.cpp
	src/mmap_allocator.cpp
	src/node_stores.cpp
	src/options_parser.cpp
	src/osm_lua_processing.cpp
	src/osm_mem_tiles.cpp
	src/osm_store.cpp
	src/output_object.cpp
	src/pbf_processor.cpp
	src/pbf_reader.cpp
	src/pmtiles.cpp
	src/pooled_string.cpp
	src/relation_roles.cpp
	src/sharded_node_store.cpp
	src/sharded_way_store.cpp
	src/shared_data.cpp
	src/shp_mem_tiles.cpp
	src/shp_processor.cpp
	src/significant_tags.cpp
	src/sorted_node_store.cpp
	src/sorted_way_store.cpp
	src/tag_map.cpp
	src/tile_coordinates_set.cpp
	src/tile_data.cpp
	src/tile_sorting.cpp
	src/tilemaker.cpp
	src/tile_worker.cpp
	src/visvalingam.cpp
	src/way_stores.cpp
  )
add_executable(tilemaker ${tilemaker_src_files})
target_include_directories(tilemaker PRIVATE include)
target_include_directories(tilemaker PRIVATE ${CMAKE_BINARY_DIR}) # for generated files
target_link_libraries(tilemaker
		${THREAD_LIB} ${CMAKE_DL_LIBS}
		${LUA_LIBRARIES}
		shapelib::shp
		SQLite::SQLite3
		Rapidjson::rapidjson
		Boost::filesystem Boost::program_options)
if(BOOST_HAS_SYSTEM)
    target_link_libraries(tilemaker Boost::system)
endif()

include(CheckCxxAtomic)
if(NOT HAVE_CXX11_ATOMIC)
	string(APPEND CMAKE_CXX_STANDARD_LIBRARIES
	" ${LIBATOMIC_LINK_FLAGS}")
endif()

install(FILES docs/man/tilemaker.1 DESTINATION share/man/man1)

add_executable(tilemaker-server server/server.cpp)
target_include_directories(tilemaker-server PRIVATE include)
target_link_libraries(tilemaker-server ${THREAD_LIB} ${CMAKE_DL_LIBS} SQLite::SQLite3 Boost::filesystem Boost::program_options)

install(TARGETS tilemaker tilemaker-server RUNTIME DESTINATION bin)
