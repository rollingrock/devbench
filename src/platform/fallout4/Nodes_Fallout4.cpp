#include "Tools_Fallout4.h"

#include "core/Json.h"
#include "core/MainThread.h"
#include "core/ToolRegistry.h"
#include "core/tools/Memory.h"

#include <algorithm>
#include <cmath>
#include <utility>

// nodes: walk the live scene graph.
//
// This is the on-demand form of the F4VR Common Framework's `sDumpDataOnceNames` dumps
// (all_nodes / skelly / fp_skelly / pipboy / geometry). There, a dump means: edit an
// INI, wait for the next frame, grep a log, and it only exists in a mod built on that
// framework with its debug config on. Here it is one call, returns JSON, works while
// any mod is loaded, and can be scoped to a subtree instead of ten thousand log lines.
// Node addresses are reported so a result pastes straight into the `memory` tool.
//
// The danger is concentrated in the ENTRY POINTS, not in the walk. CommonLibF4 (the
// rollingrock fork this links) types PlayerCharacter with the flat layout only, and VR
// inserts 0x470 bytes of player state ahead of firstPerson3D (+0xB78 flat, +0xFE8 VR)
// and carries a node table at +0x6E0 that flat does not have at all. A field read
// through the wrong layout compiles cleanly and returns a plausible qword — the same
// species as the MessageBoxMenu +0xE8/+0xF8 crash in Tools_Fallout4.cpp. So every
// pointer that enters the walk (named root, or a caller's address) is identified by
// its MSVC RTTI first — reads only, no virtual call — and refused, with what it
// actually is, when it is not a scene-graph object.
//
// Past the entry point the walk trusts the engine's own structure, as the framework
// does: NiAVObject's layout is the same on every runtime, and NiNode::children, which
// is NOT (+0x120 flat, +0x160 VR), is only ever reached through GetRuntimeData().
// Everything runs on the main thread, because the scene graph is mutated there.

namespace dvb
{
	namespace
	{
		constexpr int kDefaultMaxDepth = 6;
		constexpr int kHardMaxDepth = 64;
		constexpr int kDefaultMaxNodes = 500;
		constexpr int kHardMaxNodes = 5000;
		constexpr int kDefaultFindLimit = 50;
		constexpr int kHardFindLimit = 500;
		constexpr int kFindVisitCap = 200000;
		constexpr int kMaxAncestors = 256;

		// PlayerCharacter::firstPerson3D. CommonLibF4VR asserts +0xFE8 on VR and f4sevr's
		// GameReferences.h notes the same; the flat offset is CommonLibF4's own member.
		constexpr std::uintptr_t kFirstPerson3DFlat = 0xB78;
		constexpr std::uintptr_t kFirstPerson3DVR = 0xFE8;

		struct Slot
		{
			const char*   name;
			std::uint32_t offset;  // into PlayerCharacter
		};

		// RE::VRPlayerNodes as CommonLibF4VR names it: the 40 ref-counted slots at
		// PlayerCharacter+0x6E0..0x818, plus the lockpick parent after them. VR only. The
		// two unknown slots are listed too: "always null so far" is exactly the kind of
		// claim a live bench should be able to re-check.
		constexpr Slot kVRNodes[]{
			{ "playerWorldNode", 0x6E0 },
			{ "roomNode", 0x6E8 },
			{ "primaryWandNode", 0x6F0 },
			{ "primaryWandTouchpad", 0x6F8 },
			{ "primaryUIAttachNode", 0x700 },
			{ "primaryWeaponToWandNode", 0x708 },
			{ "primaryWeaponKickbackRecoilNode", 0x710 },
			{ "primaryWeaponOffsetNode", 0x718 },
			{ "primaryWeaponScopeCamera", 0x720 },
			{ "primaryVertibirdMinigunOffsetNode", 0x728 },
			{ "primaryMeleeWeaponOffsetNode", 0x730 },
			{ "primaryUnarmedPowerArmorWeaponOffsetNode", 0x738 },
			{ "primaryWandLaserPointer", 0x740 },
			{ "primaryWandLaserPointerAdjuster", 0x748 },
			{ "equippedWeaponNode", 0x750 },
			{ "primaryMeleeWeaponOffsetNodeInUse", 0x758 },
			{ "secondaryMeleeWeaponOffsetNodeInUse", 0x760 },
			{ "secondaryWandNode", 0x768 },
			{ "point002Node", 0x770 },
			{ "workshopPalletNode", 0x778 },
			{ "workshopPalletSlider", 0x780 },
			{ "secondaryUIOffsetNode", 0x788 },
			{ "secondaryMeleeWeaponOffsetNode", 0x790 },
			{ "secondaryUnarmedPowerArmorWeaponOffsetNode", 0x798 },
			{ "secondaryAimNode", 0x7A0 },
			{ "pipboyParentNode", 0x7A8 },
			{ "pipboyRootNIFOnlyNode", 0x7B0 },
			{ "screenNode", 0x7B8 },
			{ "pipboyLightParentNode", 0x7C0 },
			{ "unk7C8", 0x7C8 },
			{ "scopeParentNode", 0x7D0 },
			{ "compassDialsNode", 0x7D8 },
			{ "hmdNode", 0x7E0 },
			{ "offscreenHmdNode", 0x7E8 },
			{ "uprightHmdNode", 0x7F0 },
			{ "uprightHmdLagNode", 0x7F8 },
			{ "blackSphereNode", 0x800 },
			{ "headLightParentNode", 0x808 },
			{ "unk810", 0x810 },
			{ "weaponLeftNode", 0x818 },
			{ "lockPickParentNode", 0x828 },
		};

