from __future__ import annotations

from typing import Any, TypeVar

from typing_extensions import TypeAlias

# Default value for int keyword arguments, use it when default value is not clear
from focs._types import (
    _ID,
    _T,
    _Aggregator,
    _BuildingType,
    _Condition,
    _ConditionalComposition,
    _DesignID,
    _Effect,
    _EffectGroup,
    _Empire,
    _FloatParam,
    _Focus,
    _IntParam,
    _PlanetEnvironment,
    _PlanetId,
    _PlanetSize,
    _PlanetType,
    _Resource,
    _SpeciesValue,
    _StarType,
    _StringParam,
    _SystemID,
    _ValueParam,
    _Visibility,
)

Blue = _StarType()
White = _StarType()
Orange = _StarType()
Yellow = _StarType()
Red = _StarType()
Neutron = _StarType()
NoStar = _StarType()
BlackHole = _StarType()

def Object(id) -> _Condition: ...

Invisible = _Visibility()
Basic = _Visibility()
Partial = _Visibility()
Full = _Visibility()

Tiny = _PlanetSize()
Small = _PlanetSize()
Medium = _PlanetSize()
Large = _PlanetSize()
Huge = _PlanetSize()

Swamp = _PlanetType()
Toxic = _PlanetType()
Inferno = _PlanetType()
Radiated = _PlanetType()
Barren = _PlanetType()
Tundra = _PlanetType()
Desert = _PlanetType()
Terran = _PlanetType()
Ocean = _PlanetType()
AsteroidsType = _PlanetType()
GasGiantType = _PlanetType()

Good = _PlanetEnvironment()
Adequate = _PlanetEnvironment()
Poor = _PlanetEnvironment()
Hostile = _PlanetEnvironment()
Uninhabitable = _PlanetEnvironment()

ThisSpecies = _SpeciesValue()

BuildBuilding = _BuildingType()

AnyEmpire = _Empire()
EnemyOf = _Empire()
AllyOf = _Empire()
CurrentTurn = 0

ContentFocus = _Focus()

class _SystemInfo:
    LastTurnBattleHere: int
    ID: _ID

class Source:
    """
    FOCS Source is IsSource, this class is fpr Source.<something>
    """

    Owner: _Empire
    ID: _ID
    SystemID: _SystemID
    Species: _SpeciesValue
    System: _SystemInfo
    TargetPopulation: float
    Research: float
    Industry: float
    HabitableSize: float

class LocalCandidate:
    LastTurnConquered: int
    LastTurnActiveInBattle: int
    LastTurnColonized: int
    TurnsSinceFocusChange: int
    LastTurnAttackedByShip: int
    LastTurnResupplied: int
    ETA: int
    ArrivedOnTurn: int
    ID: _ID
    Owner: _Empire
    Focus: _Focus
    NextSystemID: _SystemID
    System: _SystemInfo
    OrderedColonizePlanetID: _PlanetId
    Species: _SpeciesValue
    LastInvadedByEmpire: _Empire
    LastColonizedByEmpire: _Empire
    MaxStructure: float
    Industry: float
    TargetResearch: float
    Research: float
    TargetIndustry: float
    Stockpile: float
    MaxStockpile: float
    Construction: float
    TargetConstruction: float
    Influence: float
    TargetInfluence: float
    Population: float
    TargetPopulation: float
    Happiness: float
    TargetHappiness: float

class RootCandidate:
    ID: _ID
    Owner: _Empire
    PreviousSystemID: _SystemID

class Target:
    Owner: _Empire
    ID: _ID
    SystemID: _SystemID
    DesignID: _DesignID
    Construction: Any
    PreviousSystemID: _SystemID
    TurnsSinceFocusChange: int
    Population: float
    Happiness: float
    HabitableSize: float
    MaxSupply: float
    MaxFuel: float
    MaxStructure: float
    MaxTroops: float
    MaxDefense: float
    MaxShield: float
    MaxStockpile: float
    TargetPopulation: float
    TargetResearch: float
    TargetInfluence: float
    TargetIndustry: float
    TargetConstruction: float
    TargetHappiness: float

