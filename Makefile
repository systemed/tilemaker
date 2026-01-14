# See what Lua versions are installed
# order of preference: LuaJIT, any generic Lua, then versions from 5.4 down

PLATFORM_PATH := /usr/local

# First, find what the Lua executable is called
# - when a new Lua is released, then add it before 5.4 here
LUA_CMD := $(shell luajit -e 'print("luajit")' 2> /dev/null || lua -e 'print("lua")' 2> /dev/null || lua5.4 -e 'print("lua5.4")' 2> /dev/null || lua5.3 -e 'print("lua5.3")' 2> /dev/null || lua5.2 -e 'print("lua5.2")' 2> /dev/null || lua5.1 -e 'print("lua5.1")' 2> /dev/null)
ifeq ($(LUA_CMD),"")
  $(error Couldn't find Lua interpreter)
endif
$(info Using ${LUA_CMD})

# Find the language version
LUA_LANGV := $(shell ${LUA_CMD} -e 'print(string.match(_VERSION, "%d+.%d+"))')
$(info - Lua language version ${LUA_LANGV})

# Find the directory where Lua might be
ifeq ($(LUA_CMD),luajit)
  # We need the LuaJIT version (2.0/2.1) to find this
  LUA_JITV := $(shell luajit -e 'a,b,c=string.find(jit.version,"LuaJIT (%d.%d)");print(c)')
  $(info - LuaJIT version ${LUA_JITV})
  LUA_DIR := luajit-${LUA_JITV}
  LUA_LIBS := -lluajit-${LUA_LANGV}
else
  LUA_DIR := $(LUA_CMD)
  LUA_LIBS := -l${LUA_CMD}
endif

# Find the include path by looking in the most likely locations
ifneq ('$(wildcard /usr/local/include/${LUA_DIR}/lua.h)','')
  LUA_CFLAGS := -I/usr/local/include/${LUA_DIR}
else ifneq ('$(wildcard /usr/local/include/${LUA_DIR}${LUA_LANGV}/lua.h)','')
  LUA_CFLAGS := -I/usr/local/include/${LUA_DIR}${LUA_LANGV}
  LUA_LIBS := -l${LUA_CMD}${LUA_LANGV}
else ifneq ('$(wildcard /usr/include/${LUA_DIR}/lua.h)','')
  LUA_CFLAGS := -I/usr/include/${LUA_DIR}
else ifneq ('$(wildcard /usr/include/${LUA_DIR}${LUA_LANGV}/lua.h)','')
  LUA_CFLAGS := -I/usr/include/${LUA_DIR}${LUA_LANGV}
  LUA_LIBS := -l${LUA_CMD}${LUA_LANGV}
else ifneq ('$(wildcard /usr/include/lua.h)','')
  LUA_CFLAGS := -I/usr/include
else ifneq ('$(wildcard /opt/homebrew/include/${LUA_DIR}/lua.h)','')
  LUA_CFLAGS := -I/opt/homebrew/include/${LUA_DIR}
  PLATFORM_PATH := /opt/homebrew
else ifneq ('$(wildcard /opt/homebrew/include/${LUA_DIR}${LUA_LANGV}/lua.h)','')
  LUA_CFLAGS := -I/opt/homebrew/include/${LUA_DIR}${LUA_LANGV}
  LUA_LIBS := -l${LUA_CMD}${LUA_LANGV}
  PLATFORM_PATH := /opt/homebrew
else
  $(error Couldn't find Lua libraries)
endif

# Append LuaJIT-specific flags if needed
ifeq ($(LUA_CMD),luajit)
  LUA_CFLAGS := ${LUA_CFLAGS} -DLUAJIT
  ifneq ($(OS),Windows_NT)
    ifeq ($(shell uname -s), Darwin)
      ifeq ($(LUA_JITV),2.0)
        LDFLAGS := -pagezero_size 10000 -image_base 100000000
        $(info - with MacOS LuaJIT linking)
      endif
    endif
  endif
endif

# Report success
$(info - include path is ${LUA_CFLAGS})
$(info - library path is ${LUA_LIBS})

# Main includes

prefix = /usr/local

MANPREFIX := /usr/share/man
TM_VERSION ?= $(shell git describe --tags --abbrev=0)
# Suppress warnings from third-party libraries:
# -Wno-missing-template-arg-list-after-template-kw: Boost.Interprocess compatibility with newer compilers
# -Wno-deprecated-declarations: RapidJSON uses deprecated std::iterator
CXXFLAGS ?= -O3 -Wall -Wno-unknown-pragmas -Wno-sign-compare -Wno-missing-template-arg-list-after-template-kw -Wno-deprecated-declarations -std=c++17 -pthread -fPIE -DTM_VERSION=$(TM_VERSION) $(CONFIG)
CFLAGS ?= -O3 -Wall -Wno-unknown-pragmas -Wno-sign-compare -std=c99 -fPIE -DTM_VERSION=$(TM_VERSION) $(CONFIG)
LIB := -L$(PLATFORM_PATH)/lib $(LUA_LIBS) -lboost_program_options -lsqlite3 -lboost_filesystem -lshp -pthread
INC := -I$(PLATFORM_PATH)/include -isystem ./include -I./src $(LUA_CFLAGS)

# Targets
.PHONY: test

all: tilemaker server

tilemaker: \
	src/attribute_store.o \
	src/coordinates_geom.o \
	src/coordinates.o \
	src/external/streamvbyte_decode.o \
	src/external/streamvbyte_encode.o \
	src/external/streamvbyte_zigzag.o \
	src/external/libdeflate/lib/adler32.o \
	src/external/libdeflate/lib/arm/cpu_features.o \
	src/external/libdeflate/lib/crc32.o \
	src/external/libdeflate/lib/deflate_compress.o \
	src/external/libdeflate/lib/deflate_decompress.o \
	src/external/libdeflate/lib/gzip_compress.o \
	src/external/libdeflate/lib/gzip_decompress.o \
	src/external/libdeflate/lib/utils.o \
	src/external/libdeflate/lib/x86/cpu_features.o \
	src/external/libdeflate/lib/zlib_compress.o \
	src/external/libdeflate/lib/zlib_decompress.o \
	src/geojson_processor.o \
	src/geom.o \
	src/helpers.o \
	src/mbtiles.o \
	src/mmap_allocator.o \
	src/node_stores.o \
	src/options_parser.o \
	src/osm_lua_processing.o \
	src/osm_mem_tiles.o \
	src/osm_store.o \
	src/output_object.o \
	src/pbf_processor.o \
	src/pbf_reader.o \
	src/pmtiles.o \
	src/pooled_string.o \
	src/relation_roles.o \
	src/sharded_node_store.o \
	src/sharded_way_store.o \
	src/shared_data.o \
	src/shp_mem_tiles.o \
	src/shp_processor.o \
	src/significant_tags.o \
	src/sorted_node_store.o \
	src/sorted_way_store.o \
	src/tag_map.o \
	src/tile_coordinates_set.o \
	src/tile_data.o \
	src/tile_sorting.o \
	src/tilemaker.o \
	src/tile_worker.o \
	src/visvalingam.o \
	src/way_stores.o
	$(CXX) $(CXXFLAGS) -o tilemaker $^ $(INC) $(LIB) $(LDFLAGS)

test: \
	test_append_vector \
	test_attribute_store \
	test_deque_map \
	test_helpers \
	test_options_parser \
	test_pbf_reader \
	test_pooled_string \
	test_relation_roles \
	test_significant_tags \
	test_sorted_node_store \
	test_sorted_way_store \
	test_tile_coordinates_set

test_append_vector: \
	src/mmap_allocator.o \
	test/append_vector.test.o
	$(CXX) $(CXXFLAGS) -o test.append_vector $^ $(INC) $(LIB) $(LDFLAGS) && ./test.append_vector

test_attribute_store: \
	src/mmap_allocator.o \
	src/attribute_store.o \
	src/pooled_string.o \
	test/attribute_store.test.o
	$(CXX) $(CXXFLAGS) -o test.attribute_store $^ $(INC) $(LIB) $(LDFLAGS) && ./test.attribute_store

test_deque_map: \
	test/deque_map.test.o
	$(CXX) $(CXXFLAGS) -o test.deque_map $^ $(INC) $(LIB) $(LDFLAGS) && ./test.deque_map

test_helpers: \
	src/helpers.o \
	src/external/libdeflate/lib/adler32.o \
	src/external/libdeflate/lib/arm/cpu_features.o \
	src/external/libdeflate/lib/crc32.o \
	src/external/libdeflate/lib/deflate_compress.o \
	src/external/libdeflate/lib/deflate_decompress.o \
	src/external/libdeflate/lib/gzip_compress.o \
	src/external/libdeflate/lib/gzip_decompress.o \
	src/external/libdeflate/lib/utils.o \
	src/external/libdeflate/lib/x86/cpu_features.o \
	src/external/libdeflate/lib/zlib_compress.o \
	src/external/libdeflate/lib/zlib_decompress.o \
	test/helpers.test.o
	$(CXX) $(CXXFLAGS) -o test.helpers $^ $(INC) $(LIB) $(LDFLAGS) && ./test.helpers

test_options_parser: \
	src/options_parser.o \
	test/options_parser.test.o
	$(CXX) $(CXXFLAGS) -o test.options_parser $^ $(INC) $(LIB) $(LDFLAGS) && ./test.options_parser

test_pooled_string: \
	src/mmap_allocator.o \
	src/pooled_string.o \
	test/pooled_string.test.o
	$(CXX) $(CXXFLAGS) -o test.pooled_string $^ $(INC) $(LIB) $(LDFLAGS) && ./test.pooled_string

test_relation_roles: \
	src/relation_roles.o \
	test/relation_roles.test.o
	$(CXX) $(CXXFLAGS) -o test.relation_roles $^ $(INC) $(LIB) $(LDFLAGS) && ./test.relation_roles

test_significant_tags: \
	src/significant_tags.o \
	src/tag_map.o \
	test/significant_tags.test.o
	$(CXX) $(CXXFLAGS) -o test.significant_tags $^ $(INC) $(LIB) $(LDFLAGS) && ./test.significant_tags

test_sorted_node_store: \
	src/external/streamvbyte_decode.o \
	src/external/streamvbyte_encode.o \
	src/external/streamvbyte_zigzag.o \
	src/mmap_allocator.o \
	src/sorted_node_store.o \
	test/sorted_node_store.test.o
	$(CXX) $(CXXFLAGS) -o test.sorted_node_store $^ $(INC) $(LIB) $(LDFLAGS) && ./test.sorted_node_store

test_sorted_way_store: \
	src/external/streamvbyte_decode.o \
	src/external/streamvbyte_encode.o \
	src/external/streamvbyte_zigzag.o \
	src/mmap_allocator.o \
	src/sorted_way_store.o \
	test/sorted_way_store.test.o
	$(CXX) $(CXXFLAGS) -o test.sorted_way_store $^ $(INC) $(LIB) $(LDFLAGS) && ./test.sorted_way_store

test_tile_coordinates_set: \
	src/tile_coordinates_set.o \
	test/tile_coordinates_set.test.o
	$(CXX) $(CXXFLAGS) -o test.tile_coordinates_set $^ $(INC) $(LIB) $(LDFLAGS) && ./test.tile_coordinates_set

test_pbf_reader: \
	src/helpers.o \
	src/pbf_reader.o \
	src/external/libdeflate/lib/adler32.o \
	src/external/libdeflate/lib/arm/cpu_features.o \
	src/external/libdeflate/lib/crc32.o \
	src/external/libdeflate/lib/deflate_compress.o \
	src/external/libdeflate/lib/deflate_decompress.o \
	src/external/libdeflate/lib/gzip_compress.o \
	src/external/libdeflate/lib/gzip_decompress.o \
	src/external/libdeflate/lib/utils.o \
	src/external/libdeflate/lib/x86/cpu_features.o \
	src/external/libdeflate/lib/zlib_compress.o \
	src/external/libdeflate/lib/zlib_decompress.o \
	test/pbf_reader.test.o
	$(CXX) $(CXXFLAGS) -o test.pbf_reader $^ $(INC) $(LIB) $(LDFLAGS) && ./test.pbf_reader

server: \
	server/server.o 
	$(CXX) $(CXXFLAGS) -o tilemaker-server $^ $(INC) $(LIB) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -o $@ -c $< $(INC)

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $< $(INC)

install:
	install -m 0755 -d $(DESTDIR)$(prefix)/bin/
	install -m 0755 tilemaker $(DESTDIR)$(prefix)/bin/
	install -m 0755 tilemaker-server $(DESTDIR)$(prefix)/bin/
	@install -m 0755 -d ${DESTDIR}${MANPREFIX}/man1/ || true
	@install docs/man/tilemaker.1 ${DESTDIR}${MANPREFIX}/man1/ || true

clean:
	rm -f tilemaker tilemaker-server src/*.o src/external/*.o src/external/libdeflate/lib/*.o src/external/libdeflate/lib/*/*.o include/*.o include/*.pb.h server/*.o test/*.o

.PHONY: install
