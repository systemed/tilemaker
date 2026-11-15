/*! \file */
#ifndef _MEMO_KEYS_H
#define _MEMO_KEYS_H

// The way_function memo's walk (see OsmLuaProcessing::runWayFunction) without TagMap lookups.
//
// The memo tree reads few distinct keys (tens), but each way is tested against ~50 of them, and
// most are absent. So the keys the tree reads get small ids (MemoKeyTable), the way's tags are
// looked up once each (MemoTagSlots: which ids are present, with their values), and the walk runs
// on a compact copy of the tree (MemoHotNode: kind, key id and the children of the common results).
// The results are those of TagMap::getKey / getValueFromKey: a key matches by its exact bytes and a
// key added twice has its last value.

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

class MemoKeyTable {
public:
	static constexpr uint32_t NONE = UINT32_MAX;

	uint32_t find(const char* key, size_t size) const {
		if (slots.empty()) return NONE;
		const uint64_t p = prefixOf(key, size);
		const size_t mask = slots.size() - 1;
		for (size_t i = hashOf(p, size) & mask; ; i = (i + 1) & mask) {
			const Slot& s = slots[i];
			if (s.id == NONE) return NONE;
			if (s.prefix == p && s.size == size && (size <= 8 || memcmp(names[s.id].data() + 8, key + 8, size - 8) == 0))
				return s.id;
		}
	}

	// The id of a key, given a new one if it hasn't one yet
	uint32_t add(std::string_view key) {
		uint32_t id = find(key.data(), key.size());
		if (id != NONE) return id;
		id = names.size();
		names.emplace_back(key);
		if ((names.size() + 1) * 4 > slots.size()) rehash(slots.empty() ? 64 : slots.size() * 2);
		else place(id);
		return id;
	}

	size_t size() const { return names.size(); }
	// Per id (kept up to date by the owner, see OsmLuaProcessing::runWayFunction): a key whose
	// value the tree only tests for being empty (way_memo_values)
	std::vector<uint8_t> valueOnly;
	const std::string& name(uint32_t id) const { return names[id]; }

private:
	struct Slot { uint64_t prefix; uint32_t size; uint32_t id; };
	std::vector<std::string> names;
	std::vector<Slot> slots;	// open addressing, at most a quarter full

	// The first (up to) 8 bytes of a key as an integer
	static uint64_t prefixOf(const char* key, size_t size) {
		uint64_t p = 0;
		if (size >= 8) memcpy(&p, key, 8);
		else if (size >= 4) {
			// bytes 0..3 and size-4..size-1 (the overlap has the same bytes in both)
			uint32_t lo, hi;
			memcpy(&lo, key, 4); memcpy(&hi, key + size - 4, 4);
			p = uint64_t(lo) | uint64_t(hi) << (8 * (size - 4));
		} else if (size > 0) {
			const auto b = [&](size_t i) { return uint64_t(static_cast<unsigned char>(key[i])) << (8 * i); };
			p = b(0) | b(size / 2) | b(size - 1);
		}
		return p;
	}
	static size_t hashOf(uint64_t prefix, size_t size) {
		const uint64_t h = (prefix ^ (uint64_t(size) << 56)) * 0x9e3779b97f4a7c15ull;
		return h >> 40;
	}
	void place(uint32_t id) {
		const std::string& k = names[id];
		const uint64_t p = prefixOf(k.data(), k.size());
		const size_t mask = slots.size() - 1;
		size_t i = hashOf(p, k.size()) & mask;
		while (slots[i].id != NONE) i = (i + 1) & mask;
		slots[i] = Slot{ p, uint32_t(k.size()), id };
	}
	void rehash(size_t n) {
		slots.assign(n, Slot{ 0, 0, NONE });
		for (uint32_t id = 0; id < names.size(); id++) place(id);
	}
};

