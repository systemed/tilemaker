CXXFLAGS := -O3 -Wall -Wno-unknown-pragmas -Wno-sign-compare -std=c++11
LIB := -L/usr/local/lib -lz -llua5.1 -lboost_program_options -lluabind -lsqlite3 -lboost_filesystem -lboost_system -lprotobuf
INC := -I/usr/local/include -I./include -I./src -I/usr/local/include/lua5.1 -I/usr/include/lua5.1

all:
	protoc --proto_path=include --cpp_out=include include/osmformat.proto include/vector_tile.proto
	g++ $(CXXFLAGS) -o tilemaker include/osmformat.pb.cc include/vector_tile.pb.cc src/tilemaker.cpp $(INC) $(LIB)

install:
	install -m 0755 tilemaker /usr/local/bin

clean:
	rm tilemaker

.PHONY: install
