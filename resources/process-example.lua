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
	local amenity = Find("amenity")
	local shop = Find("shop")
	if amenity~="" or shop~="" then
		Layer("poi", false)
		if amenity~="" then Attribute("class",amenity)
		else Attribute("class",shop) end
		Attribute("name", Find("name"))
	end
end

-- Similarly for ways

function way_function()
	local highway = Find("highway")
	local waterway = Find("waterway")
	local building = Find("building")
	if highway~="" then
		Layer("transportation", false)
		Attribute("class", highway)
--		Attribute("id",Id())
--		AttributeNumeric("area",37)
	end
	if waterway~="" then
		Layer("waterway", false)
		Attribute("class", waterway)
	end
	if building~="" then
		Layer("building", true)
	end
end
