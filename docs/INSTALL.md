# Installing tilemaker

### macOS

Install all dependencies with Homebrew:

    brew install boost lua shapelib rapidjson

Then:

    make
    sudo make install

(System Integrity Protection on macOS prevents the manpages being installed. This isn't important: ignore the two lines saying "Operation not permitted".)

### Ubuntu and Debian

Start with:

    sudo apt install build-essential libboost-dev libboost-filesystem-dev libboost-program-options-dev libboost-system-dev lua5.3 liblua5.3-dev libshp-dev libsqlite3-dev rapidjson-dev

Once you've installed those, then `cd` back to your Tilemaker directory and simply:

    make
    sudo make install

If it fails, check that the LIB and INC lines in the Makefile correspond with your system, then try again. The above lines install Lua 5.3, but you can also choose any newer version.

### Fedora

Start with:

    dnf install lua-devel luajit-devel sqlite-devel shapelib-devel rapidjson-devel boost-devel

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

### MLT (MapLibre Tiles) support

Support for encoding [MapLibre Tiles](https://github.com/maplibre/maplibre-tile-spec)
is optional and off by default, because the MLT encoder requires C++20 (GCC 11+
or Clang 15+, and on macOS Xcode 15+) while the rest of tilemaker is built as
C++14.

The encoder lives in the `maplibre-tile-spec` submodule, which is not populated
by a plain `git clone`. To fetch it:

    make mlt-deps

This makes a partial, sparse checkout of the parts tilemaker needs (a few MB,
rather than ~380MB for the full tree with all its submodules). Then build with:

    make MLT=1

or, with cmake:

    cmake -B build -DTILEMAKER_BUILD_MLT=ON

`tilemaker --help` reports the encoder revision when MLT support is compiled in.

To move to a newer MLT revision, update the submodule and rebuild:

    git -C maplibre-tile-spec fetch --depth 1 origin main
    git -C maplibre-tile-spec checkout FETCH_HEAD
    git add maplibre-tile-spec

### Docker

**The Dockerfile is not formally supported by project maintainers and you are encouraged to send pull requests to fix any issues you encounter.**

Build from project root directory with:

    docker build . -t tilemaker

It can also be build with a `BUILD_DEBUG` build argument, which will build the executables for Debug, and not strip out symbols. `gdb` will also
installed to facilate debugging:

    docker build . --build-arg BUILD_DEBUG=1 -t tilemaker

The docker container can be run like this:

    docker run -it --rm -v $(pwd):/data tilemaker /data/monaco-latest.osm.pbf --output /data/monaco-latest.pmtiles

The tilemaker-server can be run like this:

    docker run -it --rm -v $(pwd):/data --entrypoint /usr/src/app/tilemaker-server tilemaker --help

Keep in mind to map the volume your .osm.pbf files are in to a path within your docker container, as seen in the example above. 
