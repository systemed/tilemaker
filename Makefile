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
TM_BASE_VERSION := v$(shell sed -n '1p' VERSION)
TM_GIT_SHA := $(shell git rev-parse --short=12 HEAD 2>/dev/null)
TM_GIT_TAG := $(shell git describe --exact-match --tags HEAD 2>/dev/null)
TM_GIT_DIRTY := $(shell if git diff-index --quiet HEAD -- 2>/dev/null; then :; elif test $$? -eq 1; then printf '.dirty'; fi)
ifeq ($(TM_GIT_SHA),)
  TM_VERSION ?= $(TM_BASE_VERSION)+nogit
else ifeq ($(TM_GIT_TAG)$(TM_GIT_DIRTY),$(TM_BASE_VERSION))
  TM_VERSION ?= $(TM_BASE_VERSION)
else
  TM_VERSION ?= $(TM_BASE_VERSION)+g$(TM_GIT_SHA)$(TM_GIT_DIRTY)
endif
CXXFLAGS ?= -O3 -Wall -Wno-unknown-pragmas -Wno-sign-compare -std=c++14 -pthread -fPIE -DTM_VERSION=$(TM_VERSION) $(CONFIG)
CFLAGS ?= -O3 -Wall -Wno-unknown-pragmas -Wno-sign-compare -std=c99 -fPIE -DTM_VERSION=$(TM_VERSION) $(CONFIG)
DEPFLAGS := -MD -MP
DEPS := $(wildcard src/*.d src/external/*.d src/external/libdeflate/lib/*.d src/external/libdeflate/lib/*/*.d server/*.d test/*.d mlt-obj/*.d)
BOOST_SYSTEM_LIB := $(shell printf 'int main(){return 0;}\n' | $(CXX) -x c++ - -o /tmp/tilemaker-boost-system-check -lboost_system >/dev/null 2>&1 && echo -lboost_system; rm -f /tmp/tilemaker-boost-system-check)
LIB := -L$(PLATFORM_PATH)/lib -Wl,-rpath,$(PLATFORM_PATH)/lib $(LUA_LIBS) -lboost_program_options -lsqlite3 -lboost_filesystem $(BOOST_SYSTEM_LIB) -lshp -pthread
INC := -I$(PLATFORM_PATH)/include -isystem ./include -I./src $(LUA_CFLAGS)

# MLT (MapLibre Tiles) support - off by default, enable with `make MLT=1`
# Requires C++20 (GCC 11+/Clang 15+) and the maplibre-tile-spec submodule;
# run `make mlt-deps` once to fetch it.
MLT_CPP := maplibre-tile-spec/cpp
MLT_OBJ := mlt-obj
ifeq ($(MLT),1)
  ifneq ($(MAKECMDGOALS),mlt-deps)
    ifeq ($(wildcard $(MLT_CPP)/src/mlt/encoder.cpp),)
      $(error MLT=1 but the maplibre-tile-spec submodule is not populated - run `make mlt-deps` first)
    endif
  endif
  MLT_SHA := $(shell git -C maplibre-tile-spec rev-parse --short=12 HEAD 2>/dev/null)
  ifeq ($(MLT_SHA),)
    MLT_SHA := unknown
  endif
  MLT_INC := -isystem $(MLT_CPP)/include -isystem $(MLT_CPP)/src \
	-isystem $(MLT_CPP)/vendor/fsst -isystem $(MLT_CPP)/vendor/fastpfor \
	-isystem $(MLT_CPP)/vendor/earcut/include
  # Only the MLT sources and the bridge are built as C++20; tilemaker stays C++14
  MLT_CXXFLAGS := $(filter-out -std=c++14,$(CXXFLAGS)) -std=c++20
  MLT_OBJS := \
	$(MLT_OBJ)/encoder.o \
	$(MLT_OBJ)/int.o \
	$(MLT_OBJ)/stream.o \
	$(MLT_OBJ)/tileset.o \
	$(MLT_OBJ)/libfsst.o \
	$(MLT_OBJ)/fsst_avx512.o \
	$(MLT_OBJ)/bitpacking.o
  TILEMAKER_MLT_OBJS := src/mlt_writer.o $(MLT_OBJS)
else
  TILEMAKER_MLT_OBJS := src/mlt_writer_stub.o
endif

# Targets
.PHONY: test

all: tilemaker server

tilemaker: \
	src/attribute_store.o \
	src/config_validator.o \
	src/coordinates_geom.o \
	src/coordinates.o \
	src/declutter.o \
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
	src/simplify_buildings.o \
	src/sorted_node_store.o \
	src/sorted_way_store.o \
	src/tag_map.o \
	src/tile_coordinates_set.o \
	src/tile_data.o \
	src/tile_sorting.o \
	src/tilemaker.o \
	src/tile_worker.o \
	src/visvalingam.o \
	src/way_stores.o \
	$(TILEMAKER_MLT_OBJS)
	$(CXX) $(CXXFLAGS) -o tilemaker $^ $(INC) $(LIB) $(LDFLAGS)

test: \
	test_append_vector \
	test_attribute_store \
	test_config_validator \
	test_deque_map \
	test_helpers \
	test_options_parser \
	test_pbf_reader \
	test_pooled_string \
	test_relation_roles \
	test_significant_tags \
	test_sorted_node_store \
	test_sorted_way_store \
	test_osm_store \
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

test_config_validator: \
	src/config_validator.o \
	test/config_validator.test.o
	$(CXX) $(CXXFLAGS) -o test.config_validator $^ $(INC) $(LIB) $(LDFLAGS) && ./test.config_validator

src/config_schema.h: resources/config-schema.json
	printf '#ifndef _CONFIG_SCHEMA_H\n#define _CONFIG_SCHEMA_H\n\nstatic const char* CONFIG_SCHEMA = R"TMCONFIGSCHEMA(\n' > $@
	cat $< >> $@
	printf '\n)TMCONFIGSCHEMA";\n\n#endif //_CONFIG_SCHEMA_H\n' >> $@

src/config_validator.o: src/config_schema.h

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
	$(TILEMAKER_MLT_OBJS) \
	test/options_parser.test.o
	$(CXX) $(CXXFLAGS) -o test.options_parser $^ $(INC) $(LIB) $(LDFLAGS) && ./test.options_parser

test_osm_store: \
	test/osm_store.test.o
	$(CXX) $(CXXFLAGS) -o test.osm_store $^ $(INC) $(LIB) $(LDFLAGS) && ./test.osm_store

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
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -o $@ -c $< $(INC)

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -o $@ -c $< $(INC)

# The bridge is the only tilemaker source that sees MLT's headers, so it alone
# is built as C++20; everything it exposes to the rest of tilemaker is C++14.
src/mlt_writer.o: src/mlt_writer.cpp
	$(CXX) $(MLT_CXXFLAGS) $(DEPFLAGS) -DMLT_VERSION=$(MLT_SHA) -o $@ -c $< $(INC) $(MLT_INC)

$(MLT_OBJ):
	mkdir -p $@

$(MLT_OBJ)/%.o: $(MLT_CPP)/src/mlt/%.cpp | $(MLT_OBJ)
	$(CXX) $(MLT_CXXFLAGS) $(DEPFLAGS) -o $@ -c $< $(MLT_INC)

$(MLT_OBJ)/%.o: $(MLT_CPP)/src/mlt/encode/%.cpp | $(MLT_OBJ)
	$(CXX) $(MLT_CXXFLAGS) $(DEPFLAGS) -o $@ -c $< $(MLT_INC)

$(MLT_OBJ)/%.o: $(MLT_CPP)/src/mlt/metadata/%.cpp | $(MLT_OBJ)
	$(CXX) $(MLT_CXXFLAGS) $(DEPFLAGS) -o $@ -c $< $(MLT_INC)

$(MLT_OBJ)/%.o: $(MLT_CPP)/vendor/fastpfor/fastpfor/%.cpp | $(MLT_OBJ)
	$(CXX) $(MLT_CXXFLAGS) $(DEPFLAGS) -w -o $@ -c $< $(MLT_INC)

# fsst is C++17 and warns freely
$(MLT_OBJ)/%.o: $(MLT_CPP)/vendor/fsst/%.cpp | $(MLT_OBJ)
	$(CXX) $(filter-out -std=c++20,$(MLT_CXXFLAGS)) -std=c++17 $(DEPFLAGS) -w -o $@ -c $< $(MLT_INC)

-include $(DEPS)

install:
	install -m 0755 -d $(DESTDIR)$(prefix)/bin/
	install -m 0755 tilemaker $(DESTDIR)$(prefix)/bin/
	install -m 0755 tilemaker-server $(DESTDIR)$(prefix)/bin/
	@install -m 0755 -d ${DESTDIR}${MANPREFIX}/man1/ || true
	@install docs/man/tilemaker.1 ${DESTDIR}${MANPREFIX}/man1/ || true

# Fetch just enough of the MLT submodule to build the encoder: a partial,
# sparse checkout of cpp/ (~2MB rather than ~380MB for the full tree), plus the
# two nested vendor submodules the encoder needs. json, googletest and
# mvt-fixtures are only used by MLT's own decoder, tests and tools.
mlt-deps:
	git submodule update --init --depth 1 --filter=blob:none maplibre-tile-spec
	git -C maplibre-tile-spec sparse-checkout set cpp
	git -C maplibre-tile-spec submodule update --init --depth 1 \
		cpp/vendor/fsst cpp/vendor/earcut
	git -C maplibre-tile-spec/cpp/vendor/fsst sparse-checkout set --no-cone \
		'/*' '!/paper' '!/*.mp4' '!/*.pptx' '!/*.pdf'

clean:
	rm -rf $(MLT_OBJ)
	rm -f tilemaker tilemaker-server src/*.o src/external/*.o src/external/libdeflate/lib/*.o src/external/libdeflate/lib/*/*.o include/*.o include/*.pb.h server/*.o test/*.o src/config_schema.h
	rm -f src/*.d src/external/*.d src/external/libdeflate/lib/*.d src/external/libdeflate/lib/*/*.d server/*.d test/*.d

.PHONY: install mlt-deps
