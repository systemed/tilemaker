# Running tilemaker

You can get a run-down of available options with

    tilemaker --help

The following document explains each option.

## Standard usage

To create vector tiles from an OpenStreetMap .pbf extract, tilemaker's standard syntax is:

    tilemaker --input oxfordshire.osm.pbf \
              --output oxfordshire.mbtiles \
              --config resources/config-openmaptiles.json \
              --process resources/process-openmaptiles.lua

The `--config` and `--process` arguments are the paths of your JSON config file and Lua 
processing script. These are described in CONFIGURATION.md. Here we're using the ready-made 
OpenMapTiles-compatible script.

You can output to an .mbtiles or .pmtiles file. mbtiles is widely supported and easy to serve 
(it's an sqlite database under the hood). [pmtiles](https://github.com/protomaps/PMTiles) is 
a newer format optimised for serving over the cloud. You can also write tiles directly to the 
filesystem by specifying a directory path for `--output`.

This is all you need to know, but if you want to reduce memory requirements, read on.

## Using on-disk storage

By default, tilemaker uses RAM to store the nodes and ways it reads from the .osm.pbf, prior 
to writing them out as vector tiles. This is fine for small regions, but can impose high memory 
requirements for larger areas.

To use on-disk storage instead, pass the `--store` argument with a path to the directory where 
you want the temporary store to be created - on an SSD or other fast disk. This allows you 
to process bigger areas even on memory-constrained systems.

## Performance and memory tuning

By default, tilemaker aims to balance memory usage with speed, but with a slight tilt towards 
minimising memory. You can get faster runtimes, at the expense of a little more memory, by 
specifying `--fast`.

This is all most people will need to know. But if you have plentiful RAM, you can experiment 
with these options. In general, options that use more memory run faster - but if you can 
optimise memory such that your dataset fits entirely in RAM, this will be a big speed-up.
(`--fast` simply chooses a set of these options for you.)

* `--compact`: Use a smaller, faster data structure for node lookups. __Note__: This requires 
the .pbf to have nodes in sequential order, typically by using `osmium renumber`.
* `--no-compress-nodes` and `--no-compress-ways`: Turn off node/way compression. Increases 
RAM usage but runs faster.
* `--materialize-geometries`: Generate geometries in advance when reading .pbf. Increases RAM 
usage but runs faster.
* `--shard-stores`: Group temporary storage by area. Reduces RAM usage on large files (e.g.
whole planet) but runs slower.

You can also tell tilemaker to only look at .pbf objects with certain tags. If you're making a 
thematic map, this allows tilemaker to skip data it won't need. Specify this in your Lua file 
like one of these three examples:

    -- Only include major roads
    way_keys = {"highway=motorway", "highway=trunk", "highway=primary", "highway=secondary"}`

    -- Only include railways
    way_keys = {"railway"}

    -- Include everything but not buildings
    way_keys = {"~building"}

## Merging

You can specify multiple .pbf files on the command line, and tilemaker will read them all in 
before writing the vector tiles.

Alternatively, you can use the `--merge` switch to add to an existing .mbtiles. Create your
.mbtiles in the usual way:

    tilemaker --input australia.osm.pbf \
              --output oceania.mbtiles \
              [...]

Then rerun with another .pbf, using the `--merge` flag:

    tilemaker --input new-zealand.osm.pbf \
              --output oceania.mbtiles \
              --merge \
              [...]

The second run will proceed a little more slowly due to reading in existing tiles in areas which 
overlap. Any OSM objects which appear in both files will be written twice.

### Creating a map with varying detail

A map with global coastline, but detailed mapping only for a specific region, is a common use case.
You can use tilemaker's `--merge` switch to achieve this.

First, create a global coastline .mbtiles. There's a special stripped down config for this:

    tilemaker --output coastline.mbtiles \
              --bbox -180,-85,180,85 \
              --process resources/process-coastline.lua \
              --config resources/config-coastline.json

Save this .mbtiles somewhere; then make a copy, and call it output.mbtiles.

Edit `resources/config-openmaptiles.json` to remove the `ocean`, `urban_areas`, `ice_shelf` and 
`glacier` layers (because we've already generated these).

Now simply merge the region you want into the coastline .mbtiles you generated:

    tilemaker --input new-zealand.osm.pbf \
              --output output.mbtiles \
              --merge \
              --process resources/process-openmaptiles.lua \
              --config resources/config-openmaptiles.json

Don't forget to add `--store /path/to/your/ssd` if you don't have lots of RAM.

## Output messages

Running tilemaker with the `--verbose` argument will output any issues encountered during tile
creation.

You may see geometry errors reported by Boost::Geometry. This typically reflects an error 
in the OSM source data (for example, a multipolygon with several inner rings but no outer ring).
Often, if the geometry could not be written to the layer, the error will subsequently show in 
a failed attempt to add attributes afterwards.

If you see a (possibly fatal) error about nodes missing from ways, or ways missing from 
relations, this suggests your source .osm.pbf is malformed. This will often happen if you have 
used another program to clip the .osm.pbf with a bounding polygon. You can tell tilemaker to 
ignore missing nodes in ways with `--skip-integrity`, but it can't fix missing ways in 
multipolygon relations. Instead, tell your clipping utility to create a well-formed file using 
`--strategy=smart` (Osmium) or `clipIncompleteEntities=true` (Osmosis).

tilemaker is meant for use with OSM data. It will likely not work if you add your own data 
to the .osm.pbf file with unusual IDs (e.g. negative IDs or very large numbers). If you must 
do this, use a tool like `osmium renumber` first to get the IDs back to a normal range.

## Github Action

You can integrate tilemaker as a Github Action into your [Github Workflow](https://help.github.com/en/actions).  
Here is an example:

```yaml
- uses: systemed/tilemaker@v2.0.0
  with:
    # Required, same to --input
    input: /path/to/osm.pbf
    # Required, same to --output. Could be a directory or a .mbtiles files
    output: /path/to/output
    # Optional, same to --config
    # If not being set, default to resources/config-openmaptiles.json
    config: /path/to/config
    # Optional, same to --process
    # If not being set, default to resources/process-openmaptiles.lua
    process: /path/to/lua
    # Optional, other arguments
    # If not being set, default to '--verbose'
    extra: --threads 0
```
