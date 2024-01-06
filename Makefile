# See what Lua versions are installed
# order of preference: LuaJIT 2.1, LuaJIT 2.0, any generic Lua, Lua 5.1

PLATFORM_PATH := /usr/local

ifneq ("$(wildcard /usr/local/include/luajit-2.1/lua.h)","")
  LUA_VER := LuaJIT 2.1
  LUA_CFLAGS := -I/usr/local/include/luajit-2.1 -DLUAJIT
  LUA_LIBS := -lluajit-5.1
  LUAJIT := 1

else ifneq ("$(wildcard /usr/include/luajit-2.1/lua.h)","")
  LUA_VER := LuaJIT 2.1
  LUA_CFLAGS := -I/usr/include/luajit-2.1 -DLUAJIT
  LUA_LIBS := -lluajit-5.1
  LUAJIT := 1

else ifneq ("$(wildcard /usr/local/include/luajit-2.0/lua.h)","")
  LUA_VER := LuaJIT 2.0
  LUA_CFLAGS := -I/usr/local/include/luajit-2.0 -DLUAJIT
  LUA_LIBS := -lluajit-5.1
  LUAJIT := 1

else ifneq ("$(wildcard /usr/include/luajit-2.0/lua.h)","")
  LUA_VER := LuaJIT 2.0
  LUA_CFLAGS := -I/usr/include/luajit-2.0 -DLUAJIT
  LUA_LIBS := -lluajit-5.1
  LUAJIT := 1

else ifneq ("$(wildcard /usr/local/include/lua/lua.h)","")
  LUA_VER := system Lua
  LUA_CFLAGS := -I/usr/local/include/lua
  LUA_LIBS := -llua

else ifneq ("$(wildcard /usr/include/lua/lua.h)","")
  LUA_VER := system Lua
  LUA_CFLAGS := -I/usr/include/lua
  LUA_LIBS := -llua

else ifneq ("$(wildcard /usr/include/lua.h)","")
  LUA_VER := system Lua
  LUA_CFLAGS := -I/usr/include
  LUA_LIBS := -llua

else ifneq ("$(wildcard /usr/local/include/lua5.1/lua.h)","")
  LUA_VER := Lua 5.1
  LUA_CFLAGS := -I/usr/local/include/lua5.1
  LUA_LIBS := -llua5.1

else ifneq ("$(wildcard /usr/include/lua5.1/lua.h)","")
  LUA_VER := Lua 5.1
  LUA_CFLAGS := -I/usr/include/lua5.1
  LUA_LIBS := -llua5.1

else ifneq ("$(wildcard /usr/include/lua5.3/lua.h)","")
  LUA_VER := Lua 5.3
  LUA_CFLAGS := -I/usr/include/lua5.3
  LUA_LIBS := -llua5.3

else ifneq ("$(wildcard /opt/homebrew/include/lua5.1/lua.h)","")
  LUA_VER := Lua 5.1
  LUA_CFLAGS := -I/opt/homebrew/include/lua5.1
  LUA_LIBS := -llua5.1
  PLATFORM_PATH := /opt/homebrew

else
  $(error Couldn't find Lua)
endif

$(info Using ${LUA_VER} (include path is ${LUA_CFLAGS}, library path is ${LUA_LIBS}))
ifneq ($(OS),Windows_NT)
  ifeq ($(shell uname -s), Darwin)
    ifeq ($(LUAJIT), 1)
      LDFLAGS := -pagezero_size 10000 -image_base 100000000
      $(info - with MacOS LuaJIT linking)
    endif
  endif
endif

# Main includes

prefix = /usr/local

MANPREFIX := /usr/share/man
TM_VERSION ?= $(shell git describe --tags --abbrev=0)
CXXFLAGS ?= -O3 -Wall -Wno-unknown-pragmas -Wno-sign-compare -std=c++14 -pthread -fPIE -DTM_VERSION=$(TM_VERSION) $(CONFIG)
CFLAGS ?= -O3 -Wall -Wno-unknown-pragmas -Wno-sign-compare -std=c99 -fPIE -DTM_VERSION=$(TM_VERSION) $(CONFIG)
LIB := -L$(PLATFORM_PATH)/lib -lz $(LUA_LIBS) -lboost_program_options -lsqlite3 -lboost_filesystem -lboost_system -lboost_iostreams -lshp -pthread
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
	src/sharded_node_store.o \
	src/sharded_way_store.o \
	src/shared_data.o \
	src/shp_mem_tiles.o \
	src/shp_processor.o \
	src/sorted_node_store.o \
	src/sorted_way_store.o \
	src/tile_data.o \
	src/tilemaker.o \
	src/tile_worker.o \
	src/way_stores.o
	$(CXX) $(CXXFLAGS) -o tilemaker $^ $(INC) $(LIB) $(LDFLAGS)

test: \
	test_append_vector \
	test_attribute_store \
	test_deque_map \
	test_pbf_reader \
	test_pooled_string \
	test_sorted_node_store \
	test_sorted_way_store

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

test_options_parser: \
	src/options_parser.o \
	test/options_parser.test.o
	$(CXX) $(CXXFLAGS) -o test.options_parser $^ $(INC) $(LIB) $(LDFLAGS) && ./test.options_parser

test_pooled_string: \
	src/mmap_allocator.o \
	src/pooled_string.o \
	test/pooled_string.test.o
	$(CXX) $(CXXFLAGS) -o test.pooled_string $^ $(INC) $(LIB) $(LDFLAGS) && ./test.pooled_string

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

test_pbf_reader: \
	src/helpers.o \
	src/pbf_reader.o \
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
	install -m 0755 -d ${DESTDIR}${MANPREFIX}/man1/
	install docs/man/tilemaker.1 ${DESTDIR}${MANPREFIX}/man1/

clean:
	rm -f tilemaker tilemaker-server src/*.o src/external/*.o include/*.o include/*.pb.h server/*.o test/*.o

.PHONY: install
