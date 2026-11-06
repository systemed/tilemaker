#!/usr/bin/env bash

set -o errexit
set -o pipefail
set -o nounset


mkdir -p coastline
pushd coastline

if ! [ -f "water-polygons-split-4326.zip" ]; then
  if ! curl --proto '=https' --tlsv1.3 -sSfO https://osmdata.openstreetmap.de/download/water-polygons-split-4326.zip; then
    echo "Primary source osmdata.openstreetmap.de unreachable, falling back to mirror www.osmdata.org" >&2
	# explanation for these downtimes : https://community.openstreetmap.org/t/availability-of-https-osmdata-openstreetmap-de-is-it-down/149113/4
    curl --proto '=https' --tlsv1.3 -sSfO https://www.osmdata.org/download/water-polygons-split-4326.zip
  fi
fi

unzip -o -j water-polygons-split-4326.zip

popd

