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
LIB := -L$(PLATFORM_PATH)/lib -lz $(LUA_LIBS) -lboost_program_options -lsqlite3 -lboost_filesystem -lboost_system -lboost_iostreams -lprotobuf -lshp -pthread
INC := -I$(PLATFORM_PATH)/include -isystem ./include -I./src $(LUA_CFLAGS)

# Targets

all: tilemaker

tilemaker: include/osmformat.pb.o include/vector_tile.pb.o src/mbtiles.o src/pbf_blocks.o src/coordinates.o src/osm_store.o src/helpers.o src/output_object.o src/read_shp.o src/read_pbf.o src/osm_lua_processing.o src/write_geometry.o src/shared_data.o src/tile_worker.o src/tile_data.o src/osm_mem_tiles.o src/shp_mem_tiles.o src/attribute_store.o src/tilemaker.o src/geom.o
	$(CXX) $(CXXFLAGS) -o tilemaker $^ $(INC) $(LIB) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -o $@ -c $< $(INC)

%.o: %.cc
	$(CXX) $(CXXFLAGS) -o $@ -c $< $(INC)

%.pb.cc: %.proto
	protoc --proto_path=include --cpp_out=include $<

install:
	install -m 0755 -d $(DESTDIR)$(prefix)/bin/
	install -m 0755 tilemaker $(DESTDIR)$(prefix)/bin/
	install -d ${DESTDIR}${MANPREFIX}/man1/
	install docs/man/tilemaker.1 ${DESTDIR}${MANPREFIX}/man1/

clean:
	rm -f tilemaker src/*.o include/*.o

.PHONY: install
