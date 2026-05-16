#!/usr/bin/env python3

import gzip
import hashlib
import json
import pathlib
import sqlite3
import struct
import subprocess
import sys
import zlib


COMPRESSION_NONE = 1
COMPRESSION_GZIP = 2


status = 0


class ArchiveContent:
    def __init__(self, raw_hash, raw_tiles, metadata_hash):
        self.raw_hash = raw_hash
        self.raw_tiles = raw_tiles
        self.metadata_hash = metadata_hash
        self.semantic_tiles = {}
        self.layer_counts = {}


def error(path, message):
    global status
    label = archive_label(path)
    print(f"::error title={label}::{label}: {message}")
    status = 1


def notice(path, message):
    label = archive_label(path)
    print(f"::notice title={label}::{label}: {message}")


def archive_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def build_label(path):
    artifact = path.parent.name
    if artifact == "tile-outputs-github-action":
        return "GitHub Action"
    if not artifact.startswith("tile-outputs-"):
        return str(path.parent)

    label = artifact.removeprefix("tile-outputs-")
    if label.endswith("-cmake"):
        return f"{runner_label(label.removesuffix('-cmake'))} (CMake)"
    if label.endswith("-makefile"):
        return f"{runner_label(label.removesuffix('-makefile'))} (Makefile)"
    return artifact


def runner_label(label):
    if label == "windows":
        return "Windows"
    if label.startswith("ubuntu-"):
        return "Ubuntu " + label.removeprefix("ubuntu-")
    if label.startswith("macos-"):
        return "macOS " + label.removeprefix("macos-")
    return label.replace("-", " ")


def archive_label(path):
    return f"{build_label(path)} {path.name}"


def decompress_payload(data):
    for wbits in (16 + zlib.MAX_WBITS, zlib.MAX_WBITS):
        try:
            return zlib.decompress(data, wbits)
        except zlib.error:
            pass
    return data


def decompress_pmtiles(data, compression):
    if compression == COMPRESSION_NONE:
        return data
    if compression == COMPRESSION_GZIP:
        return gzip.decompress(data)
    raise RuntimeError(f"unsupported PMTiles compression {compression}")


def tileid_to_zxy(tileid):
    acc = 0
    for z in range(32):
        num_tiles = (1 << z) * (1 << z)
        if acc + num_tiles > tileid:
            return t_on_level(z, tileid - acc)
        acc += num_tiles
    raise RuntimeError("tile zoom exceeds 64-bit limit")


def t_on_level(z, pos):
    n = 1 << z
    t = pos
    x = 0
    y = 0
    s = 1
    while s < n:
        rx = 1 & (t // 2)
        ry = 1 & (t ^ rx)
        if ry == 0:
            if rx == 1:
                x = s - 1 - x
                y = s - 1 - y
            x, y = y, x
        x += s * rx
        y += s * ry
        t //= 4
        s *= 2
    return z, x, y


def decode_varint(data, pos):
    value = 0
    shift = 0
    for _ in range(10):
        if pos >= len(data):
            raise RuntimeError("end of buffer while reading varint")
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7f) << shift
        if byte < 0x80:
            return value, pos
        shift += 7
    raise RuntimeError("varint too long")


def deserialize_directory(data):
    pos = 0
    count, pos = decode_varint(data, pos)
    entries = [{"tile_id": 0, "run_length": 0, "length": 0, "offset": 0} for _ in range(count)]
    last_id = 0
    for entry in entries:
        value, pos = decode_varint(data, pos)
        entry["tile_id"] = last_id + value
        last_id = entry["tile_id"]
    for entry in entries:
        entry["run_length"], pos = decode_varint(data, pos)
    for entry in entries:
        entry["length"], pos = decode_varint(data, pos)
    for index, entry in enumerate(entries):
        value, pos = decode_varint(data, pos)
        if index > 0 and value == 0:
            prev = entries[index - 1]
            entry["offset"] = prev["offset"] + prev["length"]
        else:
            entry["offset"] = value - 1
    if pos != len(data):
        raise RuntimeError("trailing bytes in PMTiles directory")
    return entries


