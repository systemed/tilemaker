# Installing tilemaker

### macOS

Install all dependencies with Homebrew:

    brew install boost lua shapelib rapidjson

Then:

    make
    sudo make install

(System Integrity Protection on macOS prevents the manpages being installed. This isn't important: ignore the two lines saying "Operation not permitted".)

### Ubuntu

Start with:

    sudo apt install build-essential libboost-dev libboost-filesystem-dev libboost-iostreams-dev libboost-program-options-dev libboost-system-dev liblua5.1-0-dev libshp-dev libsqlite3-dev rapidjson-dev zlib1g-dev

Once you've installed those, then `cd` back to your Tilemaker directory and simply:

    make
    sudo make install

If it fails, check that the LIB and INC lines in the Makefile correspond with your system, then try again. The above lines install Lua 5.1, but you can also choose any newer version.

### Fedora

Start with:

    dnf install lua-devel luajit-devel sqlite-devel shapelib-devel rapidjson-devel boost-devel zlib-devel

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

tilemaker has a compile-time option that increases memory usage but may be useful in certain circumstances. You can include it when building like this:

    make "CONFIG=-DFAT_TILE_INDEX"

FAT_TILE_INDEX allows you to generate vector tiles at zoom level 17 or greater. You almost certainly don't need to do this. Vector tiles are usually generated up to zoom 14 (sometimes 15), and then the browser/app client uses the vector data to scale up at subsequent zoom levels.
