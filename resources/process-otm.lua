-- Data processing based on opentilemap.org schema
-- https://openmaptiles.org/schema/
-- Copyright (c) 2016, KlokanTech.com & OpenMapTiles contributors.
-- Used under CC-BY 4.0

-- Nodes will only be processed if one of these keys is present

node_keys = { "amenity", "shop", "sport", "tourism", "place" }

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
	local sport = node:Find("sport")
	local tourism = node:Find("tourism")

	if amenity~="" or shop~="" or sport~="" then
		node:Layer("poi", false)
		local rank = 10
		if amenity~="" then 
			rank = 3
			node:Attribute("class",amenity)
		elseif shop~="" then 
			node:Attribute("class",shop)
			rank = 4 
		elseif sport~="" then 
			node:Attribute("class",sport)
			rank = 5
		elseif tourism~="" then 
			node:Attribute("class",tourism)
			rank = 6
		end
		node:Attribute("name", node:Find("name"))
		node:AttributeNumeric("rank", rank)
	end

	local place = node:Find("place")
	if place ~= "" then
		local rank = 1

		if place == "continent" then rank = 1 end
		if place == "country" then rank = 1 end
		if place == "state" then rank = 1 end
		if place == "city" then rank = 2 end
		if place == "town" then rank = 3 end
		if place == "village" then rank = 4 end
		if place == "suburb" then rank = 3 end
		if place == "neighbourhood" then rank = 3 end
		if place == "locality" then rank = 4 end		
		if place == "hamlet" then rank = 4 end
		node:Layer("place", false)
		local name = node:Find("name")
		node:Attribute("name", name)
		name_en = name
		if node:Find("name:en") ~= "" then
			name_en = node:Find("name:en")
		end
		node:Attribute("name_en", name_en)
		node:AttributeNumeric("rank", rank)
		node:Attribute("class", place)
	end
end

-- Similarly for ways

function Set (list)
	local set = {}
	for _, l in ipairs(list) do set[l] = true end
	return set
end

function way_function(way)
	local highway = way:Find("highway")
	local waterway = way:Find("waterway")
	local building = way:Find("building")
	local natural = way:Find("natural")
	local landuse = way:Find("landuse")
	local leisure = way:Find("leisure")
	local amenity = way:Find("amenity")
	local aeroway = way:Find("aeroway")
	local railway = way:Find("railway")
	local isClosed = way:IsClosed()

	if highway~="" then
		way:Layer("transportation", false)
		if highway == "unclassified" then highway = "minor" end
		if highway == "residential" then highway = "minor" end

		local trackValues = Set { "cycleway", "byway", "bridleway" }
		if trackValues[highway] then highway = "track" end

		local pathValues = Set { "footway" }
		if pathValues[highway] then highway = "path" end

		local service = way:Find("service")
		if highway == "service" and service ~="" then
			way:Attribute("service", service)
		end

		way:Attribute("class", highway)
--		way:Attribute("id",way:Id())
--		way:AttributeNumeric("area",37)
	end
	if railway~="" then
		way:Layer("transportation", false)
		way:Attribute("class", railway)
	end
	if waterway~="" then
		if waterway == "riverbank" then
			way:Layer("water", isClosed)
			way:Attribute("class", "river")
		else
			way:Layer("waterway", false)
			way:Attribute("class", waterway)
		end
	end
	if building~="" then
		way:Layer("building", true)
	end
	if natural~="" then
		local landcoverkeys = Set { "wood", "landcover" }
		local landusekeys = Set { "farmland", "stuff" }

		if natural=="water" then
			local covered = way:Find("covered")
			local river = way:Find("river")
			if covered ~= "yes" then
				way:Layer("water", true)
				if river == "yes" then
					way:Attribute("class", "river")
				else
					way:Attribute("class", "lake")
				end
			end
		end
		if landcoverkeys[natural] then
			way:Layer("landcover", true)
			way:Attribute("class", natural)
		end
		if natural=="glacier" then
			way:Layer("landcover", true)
			way:Attribute("class", "ice")
		end
	end
	if landuse~="" then
		if landuse == "field" then landuse = "farmland" end
		way:Layer("landuse", true)
		way:Attribute("class", landuse)
	end
	if amenity~="" then
		local landusekeys = Set { "school", "university", "kindergarten", "college", "library", "hospital", "stadium"}
		if landusekeys[amenity] then
			way:Layer("landuse", true)
			way:Attribute("class", amenity)
		end
	end
	if leisure~="" then
		if leisure=="nature_reserve" then
			way:Layer("park", true)
			way:Attribute("class", leisure)			
		end
	end
	if aeroway~="" then
		way:Layer("aeroway", isClosed)
	end
end

