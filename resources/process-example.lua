--[[

	A simple example tilemaker configuration, intended to illustrate how it
	works and to act as a starting point for your own configurations.

	The basic principle is:
	- read OSM tags with Find(key)
	- write to vector tile layers with Layer(layer_name)
	- add attributes with Attribute(field, value)

	(This is a very basic subset of the OpenMapTiles schema. Don't take much 
	notice of the "class" attribute, that's an OMT implementation thing which 
	is just here to get them to show up with the default style.)

	It doesn't do much filtering by zoom level - all the roads appear all the
	time. If you want a practice project, try fixing that!

	You can view your output with tilemaker-server:

	tilemaker-server /path/to/your.mbtiles --static server/static

]]--


-- Nodes will only be processed if one of these keys is present

node_keys = { "amenity", "historic", "leisure", "place", "shop", "tourism" }


-- Assign nodes to a layer, and set attributes, based on OSM tags

function node_function(node)
	-- POIs go to a "poi" layer (we just look for amenity and shop here)
	local amenity = Find("amenity")
	local shop = Find("shop")
	if amenity~="" or shop~="" then
		Layer("poi")
		if amenity~="" then Attribute("class",amenity)
		else Attribute("class",shop) end
		Attribute("name:latin", Find("name"))
		AttributeNumeric("rank", 3)
	end
	
	-- Places go to a "place" layer
	local place = Find("place")
	if place~="" then
		Layer("place")
		Attribute("class", place)
		Attribute("name:latin", Find("name"))
		if place=="city" then
			AttributeNumeric("rank", 4)
			MinZoom(3)
		elseif place=="town" then
			AttributeNumeric("rank", 6)
			MinZoom(6)
		else
			AttributeNumeric("rank", 9)
			MinZoom(10)
		end
	end
end


-- Assign ways to a layer, and set attributes, based on OSM tags

function way_function()
	local highway  = Find("highway")
	local waterway = Find("waterway")
	local building = Find("building")

	-- Roads
	if highway~="" then
		Layer("transportation", false)
		if highway=="unclassified" or highway=="residential" then highway="minor" end
		Attribute("class", highway)
		-- ...and road names
		local name = Find("name")
		if name~="" then
			Layer("transportation_name", false)
			Attribute("class", highway)
			Attribute("name:latin", name)
		end
	end

	-- Rivers
	if waterway=="stream" or waterway=="river" or waterway=="canal" then
		Layer("waterway", false)
		Attribute("class", waterway)
		AttributeNumeric("intermittent", 0)
	end

	-- Lakes and other water polygons
	if Find("natural")=="water" then
		Layer("water", true)
		if Find("water")=="river" then
			Attribute("class", "river")
		else
			Attribute("class", "lake")
		end
	end
	-- Buildings
	if building~="" then
		Layer("building", true)
	end
end
