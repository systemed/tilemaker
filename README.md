# Tilemaker

Tilemaker creates vector tiles (in Mapbox Vector Tile format) from an .osm.pbf planet extract, as typically downloaded from providers like Geofabrik. It aims to be 'stack-free': you need no database and there is only one executable to install.

Vector tiles are used by many in-browser/app renderers, and can also power server-side raster rendering. They enable on-the-fly style changes and greater interactivity, while imposing less of a storage burden. You can output them to individual files, or to a SQLite (.mbtiles) database.

See an example of a vector tile map produced by tilemaker at [tilemaker.org](https://tilemaker.org).

![Continuous Integration](https://github.com/systemed/tilemaker/workflows/Continuous%20Integration/badge.svg)

## Installing

Tilemaker is written in C++14. The chief dependencies are:

* Google Protocol Buffers
* Boost (latest version advised, 1.66 minimum)
* Lua (5.1 or later) or LuaJIT
* sqlite3
* shapelib

rapidjson, sqlite_modern_cpp, and kaguya are bundled in the include/ directory.

You can then simply install with:

    make
    sudo make install
	
For detailed installation instructions for your operating system, see [INSTALL.md](docs/INSTALL.md).

## Out-of-the-box setup

Tilemaker comes with configuration files compatible with the popular [OpenMapTiles](https://openmaptiles.org) schema, and a demonstration map server. You'll run tilemaker to make vector tiles from your `.osm.pbf` source data. To create the tiles, run this from the tilemaker directory:

    tilemaker --input /path/to/your/input.osm.pbf \
        --output /path/to/your/output.mbtiles

If you want to include sea tiles, then create a directory called `coastline` in the same place you're running tilemaker from, and then save the files from https://osmdata.openstreetmap.de/download/water-polygons-split-4326.zip in it, such that tilemaker can find a file at `coastline/water_polygons.shp`.

Then, to serve your tiles using the demonstration server:

    cd server
    ruby server.rb /path/to/your/output.mbtiles

You can now navigate to http://localhost:8080/ and see your map!

(If you don't already have them, you'll need to install Ruby and the required gems to run the demonstration server. On Ubuntu, for example, `sudo apt install sqlite3 libsqlite3-dev ruby ruby-dev` and then `sudo gem install sqlite3 cgi glug rack`.)

## Your own configuration

Vector tiles contain (generally thematic) 'layers'. For example, your tiles might contain river, cycleway and railway layers. It's up to you what OSM data goes into each layer. You configure this in tilemaker with two files:

* a JSON file listing each layer, and the zoom levels at which to apply it
* a Lua program that looks at each node/way's tags, and places it into layers accordingly

You can read more about these in [CONFIGURATION.md](docs/CONFIGURATION.md).

The JSON configuration and Lua processing files are specified with `--config` and `--process` respectively. Defaults are config.json and process.lua in the current directory. If there is no config.json and process.lua in the current directory, and you do not specify `--config` and `--process`, an error will result.

Read about tilemaker's runtime options in [RUNNING.md](docs/RUNNING.md).

You might also find these resources helpful:

* Read our [introduction to vector tiles](docs/VECTOR_TILES.md).
* See a workflow for "Generating self-hosted maps using tilemaker" at https://blog.kleunen.nl/blog/tilemaker-generate-map.
* See https://github.com/mapbox/awesome-vector-tiles for a list of renderers which support vector tiles.

## Why tilemaker?

You might use tilemaker if:

* You want to create vector tiles yourself, without a third-party contract
* You don't want to host/maintain a database
* You want a flexible system capable of advanced OSM tag processing
* You want to create ready-to-go tiles for offline use

But don't use tilemaker if:

* You want someone else to create and host the tiles for you
* You want the entire planet
* You want continuous updates with the latest OSM data

## Contributing

Bug reports, suggestions and (especially!) pull requests are very welcome on the Github issue tracker. Please check the tracker to see if your issue is already known, and be nice. For questions, please use IRC (irc.oftc.net or https://irc.osm.org, channel #osm-dev) and https://help.osm.org.

Formatting: braces and indents as shown, hard tabs (4sp). (Yes, I know.) Please be conservative about adding dependencies or increasing the memory requirement.

## Copyright

Tilemaker is maintained by Richard Fairhurst and supported by [many contributors](https://github.com/systemed/tilemaker/graphs/contributors).

Copyright tilemaker contributors, 2015-2021. The tilemaker code is licensed as FTWPL; you may do anything you like with this code and there is no warranty. The included rapidjson (Milo Yip and THL A29) and sqlite_modern_cpp (Amin Roosta) are MIT; [kaguya](https://github.com/satoren/kaguya) is licensed under the Boost Software Licence.