def zigzag_decode(value):
    return (value >> 1) ^ -(value & 1)


def protobuf_fields(data):
    pos = 0
    while pos < len(data):
        tag, pos = decode_varint(data, pos)
        field = tag >> 3
        wire_type = tag & 7
        if wire_type == 0:
            value, pos = decode_varint(data, pos)
            yield field, wire_type, value
        elif wire_type == 1:
            if pos + 8 > len(data):
                raise RuntimeError("end of buffer while reading fixed64")
            yield field, wire_type, data[pos:pos + 8]
            pos += 8
        elif wire_type == 2:
            length, pos = decode_varint(data, pos)
            if pos + length > len(data):
                raise RuntimeError("end of buffer while reading length-delimited field")
            yield field, wire_type, data[pos:pos + length]
            pos += length
        elif wire_type == 5:
            if pos + 4 > len(data):
                raise RuntimeError("end of buffer while reading fixed32")
            yield field, wire_type, data[pos:pos + 4]
            pos += 4
        else:
            raise RuntimeError(f"unsupported protobuf wire type {wire_type}")


def packed_varints(data):
    pos = 0
    values = []
    while pos < len(data):
        value, pos = decode_varint(data, pos)
        values.append(value)
    return values


def decode_geometry(geometry):
    pos = 0
    x = 0
    y = 0
    paths = []
    current = []

    while pos < len(geometry):
        command = geometry[pos] & 7
        count = geometry[pos] >> 3
        pos += 1

        if command == 1:
            for _ in range(count):
                if current:
                    paths.append(current)
                current = []
                x += zigzag_decode(geometry[pos])
                y += zigzag_decode(geometry[pos + 1])
                pos += 2
                current.append([x, y])
        elif command == 2:
            for _ in range(count):
                if not current:
                    raise RuntimeError("MVT LineTo command without an active path")
                x += zigzag_decode(geometry[pos])
                y += zigzag_decode(geometry[pos + 1])
                pos += 2
                current.append([x, y])
        elif command == 7:
            for _ in range(count):
                if current:
                    paths.append(current)
                    current = []
        else:
            raise RuntimeError(f"unsupported MVT geometry command {command}")

    if current:
        paths.append(current)
    return paths


def canonical_geometry(geometry_type, geometry):
    paths = decode_geometry(geometry)
    if geometry_type == 1:
        points = [point for path in paths for point in path]
        points.sort(key=canonical_json)
        return ["points", points]
    if geometry_type == 3:
        return ["polygons", canonical_polygons(paths)]
    return ["geometry", paths]


def canonical_polygons(paths):
    polygons = []
    current = None
    outer_area = 0

    for path in paths:
        area = ring_area(path)
        ring = canonical_ring(path, area)

        if area == 0:
            polygons.append(["degenerate", ring])
        elif outer_area == 0 or area == outer_area:
            if current:
                current[1].sort(key=canonical_json)
                polygons.append(current)
            outer_area = area
            current = [ring, []]
        elif current:
            current[1].append(ring)
        else:
            polygons.append(["orphan", ring])

    if current:
        current[1].sort(key=canonical_json)
        polygons.append(current)

    polygons.sort(key=canonical_json)
    return polygons


def canonical_ring(path, area):
    ring = path
    if len(ring) > 1 and ring[0] == ring[-1]:
        ring = ring[:-1]
    if not ring:
        return [0, []]

    rotations = [ring[index:] + ring[:index] for index in range(len(ring))]
    return [area, min(rotations, key=canonical_json)]


def ring_area(ring):
    area = 0
    for index, point in enumerate(ring):
        next_point = ring[(index + 1) % len(ring)]
        area += point[0] * next_point[1] - next_point[0] * point[1]
    if area < 0:
        return -1
    if area > 0:
        return 1
    return 0


