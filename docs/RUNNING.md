# Running tilemaker

You can get a run-down of available options with

    tilemaker --help

The following document explains each option.

## Standard usage

To create vector tiles from an OpenStreetMap .pbf extract, tilemaker's standard syntax is:

    tilemaker --input oxfordshire.osm.pbf \
              --output oxfordshire.mbtiles \
              --config resources/config-openmaptiles.lua \
              --process resources/process-openmaptiles.lua

The `--config` and `--process` arguments are the paths of your JSON config file and Lua 
processing script. These are described in CONFIGURATION.md. Here we're using the ready-made 
OpenMapTiles-compatible script.

You'll usually want to write to an .mbtiles file (which, under the hood, is an sqlite database 
containing the vector tiles). However, you can write tiles directly to the filesystem if you 
like, by specifying a directory path for `--output`.

This is all you need to know, but if you want to reduce memory requirements, read on.

## Using on-disk storage

By default, tilemaker uses RAM to store the nodes and ways it reads from the .osm.pbf, prior 
to writing them out as vector tiles. This is fine for small regions, but can impose high memory 
requirements for larger areas.

To use on-disk storage instead, pass the `--store` argument with a path to the directory where 
you want the temporary store to be created. This should be on an SSD or other fast disk. 
Tilemaker will grow the store as required.

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

For very large areas, you could potentially use `osmium tags-filter` to split a .pbf into several 
"thematic" extracts: for example, one containing buildings, another roads, and another landuse. 
Renumber each one, then run tilemaker several times with `--merge` to add one theme at a time. 
This would greatly reduce memory usage.

## Pre-split data

Tilemaker is able to read pre-split source data, where the original .osm.pbf has already been 
split into tiled areas (but not converted any further). By reducing the amount of data tilemaker 
has to process at any one time, this can greatly reduce memory requirements.

To split an .osm.pbf, use [mapsplit](https://github.com/simonpoole/mapsplit). This will output 
an .msf file. We would recommend that you split the data at a low zoom level, such as 6; 
tilemaker will not be able to generate vector tiles at a lower zoom level than the one you 
choose for your .msf file.

You can then run tilemaker exactly as normal, with the `--input` parameter set to your .msf 
file. Source tiles will be processed one by one. Note that shapefiles will be read unsplit as 
normal.

## Output messages

Running tilemaker with the `--verbose` argument will output any issues encountered during tile
creation.

You may see "couldn't find constituent way" messages. This happens when the .pbf file contains 
a multipolygon relation, but not all the relation's members are present. Typically, this will 
happen when a multipolygon crosses the border of the extract - for example, a county boundary 
formed by a river with islands. In this case, the river will simply not be written to the tiles.

You may also see geometry errors reported by Boost::Geometry. This typically reflects an error 
in the OSM source data (for example, a multipolygon with several inner rings but no outer ring).
Often, if the geometry could not be written to the layer, the error will subsequently show in 
a failed attempt to add attributes afterwards.

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
    # If not being set, default to resources/config-openmaptiles.config
    config: /path/to/config
    # Optional, same to --process
    # If not being set, default to resources/process-openmaptiles.lua
    process: /path/to/lua
    # Optional, other arguments
    # If not being set, default to '--verbose'
    extra: --threads 0
```
