#!/usr/bin/env bash

set -o errexit
set -o pipefail
set -o nounset


mkdir -p coastline
pushd coastline

if ! [ -f "water-polygons-split-4326.zip" ]; then
  curl --proto '=https' --tlsv1.3 -sSfO https://osmdata.openstreetmap.de/download/water-polygons-split-4326.zip
fi

unzip -o -j water-polygons-split-4326.zip

popd
