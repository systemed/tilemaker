# Installing Tilemaker

### macOS

Install all dependencies with Homebrew:

	brew install protobuf boost lua51 shapelib
	
Then:

    make
    sudo make install

### Ubuntu

Start with:

	sudo apt-get install build-essential liblua5.1-0 liblua5.1-0-dev libprotobuf-dev libsqlite3-dev protobuf-compiler shapelib libshp-dev

If you're using Ubuntu 16.04, you can install Boost with `sudo apt-get install libboost-all-dev`. For 12.04 or 14.04, you'll need to install a recent Boost from [this PPA](https://launchpad.net/~ostogvin/+archive/ubuntu/tjo-develop):

	sudo add-apt-repository ppa:ostogvin/tjo-develop
	sudo apt-get update
	sudo apt-get install libboost1.58-all-dev

Once you've installed those, then `cd` back to your Tilemaker directory and simply:

    make
    sudo make install

If it fails, check that the LIB and INC lines in the Makefile correspond with your system, then try again.

### Fedora

Start with:

    dnf install lua-devel luajit-devel sqlite-devel protobuf-devel protobuf-compiler shapelib-devel

then build either with lua:

    make LUA_CFLAGS="$(pkg-config --cflags lua)" LUA_LIBS="$(pkg-config --libs lua)"
    make install

or with luajit:

    make LUA_CFLAGS="$(pkg-config --cflags luajit)" LUA_LIBS="$(pkg-config --libs luajit)"
    make install

### Saving memory

To save memory (on any platform), you can choose 32-bit storage for node IDs rather than 64-bit. You will need to run `osmium renumber` or a similar tool over your .osm.pbf first. Then compile Tilemaker with an additional flag:

    make CONFIG="-DCOMPACT_NODES"
    make install

By default, Tilemaker uses 32-bit storage for way IDs and its internal tile index. This shouldn't cause issues with standard OSM data, but if your data needs it, you can compile with `-DFAT_WAYS` for 64-bit. If you are generating vector tiles at zoom level 17 or greater (the usual limit is 14), then compile with `-DFAT_TILE_INDEX`.

### Docker

**The Dockerfile is not formally supported by project maintainers and you are encouraged to send pull requests to fix any issues you encounter.**

Build from project root directory with:

	docker build . -t tilemaker

The docker container can be run like this:

 	docker run -v /Users/Local/Downloads/:/srv -i -t --rm tilemaker /srv/germany-latest.osm.pbf --output=/srv/germany.mbtiles

Keep in mind to map the volume your .osm.pbf files are in to a path within your docker container, as seen in the example above. 