def decode_mvt_value(data):
    values = []
    for field, wire_type, value in protobuf_fields(data):
        if field == 1:
            values.append(["string", value.decode("utf-8", "replace")])
        elif field == 2:
            values.append(["float", struct.unpack("<f", value)[0]])
        elif field == 3:
            values.append(["double", struct.unpack("<d", value)[0]])
        elif field == 4:
            values.append(["int", value])
        elif field == 5:
            values.append(["uint", value])
        elif field == 6:
            values.append(["sint", zigzag_decode(value)])
        elif field == 7:
            values.append(["bool", bool(value)])
        else:
            values.append([f"field{field}", wire_type, value if isinstance(value, int) else value.hex()])
    return values


def decode_mvt_feature(data, keys, values):
    feature_id = None
    tags = []
    geometry_type = None
    geometry = []
    for field, wire_type, value in protobuf_fields(data):
        if field == 1:
            feature_id = value
        elif field == 2:
            indexes = packed_varints(value)
            if len(indexes) % 2 != 0:
                raise RuntimeError("MVT feature has an odd number of tag indexes")
            for index in range(0, len(indexes), 2):
                key_index = indexes[index]
                value_index = indexes[index + 1]
                if key_index >= len(keys) or value_index >= len(values):
                    raise RuntimeError("MVT feature tag index is out of range")
                tags.append([keys[key_index], values[value_index]])
        elif field == 3:
            geometry_type = value
        elif field == 4:
            geometry = packed_varints(value)
    tags.sort(key=canonical_json)
    geometry = canonical_geometry(geometry_type, geometry)
    return [feature_id, geometry_type, tags, geometry]


def decode_mvt_layer(data):
    name = None
    features = []
    keys = []
    values = []
    extent = None
    version = None
    for field, wire_type, value in protobuf_fields(data):
        if field == 1:
            name = value.decode("utf-8", "replace")
        elif field == 2:
            features.append(value)
        elif field == 3:
            keys.append(value.decode("utf-8", "replace"))
        elif field == 4:
            values.append(decode_mvt_value(value))
        elif field == 5:
            extent = value
        elif field == 15:
            version = value

    decoded_features = [decode_mvt_feature(feature, keys, values) for feature in features]
    decoded_features.sort(key=canonical_json)
    return [name, version, extent, decoded_features]


def canonical_mvt(data):
    layers = []
    for field, wire_type, value in protobuf_fields(data):
        if field == 3:
            layers.append(decode_mvt_layer(value))
    layers.sort(key=canonical_json)
    return layers


