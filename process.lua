-- Nodes will only be processed if one of these keys is present

node_keys = { "amenity", "shop" }

-- Assign nodes to a layer, and set attributes, based on OSM tags

function node_function(node)
	local amenity = node:Find("amenity")
	local shop = node:Find("shop")
	if amenity~="" or shop~="" then
		node:Layer("pois", false)
		if amenity~="" then node:Attribute("type",amenity)
		else node:Attribute("type",shop) end
		node:Attribute("name", node:Find("name"))
	end
end

-- Similarly for ways

function way_function(way)
	local highway = way:Find("highway")
	local waterway = way:Find("waterway")
	local building = way:Find("building")
	if highway~="" then
		way:Layer("roads", false)
		way:Attribute("name", way:Find("name"))
		way:Attribute("type",highway)
--		way:Attribute("id",way:Id())
--		way:AttributeNumeric("area",37)
	end
	if waterway~="" then
		way:Layer("water", false)
	end
	if building~="" then
		way:Layer("buildings", true)
	end
end
