## Relations

Tilemaker has (as yet not complete) support for reading relations in the Lua process scripts. This means you can support objects like route relations when creating your vector tiles.

Note that relation support is in its early stages and behaviour may change between point versions.


### Multipolygon relations

Multipolygon relations are supported natively by tilemaker; you do not need to write special Lua code for them. When a multipolygon is read, tilemaker constructs the geometry as normal, and passes the tags to `way_function` just as it would a simple area.

Boundary relations also have automatic handling of inner/outer ways, but are otherwise processed as relations. This means that you can choose to treat boundaries as properties on ways (see "Stage 2" below) or as complete geometries (see "Writing relation geometries"). You will typically want the properties-on-ways approach for administrative boundaries, and the complete-geometries approach for filled areas such as forests or nature reserves.


### Reading relation memberships

You can set your Lua script so that `way_function` is able to access the relations that the way is a member of. You would use this, for example, if you wanted to read road numbers from a relation, or to colour roads that are part of a bus route differently.

This is a two-stage process: first, when reading relations, indicate that these should be considered ("accepted"); then, when reading ways in `way_function`, you can access the relation tags.

#### Stage 1: accepting relations

To define which relations should be accepted, add a `relation_scan_function`:

```lua
    function relation_scan_function()
      if Find("type")=="route" and Find("route")=="bicycle" then
        local network = Find("network")
        if network=="ncn" then Accept() end
      end
    end
```

Examine the tags using `Find(key)` as normal. (You can also use `Holds(key)` and `Id()`.) If you want to use this relation, call `Accept()`.

#### Stage 2: accessing relations from ways

Now that you've accepted the relations, they will be available from `way_function`. They are accessed using an iterator (`NextRelation()`) which reads each relation for that way in turn, returning nil when there are no more relations available. Once you have accessed a relation with the iterator, you can read its tags with `FindInRelation(key)`. For example:

```lua
    while true do
      local rel = NextRelation()
      if not rel then break end
      print ("Part of route "..FindInRelation("ref"))
    end
```

(Should you need to re-read the relations, you can reset the iterator with `RestartRelations()`.)


### Writing relation geometries

You can also construct complete multi-linestring geometries from relations. Use this if, for example, you want a geometry for a bike or bus route that you can show at lower zoom levels, or draw with a continuous pattern.

First, make sure that you have accepted the relations using `relation_scan_function` as above.

Then write a `relation_function`, which works in the same way as `way_function` would:

```lua
    function relation_function()
      if Find("type")=="route" and Find("route")=="bicycle" then
        Layer("bike_routes", false)
        Attribute("class", Find("network"))
        Attribute("ref", Find("ref"))
      end
    end
```


### Not supported

Tilemaker does not yet support:

- relation roles
- nested relations
- nodes in relations