_O = TypeVar("_O", str, int, float)

class _ValuePlaceholder(_ConditionalComposition):
    def __call__(self, arg: _T) -> _T: ...
    def __add__(self, other: _T) -> _T: ...
    def __radd__(self, other: _T) -> _T: ...
    def __sub__(self, other: _T) -> _T: ...
    def __rsub__(self, other: _T) -> _T: ...
    def __mul__(self, other: _T) -> _T: ...
    def __rmul__(self, other: _T) -> _T: ...
    def __floordiv__(self, other: _T) -> _T: ...
    def __rfloordiv__(self, other: _T) -> _T: ...
    def __truediv__(self, other: _T) -> _T: ...
    def __rtruediv__(self, other: _T) -> _T: ...
    def __pow__(self, other: _T) -> _T: ...
    def __rpow__(self, other: _T) -> _T: ...

Value = _ValuePlaceholder()

Min = _Aggregator()
Max = _Aggregator()
Sum = _Aggregator()
Mode = _Aggregator()

GalaxySize: _FloatParam = ...

_args_type = int | float | str | _ValuePlaceholder

def OneOf(type_: type[_T], *args: _args_type) -> _T: ...
def Abs(type_: type[_T], value: _args_type) -> _T: ...
def Floor(type_: type[_T], value: _FloatParam) -> _T: ...
def MinOf(type_: type[_T], *args: _args_type) -> _T: ...
def MaxOf(type_: type[_T], *args: _args_type) -> _T: ...
def DirectDistanceBetween(target: _ID, source: _ID) -> float: ...
def StatisticIf(type_: type[_T], condition: _Condition) -> _T: ...
def StatisticCount(type_: type[_T], condition: _Condition) -> _T: ...
def Statistic(type_: type[_T], aggregator: _Aggregator, value: _ValueParam, condition: _Condition) -> _T: ...
def NamedReal(*, name: str, value: _FloatParam) -> float: ...
def NamedRealLookup(*, name: str) -> float: ...
def NamedIntegerLookup(*, name: str) -> int: ...
def JumpsBetween(source: _ID, target: _ID) -> int: ...
def SpeciesShipsDestroyed(empire: _Empire, name: _SpeciesValue) -> int: ...
def SpeciesShipsLost(empire: _Empire, name: _SpeciesValue) -> int: ...
def SpeciesEmpireTargetOpinion(*, species: _SpeciesValue, empire: _Empire) -> float: ...
def EmpireStockpile(empire: _Empire, resource: _Resource) -> float: ...
def PartCapacity(*, name: str) -> float: ...
def PartSecondaryStat(*, name: str) -> float: ...
def PartsInShipDesign(*, name: str, design: _DesignID) -> float: ...
def UserString(name: str) -> str: ...
def EmpireMeterValue(empire: _Empire, meter: str) -> float: ...
def EffectsGroup(
    *,
    scope: _Condition,
    effects: list[_Effect] | _Effect,
    activation: bool | _Condition | None = None,
    description: str = "",
    priority=None,
    accountinglabel: str = "",
    stackinggroup: str = "",
) -> _EffectGroup: ...

# Conditions

CurrentContent = ""

