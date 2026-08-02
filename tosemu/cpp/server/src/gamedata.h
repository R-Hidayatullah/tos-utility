// Game data, read from the installed client.
//
// Everything here comes out of the client's own .ipf/.ies files rather than a
// hand-maintained copy, so it tracks whatever build is installed: maps,
// monsters, spawn anchors, jobs and skills all come from the same tables the
// client reads.
//
// The one exception is the property-id table. Property ids are assigned by the
// client at runtime rather than stored in any .ies, so there is no file to
// read them from; the server takes them from a name/id list on disk. The
// shipped list was cross-checked against capture_1785545696.bin -- 16 of the
// 17 ids in a live ZC_OBJECT_PROPERTY resolve to PCEtc names -- so it matches
// this client build, but it is the only table not derived from the client and
// the place to look first if properties start reading wrong.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ies.h"
#include "ipf.h"

namespace tos::data {

struct MapData {
    int32_t id = 0;
    std::string class_name;      // "f_siauliai_west"
    std::string name;            // localised display name
    std::string eng_name;
    std::string type;
    float x = 0, y = 0, z = 0;   // DefGenX/Y/Z, the default entry point
};

struct MonsterData {
    int32_t id = 0;
    std::string class_name;
    std::string name;
    std::string faction;         // "Monster", "Neutral", "NPC", ...
    std::string rank;            // "Normal", "Boss", ...
    std::string size;
    std::string race;
    std::string type;            // ClassType: "Melee", "Magic", ...
    int32_t level = 1;
    float walk_speed = 0;
    float run_speed = 0;
    float scale = 1;
    float search_range = 0;
    float attack_range = 0;
    // Which table the row came from. Town NPCs live in their own file and are
    // mostly Faction="Neutral", so the faction string alone does not identify
    // them -- monster.ies has 137 Neutral rows of its own.
    bool from_npc_table = false;

    bool is_npc() const {
        return from_npc_table || faction == "Npc" || faction == "NPC";
    }
    // Only a Monster-faction row ever takes aggro. Everything else -- town
    // NPCs, summons, crystals, the props that share these tables -- stands
    // still and is never hostile.
    bool hostile() const { return !is_npc() && faction == "Monster"; }

    // ZC_ENTER_MONSTER's relation byte at +0x24: 1 for something the client
    // paints and targets as an enemy, 2 for a friendly. This is NOT hostile().
    // A root crystal is Faction="RootCrystal" and never chases anyone, yet the
    // live server marks it 1 because you break it; a Material-rank prop (silver
    // transporter, guild tower) and a MonRank="NPC" job master are both
    // Faction="Monster" rows that go out as 2.
    //
    // Checked against relay/dumps/capture_20260801_214001.bin: of its 162
    // ZC_ENTER_MONSTER, 130 name a row in these two tables and this rule
    // reproduces the wire byte on all 130.
    bool enemy() const {
        return !from_npc_table && rank != "Material" && rank != "NPC" &&
               !faction.empty() && faction != "Neutral" && faction != "Peaceful";
    }
};

// One row of a map's spawn table (ies_mongen/anchor_<map>.ies). This is only
// the WHERE: `gen_type` joins to the map's GenType table for the what.
struct Anchor {
    int32_t gen_type = 0;
    float x = 0, y = 0, z = 0;
    float direction = 0;
    int32_t npc_id = 0;
    float range = 0;
    std::string name;
};

// One row of a map's generator table (ies_mongen/gentype_<map>.ies), keyed by
// ClassID, which is the same number the anchor's GenType column carries.
//
// This is the WHAT of a spawn, and it -- not the anchor's NPCID column -- is
// what the live server goes by. Checked against capture_1785545696.bin: of the
// anchors whose position matches a captured spawn, resolving through GenType
// names the right monster 20 times out of 20, while NPCID gets 8 of 19. Whole
// maps have no usable NPCID at all (every one of c_Orsha's 91 anchors has 0).
struct GenTypeEntry {
    int32_t gen_type = 0;
    std::string class_type;      // monster/NPC ClassName: "npc_illanai"
    std::string name;            // display name, already localised
    std::string unique_name;
    std::string dialog;          // the dialog script the client asks for
    std::string enter;
    std::string leave;
    std::string faction;
    int32_t level = 1;
    float respawn_time = 0;      // milliseconds
    int32_t max_pop = 1;
    float gen_range = 0;
    float range = 0;
};

struct JobData {
    int32_t id = 0;
    std::string class_name;      // "Char1_1"
    std::string name;
    std::string ctrl_type;       // "Warrior", "Wizard", "Archer", ...
    int32_t rank = 1;
    int32_t barrack_stance = 0;  // the animation the lodge poses them in
};

// One entry of a job's skill tree. skilltree.ies keys rows by the job class
// name with an index suffix ("Char1_1_3" belongs to job Char1_1), and names
// the skill by class name rather than id, so both joins happen at load time.
struct SkillTreeEntry {
    int32_t skill_id = 0;
    std::string skill_class_name;
    int32_t unlock_level = 1;
    int32_t max_level = 1;
};

struct ItemData {
    int32_t id = 0;
    std::string class_name;
    std::string name;
    std::string type;            // ItemType: "Equip", "Consume", "Material", ...
    std::string group;           // GroupName
    std::string equip_type;      // Journal / slot hint
    std::string class_type;      // "Sword", "Staff", "Bow", "Hat", ...
    float weight = 0;
    int32_t price = 0;
    int32_t max_stack = 1;
    int32_t use_level = 1;
    bool stackable() const { return max_stack > 1; }
};

struct SkillData {
    int32_t id = 0;
    std::string class_name;
    std::string name;
    int32_t max_level = 1;
    float cooldown = 0;
    float range = 0;
    std::string use_type;
};

// statbase_pc.ies, keyed by the job's CtrlType. The table has exactly six rows
// -- None, Warrior, Wizard, Archer, Cleric, Scout -- and every column is a
// PERCENTAGE rate applied to a base, not an absolute stat: MOVE_SPEED is 100
// for five of them and 110 for Archer. The client's own
// SCR_GET_JOB_RATIO_STAT looks the row up by CtrlType and divides by 100.
struct PcStatBase {
    float hp = 100, sp = 100, def = 100, mdef = 100, rhp = 100, rsp = 100,
          move_speed = 100;
};

// ---- properties --------------------------------------------------------

class PropertyTable {
public:
    // Reads a "namespace name type id" list (see the file's own header).
    bool load(const std::string& path);