		// The framework's sDumpDataOnceNames, for anyone arriving with those in hand.
		// "world" is deliberately absent: the framework reaches it as the scope camera's
		// sixth parent, which names no stable node.
		constexpr std::pair<std::string_view, std::string_view> kAliases[]{
			{ "all_nodes", "scene" },
			{ "skelly", "player" },
			{ "fp_skelly", "firstPerson" },
			{ "pipboy", "pipboyRootNIFOnlyNode" },
		};

		std::string Hex(std::uintptr_t a_v)
		{
			return std::format("0x{:x}", a_v);
		}

		bool IEquals(std::string_view a_a, std::string_view a_b)
		{
			return a_a.size() == a_b.size() &&
			       std::equal(a_a.begin(), a_a.end(), a_b.begin(), [](char x, char y) {
					   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
				   });
		}

		bool IContains(std::string_view a_hay, std::string_view a_needle)
		{
			return std::search(a_hay.begin(), a_hay.end(), a_needle.begin(), a_needle.end(), [](char x, char y) {
				return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
			}) != a_hay.end();
		}

		// Rounded to 1e-4 so a tree stays readable; NaN/Inf as strings, never null —
		// a NaN transform is frequently the entire reason someone is looking.
		json Num(float a_v)
		{
			if (!std::isfinite(a_v))
				return std::isnan(a_v) ? "NaN" : (a_v > 0 ? "Inf" : "-Inf");
			return std::round(static_cast<double>(a_v) * 1e4) / 1e4;
		}

		json Vec(const RE::NiPoint3& a_p)
		{
			return json::array({ Num(a_p.x), Num(a_p.y), Num(a_p.z) });
		}

		json Rows(const RE::NiMatrix3& a_m)
		{
			json rows = json::array();
			for (std::size_t r = 0; r < 3; ++r)
				rows.push_back(json::array({ Num(a_m.entry[r][0]), Num(a_m.entry[r][1]), Num(a_m.entry[r][2]) }));
			return rows;
		}

		json Transform(const RE::NiTransform& a_t, bool a_rotation)
		{
			json out{ { "pos", Vec(a_t.translate) }, { "scale", Num(a_t.scale) } };
			if (a_rotation)
				out["rot"] = Rows(a_t.rotate);
			return out;
		}

		std::string NameOf(const RE::NiAVObject* a_node)
		{
			const char* s = a_node->name.c_str();
			return s ? std::string(s) : std::string{};
		}

		json TypeOf(const RE::NiAVObject* a_node)
		{
			const auto name = mem::RttiClassName(reinterpret_cast<std::uintptr_t>(a_node));
			return name ? json(*name) : json(nullptr);
		}

		int ChildCount(RE::NiAVObject* a_node)
		{
			auto* node = a_node->IsNode();
			if (!node)
				return 0;
			int n = 0;
			for (const auto& child : node->GetRuntimeData().children) {
				if (child)
					++n;
			}
			return n;
		}

