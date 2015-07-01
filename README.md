Tilemaker
=========

Tilemaker creates vector tiles (in Mapbox Vector Tile format) from an .osm.pbf planet extract, as typically downloaded from providers like Geofabrik. It aims to be 'stack-free': you need no database and there is only one executable to install.

Vector tiles are used by many in-browser/app renderers, and can also power server-side raster rendering. They enable on-the-fly style changes and greater interactivity, while imposing less of a storage burden. You can output them to individual files, or to a SQLite (.mbtiles) database.

Tilemaker keeps nodes and ways in RAM. If you're processing a country extract or larger, you'll need a lot of RAM. It's best suited to city and region extracts.

Installing
----------

Tilemaker is written in C++11. The chief dependencies are:

* Google Protocol Buffers
* Boost 1.57 (for boost::geometry, boost::program_options, boost::filesystem)
* Lua 5.1 and Luabind
* sqlite3

rapidjson (MIT) and sqlite_modern_cpp (MIT) are bundled in the include/ directory.

On OS X, you can install all dependencies with Homebrew. On Ubuntu, start with:

	apt-get install liblua5.1-0 liblua5.1-0-dev libprotobuf-dev libsqlite3-dev protobuf-c-compiler

You'll then need to install libboost1.57-all-dev from [this PPA](https://launchpad.net/~afrank/+archive/ubuntu/boost):

	add-apt-repository ppa:afrank/boost
	apt-get update
	apt-get install libboost1.57-all-dev

Finally, we need to install luabind manually because the Ubuntu package (sigh) requires Boost 1.54, whereas we need 1.57. So:

	git clone https://github.com/rpavlik/luabind.git
	cd luabind
	# The following line might not be necessary for you,
	# but I needed it to make sure that liblua was in /usr/lib:
	ln -s /usr/lib/x86_64-linux-gnu/liblua5.1.so /usr/lib/
	bjam install
	ln -s /usr/local/lib/libluabindd.so /usr/local/lib/libluabind.so
	ldconfig

Once you've installed those, then `cd` back to your Tilemaker directory and simply:

    make
    make install

If it fails, check that the LIB and INC lines in the Makefile correspond with your system, then try again.

On Fedora start with:

    dnf install lua-devel luajit-devel luabind-devel sqlite-devel protobuf-devel protobuf-compiler

then build either with lua:

    make LUA_CFLAGS="$(pkg-config --cflags lua)" LUA_LIBS="$(pkg-config --libs lua)"
    make install

or with luajit:

    make LUA_CFLAGS="$(pkg-config --cflags luajit)" LUA_LIBS="$(pkg-config --libs luajit)"
    make install

Configuring
-----------

Vector tiles contain (generally thematic) 'layers'. For example, your tiles might contain river, cycleway and railway layers. It's up to you what OSM data goes into each layer. You configure this in Tilemaker with two files:

* a JSON file listing each layer, and the zoom levels at which to apply it
* a Lua program that looks at each node/way's tags, and places it into layers accordingly

You can read more about these in [CONFIGURATION.md](CONFIGURATION.md). Sample files are provided to work out-of-the-box.

Running
-------

At its simplest, you can create a set of vector tiles from a .pbf with this command:

    tilemaker liechtenstein-latest.osm.pbf --output=tiles/ 

Output can be as individual files to a directory, or to an MBTiles file aka a SQLite database (with extension .mbtiles or .sqlite).

You may load multiple .pbf files in one run (for example, adjoining counties). Tilemaker does not clear the existing contents of MBTiles files, which makes it easy to load two cities into one file. This does mean you should delete any existing file if you want a fresh run.

The JSON configuration and Lua processing files are specified with --config and --process respectively. Defaults are config.json and process.lua.

You can get a run-down of available options with

    tilemaker --help

When running, you may see "couldn't find constituent way" messages. This happens when the .pbf file contains a multipolygon relation, but not all the relation's members are present. Typically, this will happen when a multipolygon crosses the border of the extract - for example, a county boundary formed by a river with islands. In this case, the river will simply not be written to the tiles.

Rendering
---------

That bit's up to you! See https://github.com/mapbox/vector-tile-spec/wiki/Implementations for a list of renderers which support vector tiles.

The [Leaflet.MapboxVectorTile plugin](https://github.com/SpatialServer/Leaflet.MapboxVectorTile) is perhaps the simplest way to test out your new vector tiles.

Contributing
------------

Bug reports, suggestions and (especially!) pull requests are very welcome on the Github issue tracker. Please check the tracker to see if your issue is already known, and be nice. For questions, please use IRC (irc.oftc.net or http://irc.osm.org, channel #osm-dev) and http://help.osm.org.

Formatting: braces and indents as shown, hard tabs (4sp). (Yes, I know.) Please be conservative about adding dependencies.


Copyright and contact
---------------------

Richard Fairhurst, 2015. The tilemaker code is licensed as FTWPL; you may do anything you like with this code and there is no warranty. The included rapidjson (Milo Yip and THL A29) and sqlite_modern_cpp (Amin Roosta) libraries are MIT.

If you'd like to sponsor development of Tilemaker, you can contact me at richard@systemeD.net.

Thank you to the usual suspects for support and advice (you know who you are), to Mapbox for developing vector tiles, and to Dennis Luxen for the introduction to Lua and the impetus to learn C++.

(Looking for a provider to host vector tiles? I recommend Thunderforest: http://thunderforest.com/)