def Enqueued(type: _BuildingType, name: str) -> _Condition: ...
def TurnTechResearched(*, empire: _Empire, name: str) -> _Condition: ...
def IsBuilding(*, name: list[str] = ...) -> _Condition: ...
def Number(*, low: int, condition: _Condition) -> _Condition: ...
def Planet(
    *, type: list[_PlanetType] = ..., environment: list[_PlanetEnvironment] = ..., size: list[_PlanetSize] = ...
) -> _Condition: ...
def Star(*, type: list[_StarType]) -> _Condition: ...
def Location(type: _Focus, name: _SpeciesValue, name2: _Focus | str) -> _Condition: ...
def HasSpecies(*, name: list[_StringParam] = ...) -> _Condition: ...
def IsField(*, name: list[_StringParam]) -> _Condition: ...
def Population(*, low: _FloatParam = ..., high: _FloatParam = ...) -> _Condition: ...
def TargetPopulation(*, low: _FloatParam = ..., high: _FloatParam = ...) -> _Condition: ...
def SpeciesLikes(*, name: _Focus) -> _Condition: ...
def SpeciesDislikes(*, name: _Focus) -> _Condition: ...
def Homeworld(*, name: list[_SpeciesValue] = ...) -> _Condition: ...
def HasTag(*, name: str) -> _Condition: ...
def VisibleToEmpire(*, empire: _Empire) -> _Condition: ...
def ContainedBy(condition: _Condition) -> _Condition: ...
def HasSpecial(*, name: str) -> _Condition: ...
def OwnerHasTech(*, name: str) -> _Condition: ...

System = _Condition()
Ship = _Condition()
Monster = _Condition()
Armed = _Condition()
Unowned = _Condition()
Capital = _Condition()
IsHuman = _Condition()
Fleet = _Condition()
Stationary = _Condition()
IsSource = _Condition()
IsTarget = _Condition()
CanColonize = _Condition()

def InSystem(*, id: _SystemID = _SystemID()) -> _Condition: ...

GalaxyMaxAIAggression = 0
GalaxyMonsterFrequency = 0

def MinimumNumberOf(number: int, sortkey: _FloatParam, condition: _Condition) -> _Condition: ...
def TargetIndustry(*, low: int) -> _Condition: ...
def Happiness(*, low: _FloatParam) -> _Condition: ...
def Focus(*, type: list[str]) -> _Condition: ...
def Random(*, probability: _FloatParam) -> _Condition: ...
def OwnedBy(*, affiliation: _Empire = _Empire(), empire: _Empire = _Empire()) -> _Condition: ...
def WithinStarlaneJumps(*, jumps: _IntParam, condition: _Condition) -> _Condition: ...
def WithinDistance(*, distance: int, condition: _Condition) -> _Condition: ...
def ResourceSupplyConnected(*, empire: _Empire, condition: _Condition) -> _Condition: ...
def Contains(scope: _Condition) -> _Condition: ...
def Turn(*, high: _IntParam = ..., low: _IntParam = ...) -> _Condition: ...
def Structure(*, low: _FloatParam = ..., high: _FloatParam = ...) -> _Condition: ...
def ResupplyableBy(*, empire: _Empire) -> _Condition: ...
def DesignHasPart(*, name: str, low: int = ..., high: int = ...) -> _Condition: ...
def EmpireHasAdoptedPolicy(*, name: str, empire: _Empire = _Empire()) -> _Condition: ...

Influence = _Resource()

def HasEmpireStockpile(*, empire: _Empire, resource: _Resource, low: int = ..., high: int = ...) -> _Condition: ...
def NumPoliciesAdopted(*, empire: _Empire) -> _Condition: ...
def OnPlanet(*, id: _ID) -> _Condition: ...

# Effects
Destroy = _Effect()

_RuleType: TypeAlias = type[_T]

def GameRule(
    *,
    type: _RuleType,
    name: str,
) -> float: ...

NoEffect = _Effect()