def canonical_json(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def semantic_tile_content(data):
    tile = canonical_mvt(data)
    digest = hashlib.sha256(canonical_json(tile).encode("utf-8", "surrogateescape")).hexdigest()
    layer_counts = {layer[0]: len(layer[3]) for layer in tile}
    return digest, layer_counts


def content_summary(content):
    return f"raw {content.raw_hash}"


def add_tile_to_raw_digest(raw_digest, raw_tiles, z, x, y, payload):
    tile = (z, x, y)
    raw_hash = hashlib.sha256(payload).hexdigest()
    raw_tiles[tile] = raw_hash

    raw_digest.update(f"T\t{z}\t{x}\t{y}\t{len(payload)}\t{raw_hash}\n".encode())


def add_tile_to_semantic_maps(content, z, x, y, payload):
    tile = (z, x, y)
    semantic_hash, counts = semantic_tile_content(payload)
    content.semantic_tiles[tile] = semantic_hash
    content.layer_counts[tile] = counts


def mbtiles_fingerprint(path):
    con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    cur = con.cursor()
    tables = {row[0] for row in cur.execute("select name from sqlite_master where type = 'table'")}
    missing = {"metadata", "tiles"} - tables
    if missing:
        raise RuntimeError("missing tables: " + ", ".join(sorted(missing)))
    tile_count = cur.execute("select count(*) from tiles").fetchone()[0]
    if tile_count == 0:
        raise RuntimeError("tiles table is empty")
    empty_tiles = cur.execute("select count(*) from tiles where tile_data is null or length(tile_data) = 0").fetchone()[0]
    if empty_tiles:
        raise RuntimeError(f"{empty_tiles} empty tile blobs")
    minzoom, maxzoom = cur.execute("select min(zoom_level), max(zoom_level) from tiles").fetchone()
    metadata_rows = list(cur.execute("select name, value from metadata order by name, value"))
    if not metadata_rows:
        raise RuntimeError("metadata table is empty")

    raw_digest = hashlib.sha256()
    metadata_digest = hashlib.sha256()
    for name, value in metadata_rows:
        metadata = f"M\t{name}\t{canonical_metadata_value(value)}\n".encode("utf-8", "surrogateescape")
        raw_digest.update(metadata)
        metadata_digest.update(metadata)

    raw_tiles = {}
    for z, x, y, tile_data in cur.execute("select zoom_level, tile_column, tile_row, tile_data from tiles order by zoom_level, tile_column, tile_row"):
        payload = decompress_payload(bytes(tile_data))
        add_tile_to_raw_digest(raw_digest, raw_tiles, z, x, y, payload)
    con.close()

    content = ArchiveContent(raw_digest.hexdigest(), raw_tiles, metadata_digest.hexdigest())
    print(
        f"{archive_label(path)}: {tile_count} tiles, zoom {minzoom}-{maxzoom}, "
        f"{len(metadata_rows)} metadata rows, raw {content.raw_hash}"
    )
    return content


def mbtiles_decode_semantic_tiles(path, content, tiles):
    con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    cur = con.cursor()
    for z, x, y in sorted(tiles):
        row = cur.execute(
            "select tile_data from tiles where zoom_level=? and tile_column=? and tile_row=?",
            (z, x, y),
        ).fetchone()
        if row is None:
            continue
        tile_data = row[0]
        payload = decompress_payload(bytes(tile_data))
        add_tile_to_semantic_maps(content, z, x, y, payload)
    con.close()


def canonical_metadata_value(value):
    if value is None:
        return ""
    try:
        return json.dumps(json.loads(value), sort_keys=True, separators=(",", ":"))
    except (TypeError, json.JSONDecodeError):
        return str(value)


def verify_pmtiles(path):
    print(f"{archive_label(path)}: verifying PMTiles archive")
    result = subprocess.run(
        ["pmtiles", "verify", str(path)],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="")
    if result.returncode != 0:
        raise RuntimeError(pmtiles_failure_message(result))


def pmtiles_failure_message(result):
    lines = [
        line.strip()
        for line in (result.stdout + "\n" + result.stderr).splitlines()
        if line.strip()
    ]
    for line in reversed(lines):
        pos = line.find("Failed to verify archive")
        if pos >= 0:
            return line[pos:]
    if lines:
        return lines[-1]
    return "pmtiles verify failed"


def pmtiles_header(data):
    if len(data) < 127:
        raise RuntimeError("PMTiles header is truncated")
    if data[:7] != b"PMTiles" or data[7] != 3:
        raise RuntimeError("not a PMTiles v3 archive")
    values = struct.unpack_from("<QQQQQQQQQQQBBBBBBiiiiBii", data, 8)
    return {
        "root_dir_offset": values[0],
        "root_dir_bytes": values[1],
        "json_metadata_offset": values[2],
        "json_metadata_bytes": values[3],
        "leaf_dirs_offset": values[4],
        "leaf_dirs_bytes": values[5],
        "tile_data_offset": values[6],
        "tile_data_bytes": values[7],
        "addressed_tiles_count": values[8],
        "tile_entries_count": values[9],
        "tile_contents_count": values[10],
        "internal_compression": values[12],
        "tile_compression": values[13],
    }


def collect_pmtiles_entries(data, header, offset, length, result):
    directory = decompress_pmtiles(data[offset:offset + length], header["internal_compression"])
    for entry in deserialize_directory(directory):
        if entry["run_length"] == 0:
            collect_pmtiles_entries(data, header, header["leaf_dirs_offset"] + entry["offset"], entry["length"], result)
        else:
            for tileid in range(entry["tile_id"], entry["tile_id"] + entry["run_length"]):
                z, x, y = tileid_to_zxy(tileid)
                result.append((z, x, y, header["tile_data_offset"] + entry["offset"], entry["length"]))


def pmtiles_fingerprint(path):
    verify_pmtiles(path)
    data = path.read_bytes()
    header = pmtiles_header(data)
    metadata = decompress_pmtiles(
        data[header["json_metadata_offset"]:header["json_metadata_offset"] + header["json_metadata_bytes"]],
        header["internal_compression"],
    )
    try:
        metadata = json.dumps(json.loads(metadata), sort_keys=True, separators=(",", ":")).encode()
    except json.JSONDecodeError:
        pass

    entries = []
    collect_pmtiles_entries(data, header, header["root_dir_offset"], header["root_dir_bytes"], entries)
    raw_digest = hashlib.sha256()
    metadata_hash = hashlib.sha256(metadata).hexdigest()
    raw_digest.update(f"M\t{metadata_hash}\n".encode())

    raw_tiles = {}
    for z, x, y, offset, length in sorted(entries):
        payload = decompress_pmtiles(data[offset:offset + length], header["tile_compression"])
        add_tile_to_raw_digest(raw_digest, raw_tiles, z, x, y, payload)

    content = ArchiveContent(raw_digest.hexdigest(), raw_tiles, metadata_hash)
    print(
        f"{archive_label(path)}: {len(entries)} addressed tiles, "
        f"raw {content.raw_hash}"
    )
    return content


def pmtiles_decode_semantic_tiles(path, content, tiles):
    data = path.read_bytes()
    header = pmtiles_header(data)
    entries = []
    collect_pmtiles_entries(data, header, header["root_dir_offset"], header["root_dir_bytes"], entries)
    wanted = set(tiles)
    for z, x, y, offset, length in sorted(entries):
        if (z, x, y) not in wanted:
            continue
        payload = decompress_pmtiles(data[offset:offset + length], header["tile_compression"])
        add_tile_to_semantic_maps(content, z, x, y, payload)


def fingerprint_archives(paths, fingerprint, failure_message):
    cache = {}
    contents = {}
    archive_hashes = {}

    for path in paths:
        archive_hashes[path] = archive_sha256(path)
        print(f"{archive_hashes[path]}  {archive_label(path)}")

    for path in paths:
        archive_hash = archive_hashes[path]
        if archive_hash not in cache:
            try:
                cache[archive_hash] = (path, fingerprint(path), None)
            except Exception as err:
                cache[archive_hash] = (path, None, str(err))

        source_path, content_hash, failure = cache[archive_hash]
        if failure is not None:
            error(path, f"{failure_message}: {failure}")
            continue

        contents[path] = content_hash
        if source_path != path:
            print(
                f"{archive_label(path)}: same archive as {archive_label(source_path)}; "
                f"{content_summary(content_hash)}"
            )

    return contents


def tile_label(tile):
    z, x, y = tile
    return f"z{z}/{x}/{y}"


def tile_difference(left, right, semantic):
    left_tiles = left.semantic_tiles if semantic else left.raw_tiles
    right_tiles = right.semantic_tiles if semantic else right.raw_tiles
    for tile in sorted(set(left_tiles) | set(right_tiles)):
        if tile not in left_tiles:
            return f"{tile_label(tile)} is missing from output"
        if tile not in right_tiles:
            return f"{tile_label(tile)} is missing from comparison output"
        if left_tiles[tile] != right_tiles[tile]:
            if semantic:
                return semantic_tile_difference(left, right, tile)
            return f"first byte-level difference at {tile_label(tile)}"
    return "content differs"


def semantic_tile_difference(left, right, tile):
    left_counts = left.layer_counts.get(tile, {})
    right_counts = right.layer_counts.get(tile, {})
    for layer in sorted(set(left_counts) | set(right_counts)):
        left_count = left_counts.get(layer, 0)
        right_count = right_counts.get(layer, 0)
        if left_count != right_count:
            return (
                f"first semantic difference at {tile_label(tile)}: "
                f"layer {layer} has {left_count} vs {right_count} features"
            )
    return f"first semantic difference at {tile_label(tile)}"


def raw_differing_tiles(left, right):
    differing = []
    for tile in sorted(set(left.raw_tiles) | set(right.raw_tiles)):
        if left.raw_tiles.get(tile) != right.raw_tiles.get(tile):
            differing.append(tile)
    return differing


def ensure_semantic_tiles(path, content, tiles, semantic_decoder, failure_message):
    needed = [tile for tile in tiles if tile in content.raw_tiles and tile not in content.semantic_tiles]
    if not needed:
        return True
    try:
        semantic_decoder(path, content, needed)
    except Exception as err:
        error(path, f"{failure_message}: {err}")
        return False

    return True


def compare_content(path, content, other_path, other_content, relation, semantic_decoder, failure_message):
    if content.raw_hash == other_content.raw_hash:
        return

    if content.metadata_hash != other_content.metadata_hash:
        error(path, f"metadata differs from {relation} {archive_label(other_path)}")
        return

    differing_tiles = raw_differing_tiles(content, other_content)

    for tile in differing_tiles:
        if not ensure_semantic_tiles(path, content, [tile], semantic_decoder, failure_message):
            return

        if not ensure_semantic_tiles(other_path, other_content, [tile], semantic_decoder, failure_message):
            return

        if content.semantic_tiles.get(tile) != other_content.semantic_tiles.get(tile):
            error(
                path,
                f"semantic content differs from {relation} {archive_label(other_path)}; "
                f"{tile_difference(content, other_content, True)}",
            )
            return

    notice(
        path,
        f"raw tile bytes differ from {relation} {archive_label(other_path)}, "
        f"but semantic content matches; {tile_difference(content, other_content, False)}",
    )


def check_repeat(contents, suffix, semantic_decoder, failure_message):
    for path, content in sorted(contents.items()):
        if path.name.endswith(f"-repeat.{suffix}"):
            continue
        repeat = path.with_name(f"{path.stem}-repeat.{suffix}")
        if repeat not in contents:
            error(path, f"missing repeat output {archive_label(repeat)}")
        else:
            compare_content(path, content, repeat, contents[repeat], "repeat output", semantic_decoder, failure_message)


def check_cross_runner(contents, suffix, semantic_decoder, failure_message):
    groups = {}
    for path, content in contents.items():
        if path.name.endswith(f"-repeat.{suffix}"):
            continue
        if path.parent.name == "tile-outputs-github-action":
            continue
        groups.setdefault(path.name, []).append((path, content))
    for name, values in sorted(groups.items()):
        reference_path, reference = values[0]
        for path, content in values[1:]:
            compare_content(path, content, reference_path, reference, "output", semantic_decoder, failure_message)


def main():
    root = pathlib.Path(sys.argv[1])
    mbtiles_paths = sorted(root.glob("**/*.mbtiles"))
    pmtiles_paths = sorted(root.glob("**/*.pmtiles"))

    print("Archive SHA-256")
    mbtiles = fingerprint_archives(
        mbtiles_paths,
        mbtiles_fingerprint,
        "MBTiles archive failed verification",
    )

    pmtiles = fingerprint_archives(
        pmtiles_paths,
        pmtiles_fingerprint,
        "PMTiles archive failed verification",
    )

    check_repeat(mbtiles, "mbtiles", mbtiles_decode_semantic_tiles, "MBTiles archive failed verification")
    check_repeat(pmtiles, "pmtiles", pmtiles_decode_semantic_tiles, "PMTiles archive failed verification")
    check_cross_runner(mbtiles, "mbtiles", mbtiles_decode_semantic_tiles, "MBTiles archive failed verification")
    check_cross_runner(pmtiles, "pmtiles", pmtiles_decode_semantic_tiles, "PMTiles archive failed verification")
    return status


if __name__ == "__main__":
    sys.exit(main())