// The current object's tags with a key of the table: present or not, and the value
class MemoTagSlots {
public:
	// Look up the tags (as added: a later one replaces an earlier one of the same key), and make
	// the object's signature for MemoSigCache: `flags`, then per tag with a key of the table (as
	// added) its id and value (valueOnly keys: whether it is empty). Equal signatures mean equal
	// tags as the walk sees them. Returns its length, 0 if too long for the cache.
	template<class Tags>
	size_t fill(const MemoKeyTable& table, const Tags& tags, uint8_t flags, char* sig, size_t sigMax) {
		if (stamp.size() < table.size()) { stamp.resize(table.size() + 16, 0); value.resize(table.size() + 16); }
		if (++gen == 0) { std::fill(stamp.begin(), stamp.end(), 0); gen = 1; }
		present = 1ull << 63;
		size_t len = 0;
		sig[len++] = flags;
		for (const auto& tag : tags) {
			const uint32_t id = table.find(tag.key.data(), tag.key.size());
			if (id == MemoKeyTable::NONE) continue;
			stamp[id] = gen;
			value[id] = std::string_view(tag.value.data(), tag.value.size());
			present |= keyBit(id);
			if (len == 0) continue;	// too long already
			const size_t vlen = tag.value.size();
			const bool valueOnly = table.valueOnly[id];
			// id (1 or 2 bytes), then 1 byte (valueOnly: empty or not), else the length and the bytes
			const size_t need = (id < 128 ? 1 : 2) + 1 + (valueOnly ? 0 : vlen);
			if (id >= 16384 || vlen >= 256 || len + need > sigMax) { len = 0; continue; }
			if (id < 128) sig[len++] = char(id);
			else { sig[len++] = char(0x80 | (id >> 8)); sig[len++] = char(id & 0xff); }
			if (valueOnly) sig[len++] = char(vlen != 0);
			else { sig[len++] = char(vlen); memcpy(sig + len, tag.value.data(), vlen); len += vlen; }
		}
		return len;
	}
	// A bit per key id (ids share bits: a clear bit means all its keys are absent); bit 63 always set
	static uint64_t keyBit(uint32_t id) { return 1ull << (id % 63); }
	uint64_t present = 0;
	// (ids the table got after fill(): absent)
	bool has(uint32_t id) const { return id < stamp.size() && stamp[id] == gen; }
	std::string_view get(uint32_t id) const { return has(id) ? value[id] : std::string_view(); }

private:
	uint32_t gen = 0;
	std::vector<uint32_t> stamp;	// == gen: present
	std::vector<std::string_view> value;
};

// A direct-mapped cache of recent signatures (short byte strings) -> 32-bit values. Used for
// - the walk: the node it reached (a Leaf or Lua node) per object signature (MemoTagSlots::fill).
//   The walk is a function of the tags it can read, IsClosed and the relations, and objects in a
//   row are often alike (buildings, streets of a town). An edge of the tree never changes once
//   added, so a path once complete keeps ending at the same node while nodes are added;
//   memoCompact renumbers the nodes (clear()).
// - the AttributeIndex of a replayed segment with way_memo_values attributes, per segment and
//   values (see OsmLuaProcessing::memoSegmentSig)
class MemoSigCache {
public:
	static constexpr size_t SIG_MAX = 52;
	explicit MemoSigCache(size_t size) : entries(size) {}	// size: a power of 2
	static uint64_t hashOf(const char* sig, size_t len) {
		uint64_t h = len * 0x9e3779b97f4a7c15ull;
		size_t i = 0;
		for (; i + 8 <= len; i += 8) { uint64_t w; memcpy(&w, sig + i, 8); h = (h ^ w) * 0xff51afd7ed558ccdull; h ^= h >> 29; }
		uint64_t w = 0;
		for (size_t k = 0; i < len; i++, k++) w |= uint64_t(static_cast<unsigned char>(sig[i])) << (8 * k);
		h = (h ^ w) * 0xc4ceb9fe1a85ec53ull; h ^= h >> 32;
		return h;
	}
	uint32_t find(uint64_t hash, const char* sig, size_t len) const {
		const Entry& e = entries[hash & (entries.size() - 1)];
		return e.len == len && e.hash == hash && memcmp(e.sig, sig, len) == 0 ? e.node : UINT32_MAX;
	}
	void put(uint64_t hash, const char* sig, size_t len, uint32_t node) {
		Entry& e = entries[hash & (entries.size() - 1)];
		e.hash = hash; e.node = node; e.len = len; memcpy(e.sig, sig, len);
	}
	void clear() { for (Entry& e : entries) e.len = 0; }
private:
	struct Entry { uint64_t hash = 0; uint32_t node = 0; uint16_t len = 0; char sig[SIG_MAX]; };	// len 0: empty
	std::vector<Entry> entries;
};

#ifndef MEMO_LEVELS
#define MEMO_LEVELS 2
#endif
#ifndef MEMO_FAN
#define MEMO_FAN 4
#endif

// A memo tree node in compact form: what the walk needs, for the results "", "0", "1" and "\x01"
// (the other results: the full node's child())
// Jumps: from a read node, the walk of a way that has none of the keys along the next (up to) 4
// resp. 16 reads takes their "absent" children ("" for Find, "0" otherwise); jump[l] is where it
// ends up, mask[l] the bits of those keys (IsClosed/NextRelation: CLOSED_BIT/RELATION_BIT).
// A node without a jump has mask bit 63, which `present` always has.
struct MemoHotNode {
	uint8_t kind = 0;	// OsmLuaProcessing::MemoNode::Kind
	bool valueOnly = false;	// a tag of way_memo_values: branches on empty / not empty only
	bool overflow = false;	// too many results: the others run through Lua
	uint16_t children = 0;	// number of children (all results)
	uint32_t key = MemoKeyTable::NONE;	// Find, Holds: the key's id
	uint32_t quick[4] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };	// children for "", "0", "1", "\x01"
	uint32_t more = UINT32_MAX;	// the node's other children / its ops (Leaf): index in the owner's side table
	uint32_t jump[MEMO_LEVELS];
	uint64_t mask[MEMO_LEVELS];
	MemoHotNode() { for (int l = 0; l < MEMO_LEVELS; l++) { jump[l] = 0; mask[l] = 1ull << 63; } }
};
static constexpr uint64_t MEMO_CLOSED_BIT = 1ull << 61, MEMO_RELATION_BIT = 1ull << 62;