		// The short form: identity plus the transforms the framework's dump printed.
		json Brief(RE::NiAVObject* a_node)
		{
			return json{
				{ "name", NameOf(a_node) },
				{ "type", TypeOf(a_node) },
				{ "address", Hex(reinterpret_cast<std::uintptr_t>(a_node)) },
				{ "hidden", (a_node->GetFlags() & 1) != 0 },
			};
		}

		json Summary(RE::NiAVObject* a_node, bool a_rotation)
		{
			json out = Brief(a_node);
			out["childCount"] = ChildCount(a_node);
			out["local"] = Transform(a_node->local, a_rotation);
			out["world"] = Transform(a_node->world, a_rotation);
			return out;
		}

		// ---- entry points ------------------------------------------------------------

		// Identity before trust. Nothing read through an offset reaches the walk until
		// its RTTI says it is a scene-graph object.
		RE::NiAVObject* AsSceneObject(std::uintptr_t a_va)
		{
			return a_va && mem::RttiIsA(a_va, "NiAVObject") ? reinterpret_cast<RE::NiAVObject*>(a_va) : nullptr;
		}

		std::uintptr_t ReadQWordAt(const void* a_base, std::uintptr_t a_offset)
		{
			std::uintptr_t v = 0;
			return mem::SafeRead(static_cast<const std::byte*>(a_base) + a_offset, &v, sizeof(v)) ? v : 0;
		}

