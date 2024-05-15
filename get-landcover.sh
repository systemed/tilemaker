#!/usr/bin/env bash

set -o errexit
set -o pipefail
set -o nounset


mkdir -p landcover
pushd landcover

if ! [ -f "ne_10m_antarctic_ice_shelves_polys.zip" ]; then
  curl --proto '=https' --tlsv1.3 -sSfO https://naciscdn.org/naturalearth/10m/physical/ne_10m_antarctic_ice_shelves_polys.zip
fi

if ! [ -f "ne_10m_urban_areas.zip" ]; then
  curl --proto '=https' --tlsv1.3 -sSfO https://naciscdn.org/naturalearth/10m/cultural/ne_10m_urban_areas.zip
fi

if ! [ -f "ne_10m_glaciated_areas.zip" ]; then
  curl --proto '=https' --tlsv1.3 -sSfO https://naciscdn.org/naturalearth/10m/physical/ne_10m_glaciated_areas.zip
fi

mkdir -p ne_10m_antarctic_ice_shelves_polys
unzip -o ne_10m_antarctic_ice_shelves_polys.zip -d ne_10m_antarctic_ice_shelves_polys

mkdir -p ne_10m_urban_areas
unzip -o ne_10m_urban_areas.zip -d ne_10m_urban_areas

mkdir -p ne_10m_glaciated_areas
unzip -o ne_10m_glaciated_areas.zip -d ne_10m_glaciated_areas

popd
