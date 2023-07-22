## Relations

Tilemaker has (as yet not complete) support for reading relations in the Lua process scripts. This means you can support objects like route relations when creating your vector tiles.

Note that relation support is in its early stages and behaviour may change between point versions.


### Multipolygon relations

Multipolygon and boundary relations are supported natively by tilemaker; you do not need to write special Lua code for them. When a multipolygon or boundary is read, tilemaker constructs the geometry as normal, and passes the tags to `way_function` just as it would a simple area.


### Reading relation memberships

You can set your Lua script so that `way_function` is able to access the relations that the way is a member of. You would use this, for example, if you wanted to read road numbers from a relation, or to colour roads that are part of a bus route differently.

This is a two-stage process: first, when reading relations, indicate that these should be considered ("accepted"); then, when reading ways in `way_function`, you can access the relation tags.

#### Stage 1: accepting relations

To define which relations should be accepted, add a `relation_scan_function`:

    function relation_scan_function(relation)
      if relation:Find("type")=="route" and relation:Find("route")=="bicycle" then
        local network = relation:Find("network")
        if network=="ncn" then relation:Accept() end
      end
    end

This function takes the relation as its sole argument. Examine the tags using `relation:Find(key)` as normal. (You can also use `relation:Holds(key)` and `relation:Id()`.) If you want to use this relation, call `relation:Accept()`.

#### Stage 2: accessing relations from ways

Now that you've accepted the relations, they will be available from `way_function`. They are accessed using an iterator (`way:NextRelation()`) which reads each relation for that way in turn, returning nil when there are no more relations available. Once you have accessed a relation with the iterator, you can read its tags with `way:FindInRelation(key)`. For example:

    while true do
      local rel = way:NextRelation()
      if not rel then break end
      print ("Part of route "..way:FindInRelation("ref"))
    end


### Writing relation geometries

You can also construct complete multi-linestring geometries from relations. Use this if, for example, you want a geometry for a bike or bus route that you can show at lower zoom levels, or draw with a continuous pattern.

First, make sure that you have accepted the relations using `relation_scan_function` as above.

Then write a `relation_function`, which works in the same way as `way_function` would:

    function relation_function(relation)
      if relation:Find("type")=="route" and relation:Find("route")=="bicycle" then
        relation:Layer("bike_routes", false)
        relation:Attribute("class", relation:Find("network"))
        relation:Attribute("ref", relation:Find("ref"))
      end
    end


### Not supported

Tilemaker does not yet support:

- relation roles
- nested relations
- nodes in relations
