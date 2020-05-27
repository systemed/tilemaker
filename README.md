# Tilemaker

Tilemaker creates vector tiles (in Mapbox Vector Tile format) from an .osm.pbf planet extract, as typically downloaded from providers like Geofabrik. It aims to be 'stack-free': you need no database and there is only one executable to install.

Vector tiles are used by many in-browser/app renderers, and can also power server-side raster rendering. They enable on-the-fly style changes and greater interactivity, while imposing less of a storage burden. You can output them to individual files, or to a SQLite (.mbtiles) database.

Tilemaker keeps nodes and ways in RAM. If you're processing a country extract or larger, you'll need a lot of RAM. It's best suited to city and region extracts.

![Continuous Integration](https://github.com/systemeD/tilemaker/workflows/Continuous%20Integration/badge.svg)

## Installing

Tilemaker is written in C++11. The chief dependencies are:

* Google Protocol Buffers
* Boost (latest version advised, 1.56 minimum: for boost::geometry, boost::program_options, boost::filesystem, boost::variant)
* Lua 5.1
* sqlite3
* shapelib

rapidjson, sqlite_modern_cpp, clipper, kaguya and sparse-map are bundled in the include/ directory.

You can then simply install with:

    make
    sudo make install
	
For detailed installation instructions for your operating system, see INSTALL.md.

## Out-of-the-box setup

Tilemaker comes with configuration files compatible with the popular [OpenMapTiles](https://openmaptiles.org) schema, and a demonstration map server. You'll run tilemaker to make vector tiles from your `.osm.pbf` source data. To create the tiles:

    tilemaker tilemaker --input /path/to/your/input.osm.pbf \
        --output /path/to/your/output.mbtiles \
        --config resources/config-openmaptiles.json \
        --process resources/process-openmaptiles.lua

If you want to include sea tiles, then create a directory called `coastline` in the same place you're running tilemaker from, and then save the files from https://osmdata.openstreetmap.de/download/water-polygons-split-4326.zip in it, such that tilemaker can find a file at `coastline/water_polygons.shp`.

Then, to serve your tiles using the demonstration server:

    cd server
    ruby server.rb /path/to/your/output.mbtiles

You can now navigate to http://localhost:8080/ and see your map!

## Your own configuration

Vector tiles contain (generally thematic) 'layers'. For example, your tiles might contain river, cycleway and railway layers. It's up to you what OSM data goes into each layer. You configure this in Tilemaker with two files:

* a JSON file listing each layer, and the zoom levels at which to apply it
* a Lua program that looks at each node/way's tags, and places it into layers accordingly

You can read more about these in [CONFIGURATION.md](CONFIGURATION.md).

At its simplest, you can create a set of vector tiles from a .pbf with this command:

    tilemaker liechtenstein-latest.osm.pbf --output=liechtenstein.mbtiles

Output can be as individual files to a directory, or to an MBTiles file aka a SQLite database (with extension .mbtiles or .sqlite). Any existing MBTiles file will be deleted (if you don't want this, specify `--merge`).

The JSON configuration and Lua processing files are specified with `--config` and `--process` respectively. Defaults are config.json and process.lua in the current directory. If there is no config.json and process.lua in the current directory, and you do not specify `--config` and `--process`, an error will result.

You can get a run-down of available options with

    tilemaker --help

When running, you may see "couldn't find constituent way" messages. This happens when the .pbf file contains a multipolygon relation, but not all the relation's members are present. Typically, this will happen when a multipolygon crosses the border of the extract - for example, a county boundary formed by a river with islands. In this case, the river will simply not be written to the tiles.

See https://github.com/mapbox/awesome-vector-tiles for a list of renderers which support vector tiles.

## Github Action
You can integrate tilemaker as Github Action into your [Github Workflow](https://help.github.com/en/actions).
```yaml
- uses: systemed/tilemaker@master
  with:
    # Same to --input
    input: ''
    # Same to --config
    # If not being set, default to resources/config-openmaptiles.config
    config: ''
    # Same to --process
    # If not being set, default to resources/process-openmaptiles.lua
    input: ''
    # Same to --output
    output: ''
    # Other options
    # If not being set, default to '--verbose'
    extra: ''
```

## Contributing

Bug reports, suggestions and (especially!) pull requests are very welcome on the Github issue tracker. Please check the tracker to see if your issue is already known, and be nice. For questions, please use IRC (irc.oftc.net or http://irc.osm.org, channel #osm-dev) and http://help.osm.org.

Formatting: braces and indents as shown, hard tabs (4sp). (Yes, I know.) Please be conservative about adding dependencies or increasing the memory requirement.

## Copyright and contact

Richard Fairhurst and contributors, 2015-2020. The tilemaker code is licensed as FTWPL; you may do anything you like with this code and there is no warranty. The included rapidjson (Milo Yip and THL A29), sqlite_modern_cpp (Amin Roosta), and sparse-map (Tessil) libraries are MIT; [kaguya](https://github.com/satoren/kaguya) is licensed under the Boost Software Licence.

If you'd like to sponsor development of Tilemaker, you can contact me at richard@systemeD.net.

Thank you to the usual suspects for support and advice (you know who you are), to Mapbox for developing vector tiles, and to Dennis Luxen for the introduction to Lua and the impetus to learn C++.

(Looking for a provider to host vector tiles? I recommend Thunderforest: http://thunderforest.com/)
