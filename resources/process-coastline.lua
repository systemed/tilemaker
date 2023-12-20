-- Data processing based on openmaptiles.org schema
-- https://openmaptiles.org/schema/
-- Copyright (c) 2016, KlokanTech.com & OpenMapTiles contributors.
-- Used under CC-BY 4.0

-- Enter/exit Tilemaker
function init_function()
end
function exit_function()
end

node_keys = {}
function node_function()
end

function way_function()
end

-- Remap coastlines
function attribute_function(attr)
	return { class="ocean" }
end
