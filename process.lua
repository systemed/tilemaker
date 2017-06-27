-- Nodes will only be processed if one of these keys is present

node_keys = { "amenity", "shop" }

-- Initialize Lua logic

function init_function()
end

-- Finalize Lua logic()
function exit_function()
end

-- Assign nodes to a layer, and set attributes, based on OSM tags

function node_function(node)
	local amenity = node:Find("amenity")
	local shop = node:Find("shop")
	if amenity~="" or shop~="" then
		node:Layer("poi", false)
		if amenity~="" then node:Attribute("class",amenity)
		else node:Attribute("class",shop) end
		node:Attribute("name", node:Find("name"))
	end
end

-- Similarly for ways

function way_function(way)
	local highway = way:Find("highway")
	local waterway = way:Find("waterway")
	local building = way:Find("building")
	if highway~="" then
		way:Layer("transportation", false)
		way:Attribute("class", highway)
--		way:Attribute("id",way:Id())
--		way:AttributeNumeric("area",37)
	end
	if waterway~="" then
		way:Layer("waterway", false)
		way:Attribute("class", waterway)
	end
	if building~="" then
		way:Layer("building", true)
	end
end