// (Re)compute the jumps of nodes [from, size). A child has a higher index than its parent, so going
// backwards, the jumps of the children are known. Jumps of a growing tree stay right (an edge
// once added never changes), they only get shorter than they could be until recomputed.
inline void memoComputeJumps(MemoHotNode* hot, size_t size, size_t from = 0) {
	for (size_t i = size; i-- > from; ) {
		MemoHotNode& node = hot[i];
		// level 0: up to MEMO_FAN single steps
		uint32_t m = i; uint64_t mask = 0; int steps = 0;
		for (; steps < MEMO_FAN; steps++) {
			const MemoHotNode& x = hot[m];
			if (x.kind < 1 || x.kind > 4) break;
			const uint32_t next = x.quick[x.kind == 1 ? 0 : 1];
			if (next == UINT32_MAX || next >= size) break;
			mask |= x.kind == 1 || x.kind == 2 ? MemoTagSlots::keyBit(x.key) : x.kind == 3 ? MEMO_CLOSED_BIT : MEMO_RELATION_BIT;
			m = next;
		}
		node.jump[0] = m; node.mask[0] = steps ? mask : 1ull << 63;
		// level l: up to MEMO_FAN jumps of level l-1 (of the children: computed already)
		for (int l = 1; l < MEMO_LEVELS; l++) {
			m = i; mask = 0; steps = 0;
			for (int k = 0; k < MEMO_FAN; k++) {
				const MemoHotNode& x = hot[m];
				if (x.mask[l - 1] >> 63) break;
				mask |= x.mask[l - 1]; m = x.jump[l - 1]; steps++;
			}
			node.jump[l] = m; node.mask[l] = steps > 1 ? mask : 1ull << 63;
		}
	}
}

// Walk the tree from the root as runWayFunction does. Kinds: 0 Unset, 1 Find, 2 Holds, 3 IsClosed,
// 4 NextRelation, 5 Leaf, 6 Lua, 7 MercatorArea, 8 FindInRelation. Returns the node reached that isn't
// a read (Unset, Leaf, Lua), or UINT32_MAX when the result read at node `last` has no child yet.
// childOf(n, result): the child of node n for a result other than the four quick ones.
// extra: the results of the reads that aren't tags: nextRelation() (only called with relations),
// area(), findInRelation(key id)
template<class ChildOf, class Extra>
inline uint32_t memoWalk(const MemoHotNode* hot, const MemoTagSlots& slots, bool isClosed, bool hasRelations, const ChildOf& childOf, Extra& extra, uint32_t& last) {
	auto byResult = [&](uint32_t n, std::string_view r) {
		if (r.empty()) return hot[n].quick[0];
		if (r.size() == 1 && (r[0] == '0' || r[0] == '1' || r[0] == '\x01')) return hot[n].quick[r[0] == '0' ? 1 : r[0] == '1' ? 2 : 3];
		return childOf(n, r);
	};
	const uint64_t present = slots.present | (isClosed ? MEMO_CLOSED_BIT : 0) | (hasRelations ? MEMO_RELATION_BIT : 0);
	uint32_t n = 0;
	while (true) {
		const MemoHotNode& node = hot[n];
		bool jumped = false;
		for (int l = MEMO_LEVELS; l-- > 0; )
			if (!(present & node.mask[l])) { n = node.jump[l]; jumped = true; break; }
		if (jumped) continue;
		uint32_t next;
		switch (node.kind) {
			case 1: {	// Find
				if (!slots.has(node.key)) { next = node.quick[0]; break; }
				const std::string_view v = slots.get(node.key);
				if (v.empty()) next = node.quick[0];
				else if (node.valueOnly) next = node.quick[3];
				else if (v.size() == 1 && (v[0] == '0' || v[0] == '1' || v[0] == '\x01')) next = node.quick[v[0] == '0' ? 1 : v[0] == '1' ? 2 : 3];
				else next = childOf(n, v);
				break;
			}
			case 2: next = node.quick[slots.has(node.key) ? 2 : 1]; break;	// Holds
			case 3: next = node.quick[isClosed ? 2 : 1]; break;
			case 4: next = hasRelations ? byResult(n, extra.nextRelation()) : node.quick[1]; break;
			case 7: next = byResult(n, extra.area()); break;
			case 8: next = byResult(n, extra.findInRelation(node.key)); break;
			default: return n;
		}
		if (next == UINT32_MAX) { last = n; return UINT32_MAX; }
		n = next;
	}
}

#endif //_MEMO_KEYS_H
