LUA_CFLAGS := -I/usr/local/include/lua5.1 -I/usr/include/lua5.1
LUA_LIBS := -llua5.1
CXXFLAGS := -O3 -Wall -Wno-unknown-pragmas -Wno-sign-compare -std=c++11 -pthread $(CONFIG)
LIB := -L/usr/local/lib -lz $(LUA_LIBS) -lboost_program_options -lsqlite3 -lboost_filesystem -lboost_system -lprotobuf -lshp
INC := -I/usr/local/include -isystem ./include -I./src $(LUA_CFLAGS)

all: tilemaker

tilemaker: include/osmformat.pb.o include/vector_tile.pb.o clipper/clipper.o src/mbtiles.o src/pbf_blocks.o src/coordinates.o src/osm_store.o src/helpers.o src/output_object.o src/read_shp.o src/read_pbf.o src/osm_object.o src/write_geometry.o src/shared_data.o src/tile_worker.o src/tilemaker.o
	$(CXX) $(CXXFLAGS) -o tilemaker $^ $(INC) $(LIB)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -o $@ -c $< $(INC)

%.o: %.cc
	$(CXX) $(CXXFLAGS) -o $@ -c $< $(INC)

%.pb.cc: %.proto
	protoc --proto_path=include --cpp_out=include $<

install:
	install -m 0755 tilemaker /usr/local/bin

clean:
	rm -f tilemaker src/*.o

.PHONY: install
