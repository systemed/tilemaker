# Relations

Tilemaker supports reading relations in the Lua process scripts. This means you can include objects like route relations when creating your vector tiles.


## Multipolygon relations

Multipolygon relations are supported natively by tilemaker; you do not need to write special Lua code for them. When a multipolygon is read, tilemaker constructs the geometry as normal, and passes the tags to `way_function` just as it would a simple area.

Boundary relations also have automatic handling of inner/outer ways, but are otherwise processed as relations. This means that you can choose to treat boundaries as properties on ways (see "Stage 2" below) or as complete geometries (see "Writing relation geometries"). You will typically want the properties-on-ways approach for administrative boundaries, and the complete-geometries approach for filled areas such as forests or nature reserves.


## Reading relation memberships

You can set your Lua script so that `way_function` is able to access the relations that the way is a member of. You would use this, for example, if you wanted to read road numbers from a relation, or to colour roads that are part of a bus route differently.

This is a two-stage process: first, when reading relations, indicate that these should be considered ("accepted"); then, when reading ways in `way_function`, you can access the relation tags.

### Stage 1: accepting relations

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

### Stage 2: accessing relations from ways and nodes

Now that you've accepted the relations, they will be available from `way_function` or `node_function`. They are accessed using an iterator (`NextRelation()`) which reads each relation for that way in turn, returning nil when there are no more relations available. Once you have accessed a relation with the iterator, you can read its tags with `FindInRelation(key)`. For example:

```lua
while true do
  local rel = NextRelation()
  if not rel then break end
  print ("Part of route "..FindInRelation("ref"))
end
```

You can obtain the relation ID and role from `rel`, which is a list (a two-element Lua table). The first element (`rel[1]`) is the id, the second (`rel[2]`) the role. Remember Lua tables are 1-indexed!

Should you need to re-read the relations, you can reset the iterator with `RestartRelations()`.


## Writing relation geometries

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


## Nested relations

In some advanced circumstances you may need to deal with nested relations - for example, routes and superroutes. tilemaker provides two ways to do this. Note that you always need to `Accept()` the parent relations in `relation_scan_function` so that they're available for processing.

### Processing nested relations in relation_function

You can read parent relations in `relation_function` in just the same way as you would for ways and nodes (see "Stage 2" above). Just iterate through them with `NextRelation` and read their tags with `FindInRelation`.

### Bouncing tags down

The three main processing functions (`way_function`, `node_function`, `relation_function`) only cope with one level of parent relations for performance reasons. So if you need to work with grandparent relations, this won't work on its own.

However, relation hierarchies aren't available in `relation_scan_function`, because not all relations have been read yet.

Instead, you can supply an additional `relation_postscan_function`. This runs immediately after `relation_scan_function`, when all accepted relations are in memory. The concept is that you read the ancestor relations, and then use this to set tags on the child relation. These tags will then be accessible when reading the relation in `way_function` etc.

For example, let's say you have a cycle route relation 8590454 (`type=route, route=bicycle, network=ncn, name=Loire à Vélo 3`). This has a parent relation 3199605 (`type=superroute, route=bicycle, network=ncn, name=Loire à Vélo`). You want to use the name from the latter. Write a relation_postscan_function like this:

```lua
function relation_postscan_function()
  while true do
    local parent,role = NextRelation()
    if not parent then break end
    -- FindInRelation reads from the parent relation
    -- SetTag sets on the child relation
    if FindInRelation("type")=="superroute" then
      local parent_name = FindInRelation("name")
      SetTag("name", parent_name)
    end
  end
end
```

This will alter the relation 8590454 to have the name from the parent superroute, "Loire à Vélo".

In case of deeply nested hierarchies, tilemaker flattens them out so that `relation_postscan_function` can iterate through all parents/grandparents/etc. with `NextRelation`. The hierarchy is not preserved.
