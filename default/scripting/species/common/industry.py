from common.base_prod import INDUSTRY_PER_POP
from common.priorities import (
    TARGET_AFTER_SCALING_PRIORITY,
    TARGET_EARLY_BEFORE_SCALING_PRIORITY,
    TARGET_SCALING_PRIORITY,
)
from species.common.multiplier import GREAT_MULTIPLIER

BASIC_INDUSTRY = [
    EffectsGroup(
        scope=Source,
        activation=Planet() & TargetIndustry(low=0) & Happiness(low=0) & Focus(type=["FOCUS_INDUSTRY"]),
        accountinglabel="FOCUS_INDUSTRY_LABEL",
        priority=TARGET_EARLY_BEFORE_SCALING_PRIORITY,
        effects=SetTargetIndustry(
            value=Value
            + Target.Population * NamedReal(name="INDUSTRY_FOCUS_TARGET_INDUSTRY_PERPOP", value=1.0 * INDUSTRY_PER_POP)
        ),
    ),
    EffectsGroup(  # gives human bonuses when AI Aggression set to Beginner
        scope=Source,
        activation=Planet() & IsHuman & (GalaxyMaxAIAggression == 0),  # human player, not human species
        accountinglabel="DIFFICULTY",
        priority=TARGET_AFTER_SCALING_PRIORITY,
        effects=SetTargetIndustry(value=Value * 2),
    ),
]

AVERAGE_INDUSTRY = BASIC_INDUSTRY

GREAT_INDUSTRY = [
    *BASIC_INDUSTRY,
    EffectsGroup(
        description="GREAT_INDUSTRY_DESC",
        scope=Source,
        activation=Planet() & TargetIndustry(low=0) & Happiness(low=0) & Focus(type=["FOCUS_INDUSTRY"]),
        accountinglabel="GREAT_INDUSTRY_LABEL",
        priority=TARGET_SCALING_PRIORITY,
        effects=SetTargetIndustry(
            value=Value * NamedReal(name="GREAT_INDUSTRY_TARGET_INDUSTRY_SCALING", value=GREAT_MULTIPLIER)
        ),
    ),
]
