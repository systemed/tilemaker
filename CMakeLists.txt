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

find_package(Boost 1.66 REQUIRED COMPONENTS system filesystem program_options)
include_directories(SYSTEM ${Boost_INCLUDE_DIRS})

find_package(Protobuf REQUIRED)
include_directories(${PROTOBUF_INCLUDE_DIR})

find_package(SQLite3 REQUIRED)
include_directories(${SQLITE3_INCLUDE_DIR})

find_package(libshp REQUIRED)
include_directories(${LIBSHP_INCLUDE_DIR})

find_package(LuaJIT)
if(NOT LUAJIT_FOUND)
	find_package(Lua REQUIRED)
endif()
include_directories(${LUA_INCLUDE_DIR})

find_package(ZLIB REQUIRED)
include_directories(${ZLIB_INCLUDE_DIR})

set(CMAKE_CXX_STANDARD 11)

if(MSVC)
  add_definitions(-D_USE_MATH_DEFINES)
  set(THREAD_LIB "")
else()
  set(THREAD_LIB pthread)
endif()

ADD_CUSTOM_COMMAND(OUTPUT vector_tile.pb.cc vector_tile.pb.h
                   COMMAND ${PROTOBUF_PROTOC_EXECUTABLE}
		   ARGS --cpp_out ${CMAKE_BINARY_DIR} -I ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/include/vector_tile.proto)

ADD_CUSTOM_COMMAND(OUTPUT osmformat.pb.cc osmformat.pb.h
                   COMMAND ${PROTOBUF_PROTOC_EXECUTABLE}
		   ARGS --cpp_out ${CMAKE_BINARY_DIR} -I ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/include/osmformat.proto)

file(GLOB tilemaker_src_files
    "src/*.cpp"
    "clipper/*.cpp"
  )
add_executable(tilemaker vector_tile.pb.cc osmformat.pb.cc ${tilemaker_src_files})
target_link_libraries(tilemaker ${PROTOBUF_LIBRARY} ${LIBSHP_LIBRARIES} ${SQLITE3_LIBRARIES} ${LUAJIT_LIBRARIES} ${LUA_LIBRARIES} ${ZLIB_LIBRARY} ${THREAD_LIB} ${CMAKE_DL_LIBS}
	Boost::system Boost::filesystem Boost::program_options)

install(TARGETS tilemaker RUNTIME DESTINATION bin)
