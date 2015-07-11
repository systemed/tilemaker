LUA_CFLAGS := -I/usr/local/include/lua5.1 -I/usr/include/lua5.1
LUA_LIBS := -llua5.1
CXXFLAGS := -g -O3 -Wall -Wno-unknown-pragmas -Wno-sign-compare -std=c++11
LIB := -L/usr/local/lib -lz $(LUA_LIBS) -lboost_program_options -lluabind -lsqlite3 -lboost_filesystem -lboost_system -lprotobuf
INC := -I/usr/local/include -I./include -I./src $(LUA_CFLAGS)

all: tilemaker

tilemaker: include/osmformat.pb.o include/vector_tile.pb.o src/tilemaker.o
	g++ $(CXXFLAGS) -o tilemaker $^ $(INC) $(LIB)

%.o: %.cpp
	g++ $(CXXFLAGS) -o $@ -c $< $(INC)

%.o: %.cc
	g++ $(CXXFLAGS) -o $@ -c $< $(INC)


%.pb.cc: %.proto
	protoc --proto_path=include --cpp_out=include $<

install:
	install -m 0755 tilemaker /usr/local/bin

clean:
	rm tilemaker

.PHONY: install
