Tilemaker
=========

Tilemaker creates vector tiles (in Mapbox Vector Tile format) from an .osm.pbf planet extract, as typically downloaded from providers like Geofabrik. It aims to be 'stack-free': you need no database and there is only one executable to install.

Vector tiles are used by many in-browser/app renderers, and can also power server-side raster rendering. They enable on-the-fly style changes and greater interactivity, while imposing less of a storage burden. You can output them to individual files, or to a SQLite (.mbtiles) database.

Tilemaker keeps nodes and ways in RAM. If you're processing a country extract or larger, you'll need a lot of RAM. It's best suited to city and region extracts.

Installing
----------

Tilemaker is written in C++11. The chief dependencies are:

* Google Protocol Buffers
* Boost (latest version advised, 1.56 minimum: for boost::geometry, boost::program_options, boost::filesystem, boost::variant)
* Lua 5.1
* sqlite3
* shapelib

rapidjson, sqlite_modern_cpp, clipper, kaguya and sparse-map are bundled in the include/ directory.

### OS X

Install all dependencies with Homebrew:

	brew install protobuf boost lua51 shapelib

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

Configuring
-----------

Vector tiles contain (generally thematic) 'layers'. For example, your tiles might contain river, cycleway and railway layers. It's up to you what OSM data goes into each layer. You configure this in Tilemaker with two files:

* a JSON file listing each layer, and the zoom levels at which to apply it
* a Lua program that looks at each node/way's tags, and places it into layers accordingly

You can read more about these in [CONFIGURATION.md](CONFIGURATION.md). Sample files are provided to work out-of-the-box: you may find more in the `resources` directory.

Running
-------

At its simplest, you can create a set of vector tiles from a .pbf with this command:

    tilemaker liechtenstein-latest.osm.pbf --output=tiles/

Output can be as individual files to a directory, or to an MBTiles file aka a SQLite database (with extension .mbtiles or .sqlite).

You may load multiple .pbf files in one run (for example, adjoining counties). Tilemaker does not clear the existing contents of MBTiles files, which makes it easy to load two cities into one file. This does mean you should delete any existing file if you want a fresh run.

The JSON configuration and Lua processing files are specified with --config and --process respectively. Defaults are config.json and process.lua in the current directory. If there is no config.json and process.lua in the current directory, and you do not specify --config and --process, an error will result.

You can get a run-down of available options with

    tilemaker --help

When running, you may see "couldn't find constituent way" messages. This happens when the .pbf file contains a multipolygon relation, but not all the relation's members are present. Typically, this will happen when a multipolygon crosses the border of the extract - for example, a county boundary formed by a river with islands. In this case, the river will simply not be written to the tiles.

Rendering
---------

That bit's up to you! See https://github.com/mapbox/awesome-vector-tiles for a list of renderers which support vector tiles.

The [Leaflet.MapboxVectorTile plugin](https://github.com/SpatialServer/Leaflet.MapboxVectorTile) is perhaps the simplest way to test out your new vector tiles.

Contributing
------------

Bug reports, suggestions and (especially!) pull requests are very welcome on the Github issue tracker. Please check the tracker to see if your issue is already known, and be nice. For questions, please use IRC (irc.oftc.net or http://irc.osm.org, channel #osm-dev) and http://help.osm.org.

Formatting: braces and indents as shown, hard tabs (4sp). (Yes, I know.) Please be conservative about adding dependencies.


Copyright and contact
---------------------

Richard Fairhurst and contributors, 2015-2020. The tilemaker code is licensed as FTWPL; you may do anything you like with this code and there is no warranty. The included rapidjson (Milo Yip and THL A29), sqlite_modern_cpp (Amin Roosta), and sparse-map (Tessil) libraries are MIT; [kaguya](https://github.com/satoren/kaguya) is licensed under the Boost Software Licence.

If you'd like to sponsor development of Tilemaker, you can contact me at richard@systemeD.net.

Thank you to the usual suspects for support and advice (you know who you are), to Mapbox for developing vector tiles, and to Dennis Luxen for the introduction to Lua and the impetus to learn C++.

(Looking for a provider to host vector tiles? I recommend Thunderforest: http://thunderforest.com/)
