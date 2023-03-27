# Installing tilemaker

### macOS

Install all dependencies with Homebrew:

    brew install protobuf boost lua51 shapelib rapidjson

Then:

    make
    sudo make install

### Ubuntu

Start with:

    sudo apt install build-essential libboost-dev libboost-filesystem-dev libboost-iostreams-dev libboost-program-options-dev libboost-system-dev liblua5.1-0-dev libprotobuf-dev libshp-dev libsqlite3-dev protobuf-compiler rapidjson-dev

Once you've installed those, then `cd` back to your Tilemaker directory and simply:

    make
    sudo make install

If it fails, check that the LIB and INC lines in the Makefile correspond with your system, then try again.

### Fedora

Start with:

    dnf install lua-devel luajit-devel sqlite-devel protobuf-devel protobuf-compiler shapelib-devel rapidjson

then build either with lua:

    make LUA_CFLAGS="$(pkg-config --cflags lua)" LUA_LIBS="$(pkg-config --libs lua)"
    make install

or with luajit:

    make LUA_CFLAGS="$(pkg-config --cflags luajit)" LUA_LIBS="$(pkg-config --libs luajit)"
    make install

### Using cmake

You can optionally use cmake to build:

    mkdir build
    cd build
    cmake ..
    make
    sudo make install

### Docker

**The Dockerfile is not formally supported by project maintainers and you are encouraged to send pull requests to fix any issues you encounter.**

Build from project root directory with:

    docker build . -t tilemaker

The docker container can be run like this:

    docker run -v /Users/Local/Downloads/:/srv -i -t --rm tilemaker /srv/germany-latest.osm.pbf --output=/srv/germany.mbtiles

Keep in mind to map the volume your .osm.pbf files are in to a path within your docker container, as seen in the example above. 

### Compile-time options

tilemaker has two compile-time options that increase memory usage but may be useful in certain circumstances. You can include them when building like this:

    make "CONFIG=-DFLOAT_Z_ORDER"

FLOAT_Z_ORDER allows you to use a full range of ZOrder values in your Lua script, rather than being restricted to single-byte integer (-127 to 127).

FAT_TILE_INDEX allows you to generate vector tiles at zoom level 17 or greater. You almost certainly don't need to do this. Vector tiles are usually generated up to zoom 14 (sometimes 15), and then the browser/app client uses the vector data to scale up at subsequent zoom levels.