def Conditional(
    *,
    condition: _Condition | bool,
    effects: list[_Effect],
    else_: list[_Effect] = ...,
) -> _Effect: ...
def AddSpecial(*, name: str) -> _Effect: ...
def RemoveSpecial(*, name: str) -> _Effect: ...
def SetMaxShield(*, value: _FloatParam, accountinglabel: str = "") -> _Effect: ...
def SetMaxStructure(*, value: _FloatParam, accountinglabel: str = "") -> _Effect: ...
def SetEmpireStockpile(*, resource: _Resource, value: _FloatParam) -> _Effect: ...
def SetShield(*, value: _FloatParam) -> _Effect: ...
def SetTargetIndustry(*, value: _FloatParam) -> _Effect: ...
def SetDetection(*, value: _FloatParam) -> _Effect: ...
def SetStructure(*, value: _FloatParam) -> _Effect: ...
def SetFuel(*, value: _FloatParam) -> _Effect: ...
def SetDefense(*, value: _FloatParam) -> _Effect: ...
def SetMaxTroops(*, value: _FloatParam) -> _Effect: ...
def SetStockpile(*, value: _FloatParam) -> _Effect: ...
def SetMaxStockpile(*, value: _FloatParam, accountinglabel: str = "") -> _Effect: ...
def SetTargetHappiness(*, value: _FloatParam) -> _Effect: ...
def GiveEmpirePolicy(*, name: str, empire: _Empire = _Empire()) -> _Effect: ...
def SetVisibility(empire: _Empire, visibility: Any) -> _Effect: ...
def SetPopulation(*, value: _FloatParam) -> _Effect: ...
def SetTroops(*, value: _FloatParam) -> _Effect: ...
def SetMaxDefense(*, value: _FloatParam) -> _Effect: ...
def SetSupply(*, value: _FloatParam) -> _Effect: ...
def SetTargetResearch(*, value: _FloatParam) -> _Effect: ...
def SetHappiness(*, value: _FloatParam) -> _Effect: ...
def SetPlanetSize(*, planetsize: _PlanetSize) -> _Effect: ...
def SetMaxSupply(*, value: _FloatParam) -> _Effect: ...
def SetMaxFuel(*, value: _FloatParam) -> _Effect: ...
def SetResearch(*, value: _FloatParam) -> _Effect: ...
def SetIndustry(*, value: _FloatParam) -> _Effect: ...
def SetInfluence(*, value: _FloatParam) -> _Effect: ...
def SetSpeciesTargetOpinion(*, species: _SpeciesValue, empire: _Empire, opinion: _FloatParam) -> _Effect: ...
def SetSpeciesOpinion(*, species: _SpeciesValue, empire: _Empire, opinion: _FloatParam) -> _Effect: ...
def SetTargetPopulation(*, value: _FloatParam, accountinglabel: str = "") -> _Effect: ...
def SetTargetInfluence(*, value: _FloatParam) -> _Effect: ...
def SetTargetConstruction(*, value: _FloatParam) -> _Effect: ...
def SetConstruction(*, value: _FloatParam) -> _Effect: ...
def SetRebelTroops(*, value: _FloatParam) -> _Effect: ...
def SetStealth(*, value: _FloatParam) -> _Effect: ...
def CreateBuilding(*, type: str) -> _Effect: ...
def CreateShip(*, designname: str, species: str = "") -> _Effect: ...
def SetCapacity(*, partname: str, value: _FloatParam) -> _Effect: ...
def SetSecondaryStat(*, partname: str, value: _FloatParam) -> _Effect: ...
def SetMaxDamage(*, partname: str, value: _FloatParam) -> _Effect: ...
def SetMaxCapacity(*, partname: str, value: _FloatParam) -> _Effect: ...
def SetMaxSecondaryStat(*, partname: str, value: _FloatParam) -> _Effect: ...
def MoveTo(*, destination: _Condition) -> _Effect: ...
def MoveTowards(*, speed: int, target: _Condition) -> _Effect: ...
def SetStarType(*, type: _StarType) -> _Effect: ...
def SetEmpireMeter(*, empire: _Empire, meter: str, value: _FloatParam) -> _Effect: ...
def SetOwner(*, empire: _Empire) -> _Effect: ...
def GenerateSitRepMessage(
    *,
    message: str,
    label: str,
    empire: _Empire,
    parameters: dict[str, Any] = {},
    icon: str = "",
) -> _Effect: ...
def GiveEmpireTech(*, name: str, empire=_Empire()) -> _Effect: ...
def Victory(*, reason: str) -> _Effect: ...
