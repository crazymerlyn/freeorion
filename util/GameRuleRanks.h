#ifndef _GameRuleRanks_h_
#define _GameRuleRanks_h_

/** This file is to keep internal engine game rules' ranks in one place for easier re-ordering,
    though still needs to be manually synchronized with game_rules.focs.py
    */

namespace GameRuleRanks {
    enum GameRuleRanks : uint32_t {
        // "" aka "GENERAL" category
        RULE_RESEED_PRNG_SERVER_RANK = 1,
        RULE_EXTRASOLAR_SHIP_DETECTION_RANK = 2,
        RULE_BASIC_VIS_SYSTEM_INFO_SHOWN_RANK = 10,
        RULE_NUM_COMBAT_ROUNDS_RANK = 30,
        RULE_AGGRESSIVE_SHIPS_COMBAT_VISIBLE_RANK = 35,
        RULE_OVERRIDE_VIS_LEVEL_RANK = 40,
        RULE_STOCKPILE_IMPORT_LIMITED_RANK = 50,
        RULE_PRODUCTION_QUEUE_FRONTLOAD_FACTOR_RANK = 51,
        RULE_PRODUCTION_QUEUE_TOPPING_UP_FACTOR_RANK = 52,

        // "BALANCE" category
        RULE_SHIP_WEAPON_DAMAGE_FACTOR_RANK = 1100,
        RULE_FIGHTER_DAMAGE_FACTOR_RANK = 1200,
        RULE_SHIP_STRUCTURE_FACTOR_RANK = 1300,
        RULE_SHIP_SPEED_FACTOR_RANK = 1400,

        // "TEST" category
        RULE_CHEAP_AND_FAST_BUILDING_PRODUCTION_RANK = 110,
        RULE_CHEAP_AND_FAST_SHIP_PRODUCTION_RANK = 111,
        RULE_CHEAP_AND_FAST_TECH_RESEARCH_RANK = 112,
        RULE_CHEAP_POLICIES_RANK = 113,
        RULE_ALL_OBJECTS_VISIBLE_RANK = 130,
        RULE_ALL_SYSTEMS_VISIBLE_RANK = 131,
        RULE_UNSEEN_STEALTHY_PLANETS_INVISIBLE_RANK = 132,
        RULE_STARLANES_EVERYWHERE_RANK = 135,

        // "BALANCE_STABILITY" category (should there be a separate opinion and annexation group?)
        RULE_ANNEX_COST_OPINION_EXP_BASE_RANK = 5500,
        RULE_ANNEX_COST_STABILITY_EXP_BASE_RANK = 5510,
        RULE_ANNEX_COST_SCALING_RANK = 5520,
        RULE_BUILDING_ANNEX_COST_SCALING_RANK = 5530,
        RULE_ANNEX_COST_MINIMUM_RANK = 5540,

        // "MULTIPLAYER" category
        RULE_DIPLOMACY_RANK = 90000000,
        RULE_THRESHOLD_HUMAN_PLAYER_WIN_RANK = 90000010,
        RULE_ONLY_ALLIANCE_WIN_RANK = 90000020,
        RULE_ALLOW_CONCEDE_RANK = 90000030,
        RULE_CONCEDE_COLONIES_THRESHOLD_RANK = 90000040,
    };
}

#endif