    // 0 when the property is unknown; callers treat that as "do not send".
    int32_t id(const std::string& ns, const std::string& name) const;
    bool is_string(const std::string& ns, const std::string& name) const;
    const std::string* name_of(const std::string& ns, int32_t id) const;
    size_t count() const { return count_; }
    bool loaded() const { return count_ != 0; }

private:
    struct Ns {
        std::unordered_map<std::string, int32_t> ids;
        std::unordered_map<int32_t, std::string> names;
        std::unordered_map<std::string, bool> strings;
    };
    std::unordered_map<std::string, Ns> ns_;
    size_t count_ = 0;
};

// ---- the whole set -----------------------------------------------------

class GameData {
public:
    // Index every archive under <game_root>/data and <game_root>/patch.
    bool open_client(const std::string& game_root);
    bool client_open() const { return fs_ && fs_->unique_count() > 0; }
    const ipf::FileSystem* fs() const { return fs_.get(); }

    // Parse the tables the server needs. Safe to call with no client present;
    // each table just stays empty and the server falls back to defaults.
    void load_tables();

    // Anchors are loaded lazily -- 711 maps' worth is a lot to parse for the
    // one or two a session actually visits.
    const std::vector<Anchor>* anchors(const std::string& map_class_name) const;
    // The map's generator table, keyed by GenType. Loaded lazily alongside the
    // anchors and returns null when the map has none.
    const std::unordered_map<int32_t, GenTypeEntry>* gen_types(
        const std::string& map_class_name) const;

    const MapData* map(int32_t id) const;
    const MapData* map(const std::string& class_name) const;
    const MonsterData* monster(int32_t id) const;
    const MonsterData* monster(const std::string& class_name) const;
    const JobData* job(int32_t id) const;
    const JobData* job(const std::string& class_name) const;
    const SkillData* skill(int32_t id) const;
    const SkillData* skill(const std::string& class_name) const;
    const ItemData* item(int32_t id) const;
    const ItemData* item(const std::string& class_name) const;
    // Lowest-requirement item of a given ClassType, which is how a starting
    // weapon is chosen without hard-coding ids that differ per build.
    const ItemData* lowest_of_class_type(const std::string& class_type) const;

    // The animation set for a job holding a given pair of weapons, resolved
    // through the client's own two tables: stancecondition.ies keys
    // (job, right hand, left hand) to a stance NAME, and stance.ies turns that
    // name into the id the packets carry. Returns 0 when nothing matches, and
    // 0 is not a valid stance -- it renders the character in a T-pose.
    int32_t stance_for(int32_t job_id, const std::string& right_hand_class,
                       const std::string& left_hand_class) const;
    // Skill tree for a job, in tree order. Empty when the job is unknown.
    const std::vector<SkillTreeEntry>& skill_tree(int32_t job_id) const;
    // Keyed by CtrlType ("Warrior", "Archer", ...), not by level. An unknown
    // key returns the all-100 default, which is the same fallback the client
    // uses when the row is missing.
    PcStatBase pc_stat_base(const std::string& ctrl_type) const;

    const std::vector<MapData>& maps() const { return maps_; }
    size_t monster_count() const { return monsters_.size(); }
    size_t skill_count() const { return skills_.size(); }
    size_t item_count() const { return items_.size(); }
    size_t job_count() const { return jobs_.size(); }

    PropertyTable properties;

private:
    bool read_ies(const std::string& vpath, ies::Table& out) const;

    std::unique_ptr<ipf::FileSystem> fs_;

    std::vector<MapData> maps_;
    std::unordered_map<int32_t, size_t> map_by_id_;
    std::unordered_map<std::string, size_t> map_by_name_;

    std::vector<MonsterData> monsters_;
    std::unordered_map<int32_t, size_t> mon_by_id_;
    std::unordered_map<std::string, size_t> mon_by_name_;

    std::vector<JobData> jobs_;
    std::unordered_map<int32_t, size_t> job_by_id_;
    std::unordered_map<std::string, size_t> job_by_name_;

    std::vector<SkillData> skills_;
    std::unordered_map<int32_t, size_t> skill_by_id_;
    std::unordered_map<std::string, size_t> skill_by_name_;

    std::unordered_map<int32_t, std::vector<SkillTreeEntry>> skill_trees_;

    std::vector<ItemData> items_;
    std::unordered_map<int32_t, size_t> item_by_id_;
    std::unordered_map<std::string, size_t> item_by_name_;

    std::unordered_map<std::string, PcStatBase> pc_stats_;

    // "<job class>|<right hand>|<left hand>" -> stance id
    std::unordered_map<std::string, int32_t> stances_;

    mutable std::unordered_map<std::string, std::vector<Anchor>> anchors_;
    mutable std::unordered_map<std::string,
                               std::unordered_map<int32_t, GenTypeEntry>>
        gen_types_;
};

}  // namespace tos::data
