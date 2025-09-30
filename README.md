# tilemaker

tilemaker creates vector tiles (in Mapbox Vector Tile format) from an .osm.pbf planet extract, as typically downloaded from providers like Geofabrik. It aims to be 'stack-free': you need no database and there is only one executable to install.

Vector tiles are used by many in-browser/app renderers, and can also power server-side raster rendering. They enable on-the-fly style changes and greater interactivity, while imposing less of a storage burden. tilemaker can output them to individual files, or to .mbtiles or .pmtiles tile containers.

See an example of a vector tile map produced by tilemaker at [tilemaker.org](https://tilemaker.org).

![Continuous Integration](https://github.com/systemed/tilemaker/workflows/Continuous%20Integration/badge.svg)

## Getting Started

We provide a ready-to-use docker image that gets you started without having to compile tilemaker from source:

1. Go to [Geofabrik](http://download.geofabrik.de/europe.html) and download the `monaco-latest.osm.pbf` snapshot of OpenStreetMap
2. Run tilemaker on the OpenStreetMap snapshot to generate [Protomaps](https://protomaps.com) vector tiles (see below)

```
    docker run -it --rm -v $(pwd):/data ghcr.io/systemed/tilemaker:master /data/monaco-latest.osm.pbf --output /data/monaco-latest.pmtiles
```

3. Check out what's in the vector tiles e.g. by using the debug viewer [here](https://protomaps.github.io/PMTiles/)

To run tilemaker with its default configuration

```bash
docker run -it --rm --pull always -v $(pwd):/data \
  ghcr.io/systemed/tilemaker:master \
  /data/monaco-latest.osm.pbf \
  --output /data/monaco-latest.pmtiles
```

To run tilemaker with a custom configuration using coastlines and landcover you have two options
1. In the config.json use absolute paths such as `/data/coastline/water_polygons.shp` or
2. Set the docker workdir `-w /data` with relative paths `coastline/water_polygons.shp` (see below)

```bash
docker run -it --rm --pull always -v $(pwd):/data -w /data \
  ghcr.io/systemed/tilemaker:master \
  /data/monaco-latest.osm.pbf \
  --output /data/monaco-latest.pmtiles \
  --config /data/config-coastline.json \
  --process /data/process-coastline.lua
```

## Installing

tilemaker is written in C++14. The chief dependencies are:

* Boost (latest version advised, 1.66 minimum)
* Lua (5.1 or later) or LuaJIT
* sqlite3
* shapelib
* rapidjson

Other third-party code is bundled in the include/ directory.

You can then simply install with:

    make
    sudo make install
	
For detailed installation instructions for your operating system, see [INSTALL.md](docs/INSTALL.md).

## Out-of-the-box setup

tilemaker comes with configuration files compatible with the popular [OpenMapTiles](https://openmaptiles.org) schema, and a demonstration map server. You'll run tilemaker to make vector tiles from your `.osm.pbf` source data. To create the tiles, run this from the tilemaker directory:

    tilemaker /path/to/your/input.osm.pbf /path/to/your/output.mbtiles

tilemaker keeps everything in RAM by default. To process large areas without running out of memory, tell it to use temporary storage on SSD:

    tilemaker /path/to/your/input.osm.pbf /path/to/your/output.mbtiles --store /path/to/your/ssd

Then, to serve your tiles using the demonstration server:

    cd server
	tilemaker-server /path/to/your/output.mbtiles

You can now navigate to http://localhost:8080/ and see your map!

## Coastline and Landcover

To include sea tiles and small-scale landcover, run

    ./get-coastline.sh
    ./get-landcover.sh

This will download coastline and landcover data; you will need around 2GB disk space.

Have a look at the coastline and landcover example in the [`resources/`](./resources) directory.

## Your own configuration

Vector tiles contain (generally thematic) 'layers'. For example, your tiles might contain river, cycleway and railway layers. It's up to you what OSM data goes into each layer. You configure this in tilemaker with two files:

* a JSON file listing each layer, and the zoom levels at which to apply it
* a Lua program that looks at each node/way's tags, and places it into layers accordingly

You can read more about these in [CONFIGURATION.md](docs/CONFIGURATION.md).

The JSON configuration and Lua processing files are specified with `--config` and `--process` respectively. Defaults are config.json and process.lua in the current directory. If there is no config.json and process.lua in the current directory, and you do not specify `--config` and `--process`, an error will result.

Read about tilemaker's runtime options in [RUNNING.md](docs/RUNNING.md).

You might also find these resources helpful:

* Read our [introduction to vector tiles](docs/VECTOR_TILES.md).
* See https://github.com/mapbox/awesome-vector-tiles for a list of renderers which support vector tiles.

## Why tilemaker?

You might use tilemaker if:

* You want to create vector tiles yourself, without a third-party contract
* You don't want to host/maintain a database
* You want a flexible system capable of advanced OSM tag processing
* You want to create ready-to-go tiles for offline use

But don't use tilemaker if:

* You want someone else to create and host the tiles for you
* You want continuous updates with the latest OSM data

## Contributing

Bug reports, suggestions and (especially!) pull requests are very welcome on the Github issue tracker. Please check the tracker to see if your issue is already known, and be nice. For questions, please use IRC (irc.oftc.net or https://irc.osm.org, channel #osm-dev) and https://community.osm.org.

Formatting: braces and indents as shown, hard tabs (4sp). (Yes, I know.) Please be conservative about adding dependencies or increasing the memory requirement.

The Makefile does not currently pick up changes to header files (.h). If you change these, you may need to run `make clean` before building with `make` and `sudo make install`.

## Copyright

tilemaker is maintained by Richard Fairhurst and supported by [many contributors](https://github.com/systemed/tilemaker/graphs/contributors). We particularly celebrate the invaluable contributions of Wouter van Kleunen, who passed away in 2022.

Copyright tilemaker contributors, 2015-2025.

The tilemaker code is licensed as FTWPL; you may do anything you like with this code and there is no warranty.

Licenses of third-party libraries:

- [kaguya](https://github.com/satoren/kaguya) is licensed under the Boost Software Licence
- [libdeflate](https://github.com/ebiggers/libdeflate/) is licensed under MIT
- [libpopcnt](https://github.com/kimwalisch/libpopcnt) is licensed under BSD 2-clause
- [minunit](https://github.com/siu/minunit) is licensed under MIT
- [pmtiles](https://github.com/protomaps/PMTiles) is licensed under BSD 3-clause
- [polylabel](https://github.com/mapbox/polylabel) is licensed under ISC
- [protozero](https://github.com/mapbox/protozero) is licensed under BSD 2-clause
- [Simple-Web-Server](https://gitlab.com/eidheim/Simple-Web-Server) is licensed under MIT
- [sqlite_modern_cpp](https://github.com/SqliteModernCpp/sqlite_modern_cpp) is licensed under MIT
- [streamvbyte](https://github.com/lemire/streamvbyte) is licensed under Apache 2
- [visvalingam.cpp](https://github.com/felt/tippecanoe/blob/main/visvalingam.cpp) is licensed under MIT
- [vtzero](https://github.com/mapbox/vtzero) is licensed under BSD 2-clause
