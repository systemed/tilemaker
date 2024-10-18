#!/usr/bin/env bash

set -o errexit
set -o pipefail
set -o nounset


if ! [ -f "monaco-latest.osm.pbf" ]; then
  curl --proto '=https' --tlsv1.3 -sSfO https://download.geofabrik.de/europe/monaco-latest.osm.pbf
fi
