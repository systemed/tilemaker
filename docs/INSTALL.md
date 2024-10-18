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

    sudo apt install build-essential libboost-dev libboost-filesystem-dev libboost-program-options-dev libboost-system-dev lua5.1 liblua5.1-0-dev libshp-dev libsqlite3-dev rapidjson-dev

Once you've installed those, then `cd` back to your Tilemaker directory and simply:

    make
    sudo make install

If it fails, check that the LIB and INC lines in the Makefile correspond with your system, then try again. The above lines install Lua 5.1, but you can also choose any newer version.

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