		RE::PlayerCharacter* RequirePlayer()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player)
				throw ToolError(409, "nodes: no player (no save loaded yet)");
			return player;
		}

		// A resolved starting point. `address` is 0 when the slot is legitimately empty
		// (e.g. equippedWeaponNode while nothing is drawn); `refused` is set when it held
		// something that is not a scene-graph object — both are answers, not failures.
		struct Resolved
		{
			std::string     name;
			std::uintptr_t  address = 0;
			RE::NiAVObject* node = nullptr;
			std::string     refused;
			json            source = json::object();
		};

		Resolved FromPointer(std::string a_name, std::uintptr_t a_va, json a_source)
		{
			Resolved r{ std::move(a_name), a_va, nullptr, {}, std::move(a_source) };
			if (!a_va)
				return r;
			r.node = AsSceneObject(a_va);
			if (!r.node) {
				const auto actual = mem::RttiClassName(a_va);
				r.refused = actual ? std::format("{} holds a {}, not a scene-graph object (NiAVObject)", Hex(a_va), *actual) :
				                     std::format("{} is not a polymorphic object at all -- nothing was dereferenced", Hex(a_va));
			}
			return r;
		}

		Resolved PlayerRoot()
		{
			auto*          player = RequirePlayer();
			std::uintptr_t va = 0;
			if (player->loadedData)
				va = reinterpret_cast<std::uintptr_t>(player->loadedData->data3D.get());
			return FromPointer("player", va, json{ { "via", "PlayerCharacter::loadedData->data3D" } });
		}

		Resolved FirstPersonRoot()
		{
			auto*      player = RequirePlayer();
			const bool vr = REL::Module::IsVR();
			const auto offset = vr ? kFirstPerson3DVR : kFirstPerson3DFlat;
			return FromPointer("firstPerson", ReadQWordAt(player, offset),
				json{ { "via", "PlayerCharacter::firstPerson3D" }, { "offset", Hex(offset) }, { "layout", vr ? "vr" : "flat" } });
		}

		// The top of the graph the player's 3D hangs in, reached by walking parents —
		// the same route the framework's all_nodes dump takes.
		Resolved SceneRoot()
		{
			Resolved start = PlayerRoot();
			if (!start.node)
				return Resolved{ "scene", 0, nullptr, start.refused, json{ { "via", "top ancestor of the player's 3D" } } };
			RE::NiAVObject* node = start.node;
			int             hops = 0;
			while (node->parent && hops < kMaxAncestors) {
				node = node->parent;
				++hops;
			}
			return Resolved{ "scene", reinterpret_cast<std::uintptr_t>(node), node, {},
				json{ { "via", "top ancestor of the player's 3D" }, { "hops", hops } } };
		}

		const Slot* FindVRSlot(std::string_view a_name)
		{
			for (const auto& s : kVRNodes) {
				if (IEquals(s.name, a_name))
					return &s;
			}
			return nullptr;
		}

		Resolved VRSlotRoot(const Slot& a_slot)
		{
			auto* player = RequirePlayer();
			return FromPointer(a_slot.name, ReadQWordAt(player, a_slot.offset),
				json{ { "via", "RE::VRPlayerNodes" }, { "offset", Hex(a_slot.offset) } });
		}

		std::string ValidRootNames()
		{
			std::string s = "scene, player, firstPerson";
			if (REL::Module::IsVR()) {
				for (const auto& slot : kVRNodes)
					s += std::string(", ") + slot.name;
			} else {
				s += " (the VR node table is not available on this flat runtime)";
			}
			return s;
		}

		Resolved ResolveNamedRoot(std::string_view a_name)
		{
			std::string_view name = a_name;
			for (const auto& [alias, target] : kAliases) {
				if (IEquals(alias, name))
					name = target;
			}
			if (IEquals(name, "scene"))
				return SceneRoot();
			if (IEquals(name, "player"))
				return PlayerRoot();
			if (IEquals(name, "firstPerson"))
				return FirstPersonRoot();
			if (const Slot* slot = FindVRSlot(name)) {
				if (!REL::Module::IsVR())
					throw ToolError(404, std::format("nodes: '{}' is a VR player node; this is flat Fallout 4", slot->name));
				return VRSlotRoot(*slot);
			}
			throw ToolError(400, std::format("nodes: unknown root '{}'. Valid: {}", a_name, ValidRootNames()));
		}

		// `address` wins over `root`: an address expression (the `memory` grammar, so a
		// result's address or [base+0x...] pastes in) validated like any other entry.
		RE::NiAVObject* ResolveStart(const json& a_args, const std::string& a_defaultRoot, json& a_describe)
		{
			const std::string addr = a_args.value("address", std::string{});
			Resolved          r;
			if (!addr.empty()) {
				std::uint64_t va = 0;
				std::string   err;
				if (!mem::ResolveAddress(addr, va, err))
					throw ToolError(400, "nodes: address: " + err);
				r = FromPointer({}, static_cast<std::uintptr_t>(va), json{ { "via", "address" }, { "expr", addr } });
			} else {
				r = ResolveNamedRoot(a_args.value("root", a_defaultRoot));
			}

			if (!r.refused.empty())
				throw ToolError(422, "nodes: " + r.refused);
			if (!r.node)
				throw ToolError(404, std::format("nodes: '{}' is empty right now (a null slot, or the player's 3D is not loaded)", r.name));

			a_describe = r.source;
			if (!r.name.empty())
				a_describe["root"] = r.name;
			return r.node;
		}

		int ClampArg(const json& a_args, const char* a_key, int a_default, int a_min, int a_max, bool& a_capped)
		{
			const auto v = a_args.value(a_key, static_cast<std::int64_t>(a_default));
			if (v > a_max) {
				a_capped = true;
				return a_max;
			}
			return static_cast<int>(std::max<std::int64_t>(v, a_min));
		}

		// ---- actions -----------------------------------------------------------------

		json RootEntry(const Resolved& a_r)
		{
			json out{ { "name", a_r.name } };
			for (const auto& [k, v] : a_r.source.items())
				out[k] = v;
			if (a_r.node) {
				out["address"] = Hex(a_r.address);
				out["nodeName"] = NameOf(a_r.node);
				out["type"] = TypeOf(a_r.node);
			} else if (!a_r.refused.empty()) {
				out["address"] = Hex(a_r.address);
				out["refused"] = a_r.refused;
			} else {
				out["address"] = nullptr;
			}
			return out;
		}

		json Roots()
		{
			json out{
				{ "vr", REL::Module::IsVR() },
				{ "roots", json::array({ RootEntry(SceneRoot()), RootEntry(PlayerRoot()), RootEntry(FirstPersonRoot()) }) },
			};
			if (REL::Module::IsVR()) {
				json vr = json::array();
				for (const auto& slot : kVRNodes)
					vr.push_back(RootEntry(VRSlotRoot(slot)));
				out["vrNodes"] = std::move(vr);
			} else {
				out["vrNodes"] = nullptr;
				out["note"] = "flat runtime: there is no VR player node table";
			}
			return out;
		}

		struct TreeWalk
		{
			int  maxDepth;
			int  maxNodes;
			bool rotation;
			int  emitted = 0;
			bool capped = false;
		};

		json TreeOf(RE::NiAVObject* a_node, int a_depth, TreeWalk& a_walk)
		{
			json out = Summary(a_node, a_walk.rotation);
			++a_walk.emitted;
			auto* node = a_node->IsNode();
			if (!node || out["childCount"].get<int>() == 0)
				return out;
			if (a_depth >= a_walk.maxDepth) {
				out["depthCut"] = true;  // has children, not expanded
				return out;
			}
			json children = json::array();
			for (const auto& child : node->GetRuntimeData().children) {
				if (!child)
					continue;
				if (a_walk.emitted >= a_walk.maxNodes) {
					a_walk.capped = true;
					break;
				}
				children.push_back(TreeOf(child.get(), a_depth + 1, a_walk));
			}
			// Never a silently short list: a node whose children were cut by the node
			// cap says so itself, not only the top-level `capped`.
			if (static_cast<int>(children.size()) < out["childCount"].get<int>())
				out["partial"] = true;
			out["children"] = std::move(children);
			return out;
		}

		json Tree(const json& a_args)
		{
			bool     depthCapped = false;
			bool     nodesCapped = false;
			TreeWalk walk{
				ClampArg(a_args, "maxDepth", kDefaultMaxDepth, 0, kHardMaxDepth, depthCapped),
				ClampArg(a_args, "maxNodes", kDefaultMaxNodes, 1, kHardMaxNodes, nodesCapped),
				a_args.value("rotation", false),
			};
			json  start;
			auto* root = ResolveStart(a_args, "player", start);
			json  tree = TreeOf(root, 0, walk);
			json  out{
				{ "start", std::move(start) },
				{ "maxDepth", walk.maxDepth },
				{ "maxNodes", walk.maxNodes },
				{ "nodes", walk.emitted },
				{ "capped", walk.capped },
				{ "tree", std::move(tree) },
			};
			if (depthCapped || nodesCapped)
				out["argsClamped"] = json{ { "maxDepth", depthCapped }, { "maxNodes", nodesCapped } };
			return out;
		}

		// "a/b/c" from the search root down to `a_node`, from parent pointers — built for
		// matches only, so a 100k-node search does not pay for a path per visit.
		std::string PathFrom(RE::NiAVObject* a_root, RE::NiAVObject* a_node)
		{
			std::vector<std::string> parts;
			for (auto* n = a_node; n && static_cast<int>(parts.size()) < kMaxAncestors; n = n->parent) {
				const auto name = NameOf(n);
				parts.push_back(name.empty() ? "(unnamed)" : name);
				if (n == a_root)
					break;
			}
			std::string path;
			for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
				if (!path.empty())
					path += '/';
				path += *it;
			}
			return path;
		}

		struct Search
		{
			std::vector<RE::NiAVObject*> matches;
			int                          visited = 0;
			bool                         limited = false;      // more matches exist than the limit
			bool                         visitCapped = false;  // the graph was not fully searched
		};

		// Depth-first, iterative (a scene graph is deep enough that recursion per node is
		// a stack risk for no benefit), visiting children in their stored order.
		Search SearchUnder(RE::NiAVObject* a_root, const std::string& a_exact, const std::string& a_contains, int a_limit)
		{
			Search                       s;
			std::vector<RE::NiAVObject*> stack{ a_root };
			while (!stack.empty() && s.visited < kFindVisitCap) {
				auto* n = stack.back();
				stack.pop_back();
				++s.visited;
				const auto name = NameOf(n);
				if (a_exact.empty() ? IContains(name, a_contains) : IEquals(name, a_exact)) {
					if (static_cast<int>(s.matches.size()) >= a_limit) {
						s.limited = true;
						return s;
					}
					s.matches.push_back(n);
				}
				if (auto* node = n->IsNode()) {
					auto& children = node->GetRuntimeData().children;
					for (auto i = children.capacity(); i-- > 0;) {
						if (auto* c = children[i].get())
							stack.push_back(c);
					}
				}
			}
			s.visitCapped = !stack.empty();
			return s;
		}

		void RequireOneOf(const std::string& a_exact, const std::string& a_contains, const char* a_action)
		{
			if (a_exact.empty() == a_contains.empty())
				throw ToolError(400, std::format("nodes {}: give exactly one of 'name' (whole name, case-insensitive) or 'contains' (substring)", a_action));
		}

		json Find(const json& a_args)
		{
			const std::string exact = a_args.value("name", std::string{});
			const std::string contains = a_args.value("contains", std::string{});
			RequireOneOf(exact, contains, "find");

			bool         limitCapped = false;
			const int    limit = ClampArg(a_args, "limit", kDefaultFindLimit, 1, kHardFindLimit, limitCapped);
			json         start;
			auto*        root = ResolveStart(a_args, "scene", start);
			const Search s = SearchUnder(root, exact, contains, limit);

			json matches = json::array();
			for (auto* n : s.matches) {
				json m = Brief(n);
				m["path"] = PathFrom(root, n);
				m["world"] = Transform(n->world, false);
				matches.push_back(std::move(m));
			}
			return json{
				{ "start", std::move(start) },
				{ "matches", std::move(matches) },
				{ "visited", s.visited },
				{ "limited", s.limited },
				{ "visitCapped", s.visitCapped },
			};
		}

		json Get(const json& a_args)
		{
			json            start;
			RE::NiAVObject* node = nullptr;
			// `name` = the first match under root (default: the whole scene), so "get
			// HMDNode" is one call instead of find-then-get.
			if (const std::string name = a_args.value("name", std::string{}); !name.empty()) {
				auto*        root = ResolveStart(a_args, "scene", start);
				const Search s = SearchUnder(root, name, {}, 1);
				if (s.matches.empty())
					throw ToolError(404, std::format("nodes get: no node named '{}' under '{}' ({} visited{})", name, NameOf(root), s.visited,
											 s.visitCapped ? ", search capped" : ""));
				node = s.matches.front();
			} else {
				if (!a_args.contains("address") && !a_args.contains("root"))
					throw ToolError(400, "nodes get: give 'address', 'root', or 'name'");
				node = ResolveStart(a_args, "player", start);
			}

			json out = Summary(node, true);
			out["start"] = std::move(start);
			out["flags"] = Hex(node->GetFlags());
			out["worldBound"] = json{ { "center", Vec(node->worldBound.center) }, { "radius", Num(node->worldBound.fRadius) } };

			json ancestors = json::array();
			for (auto* p = node->parent; p && static_cast<int>(ancestors.size()) < kMaxAncestors; p = p->parent)
				ancestors.push_back(Brief(p));
			out["ancestors"] = std::move(ancestors);  // nearest first

			if (auto* n = node->IsNode()) {
				json children = json::array();
				for (const auto& child : n->GetRuntimeData().children) {
					if (child)
						children.push_back(Brief(child.get()));
				}
				out["children"] = std::move(children);
			}
			return out;
		}

		// BSFadeNode's flattened geometry list: every shape the node renders, with its
		// hidden bit — the framework's `geometry` dump, which is how a hidden body part
		// or a stray visible one is found without guessing at node names.
		json Geometry(const json& a_args)
		{
			json       start;
			auto*      root = ResolveStart(a_args, "player", start);
			const auto va = reinterpret_cast<std::uintptr_t>(root);
			if (!mem::RttiIsA(va, "BSFadeNode"))
				throw ToolError(422, std::format("nodes geometry: '{}' is a {}, not a BSFadeNode -- only a fade node carries a flattened geometry list",
										 NameOf(root), mem::RttiClassName(va).value_or("<no RTTI>")));
			// GetFadeNodeRuntimeData, not GetRuntimeData: the latter is NiNode's and would
			// hand back the children block. geomArray moves with the runtime too (+0x168
			// flat, +0x1A8 VR).
			auto&      data = static_cast<RE::BSFadeNode*>(root)->GetFadeNodeRuntimeData();
			json       shapes = json::array();
			const auto count = data.geomArray.size();
			for (std::uint32_t i = 0; i < count; ++i) {
				auto* geometry = data.geomArray[i].geometry.get();
				if (!geometry) {
					shapes.push_back(json{ { "index", i }, { "geometry", nullptr } });
					continue;
				}
				json g = Brief(geometry);
				g["index"] = i;
				shapes.push_back(std::move(g));
			}
			return json{ { "start", std::move(start) }, { "node", Brief(root) }, { "count", count }, { "geometry", std::move(shapes) } };
		}

		json NodesHandler(const json& a_args, const ToolContext&)
		{
			const std::string action = a_args.value("action", std::string("roots"));
			if (action != "roots" && action != "tree" && action != "find" && action != "get" && action != "geometry")
				throw ToolError(400, std::format("nodes: unknown action '{}' (roots|tree|find|get|geometry)", action));

			return MainThread::RunAndWait([action, a_args]() -> json {
				if (action == "roots")
					return Roots();
				if (action == "tree")
					return Tree(a_args);
				if (action == "find")
					return Find(a_args);
				if (action == "get")
					return Get(a_args);
				return Geometry(a_args);
			});
		}
	}

	void RegisterNodeTools(ToolRegistry& a_registry)
	{
		ToolDescriptor nodes;
		nodes.name = "nodes";
		nodes.description =
			"Walk the live scene graph (NiAVObject/NiNode) — the on-demand, JSON form of the F4VR "
			"Common Framework's sDumpDataOnceNames dumps, available to any mod without an INI edit. "
			"Runs on the main thread. Every node is reported as { name, type, address, hidden } — "
			"`type` is the object's real class from its RTTI, and `address` pastes into the `memory` "
			"tool. Transforms are { pos:[x,y,z], scale } (plus rot rows when asked), rounded to 1e-4, "
			"with NaN/Inf as strings. "
			"actions: 'roots' (default) → the named starting points and what each currently holds: "
			"scene (top of the graph the player is in), player (third-person 3D), firstPerson, and on "
			"VR every RE::VRPlayerNodes slot (hmdNode, primaryWandNode, pipboyRootNIFOnlyNode, "
			"primaryWeaponScopeCamera, ...). A slot that holds something that is not a node is "
			"reported as `refused`, never followed. "
			"'tree' (root default 'player', or address; maxDepth default 6 ≤ 64; maxNodes default "
			"500 ≤ 5000; rotation) → nested { ..., childCount, local, world, children } — a node "
			"with unexpanded children says `depthCut`, one cut by maxNodes says `partial`, and "
			"`capped` is set at the top. "
			"'find' (name = whole name, or contains = substring, both case-insensitive; root default "
			"'scene'; limit default 50) → matches with their path and world transform; `limited` = "
			"more matches exist, `visitCapped` = the graph was not fully searched. "
			"'get' (address | root | name = first match under root) → one node in full: local/world "
			"with rotation, flags, worldBound, ancestors (nearest first), children. "
			"'geometry' (root default 'player') → a BSFadeNode's flattened geometry list with each "
			"shape's hidden bit. "
			"The framework's dump names work as root aliases: all_nodes→scene, skelly→player, "
			"fp_skelly→firstPerson, pipboy→pipboyRootNIFOnlyNode.";
		nodes.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "action", json{ { "type", "string" }, { "enum", json::array({ "roots", "tree", "find", "get", "geometry" }) }, { "description", "default 'roots'" } } },
								{ "root", json{ { "type", "string" }, { "description", "named starting point (see action='roots'), case-insensitive" } } },
								{ "address", json{ { "type", "string" }, { "description", "start at this node instead of a named root: an address expression as the `memory` tool takes it (e.g. a node address from an earlier result). Refused unless its RTTI says it is a NiAVObject." } } },
								{ "name", json{ { "type", "string" }, { "description", "find/get: whole node name, case-insensitive" } } },
								{ "contains", json{ { "type", "string" }, { "description", "find: substring of the node name, case-insensitive" } } },
								{ "maxDepth", json{ { "type", "integer" }, { "description", "tree: levels below the start to expand, default 6, max 64" } } },
								{ "maxNodes", json{ { "type", "integer" }, { "description", "tree: nodes to emit, default 500, max 5000" } } },
								{ "rotation", json{ { "type", "boolean" }, { "description", "tree: include rotation matrix rows in local/world" } } },
								{ "limit", json{ { "type", "integer" }, { "description", "find: max matches, default 50, max 500" } } },
							} },
		};
		nodes.readOnly = true;
		a_registry.Register(std::move(nodes), &NodesHandler);
	}
}
