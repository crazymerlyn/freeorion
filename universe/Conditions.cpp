#include "Conditions.h"

#include <array>
#include <cfloat>
#include <boost/algorithm/cxx11/all_of.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/st_connected.hpp>
#include "BuildingType.h"
#include "Building.h"
#include "Field.h"
#include "Fighter.h"
#include "Fleet.h"
#include "Meter.h"
#include "ObjectMap.h"
#include "Pathfinder.h"
#include "Planet.h"
#include "ShipDesign.h"
#include "ShipHull.h"
#include "ShipPart.h"
#include "Ship.h"
#include "Special.h"
#include "Species.h"
#include "System.h"
#include "UniverseObject.h"
#include "Universe.h"
#include "ValueRefs.h"
#include "../Empire/EmpireManager.h"
#include "../Empire/Empire.h"
#include "../Empire/Supply.h"
#include "../util/Logger.h"
#include "../util/OptionsDB.h"
#include "../util/Random.h"
#include "../util/ScopedTimer.h"
#include "../util/i18n.h"


using boost::io::str;

bool UserStringExists(const std::string& str);

namespace {
    void AddOptions(OptionsDB& db)
    { db.Add<bool>("effects.move.test", UserStringNop("OPTIONS_DB_UI_EFFECTS_MOVE_TEST"), false); }
    bool temp_bool = RegisterOptions(&AddOptions);

    const std::string EMPTY_STRING;

    DeclareThreadSafeLogger(conditions);

    using boost::placeholders::_1;

    void AddAllObjectsSet(const ObjectMap& objects, Condition::ObjectSet& condition_non_targets) {
        condition_non_targets.reserve(condition_non_targets.size() + objects.ExistingObjects().size());
        for (const auto& obj : objects.ExistingObjects())
            condition_non_targets.push_back(obj.second);
        // in my tests, this range for loop with emplace_back was about 5% faster than std::transform with std::back_inserter and a lambda returning the .second of the map entries
    }

    void AddBuildingSet(const ObjectMap& objects, Condition::ObjectSet& condition_non_targets) {
        condition_non_targets.reserve(condition_non_targets.size() + objects.ExistingBuildings().size());
        for (const auto& obj : objects.ExistingBuildings())
            condition_non_targets.push_back(obj.second);
    }

    void AddFieldSet(const ObjectMap& objects, Condition::ObjectSet& condition_non_targets) {
        condition_non_targets.reserve(condition_non_targets.size() + objects.ExistingFields().size());
        for (const auto& obj : objects.ExistingFields())
            condition_non_targets.push_back(obj.second);
    }

    void AddFleetSet(const ObjectMap& objects, Condition::ObjectSet& condition_non_targets) {
        condition_non_targets.reserve(condition_non_targets.size() + objects.ExistingFleets().size());
        for (const auto& obj : objects.ExistingFleets())
            condition_non_targets.push_back(obj.second);
    }

    void AddPlanetSet(const ObjectMap& objects, Condition::ObjectSet& condition_non_targets) {
        condition_non_targets.reserve(condition_non_targets.size() + objects.ExistingPlanets().size());
        for (const auto& obj : objects.ExistingPlanets())
            condition_non_targets.push_back(obj.second);
    }

    void AddPopCenterSet(const ObjectMap& objects, Condition::ObjectSet& condition_non_targets) {
        condition_non_targets.reserve(condition_non_targets.size() + objects.ExistingPopCenters().size());
        for (const auto& obj : objects.ExistingPopCenters())
            condition_non_targets.push_back(obj.second);
    }

    void AddResCenterSet(const ObjectMap& objects, Condition::ObjectSet& condition_non_targets) {
        condition_non_targets.reserve(condition_non_targets.size() + objects.ExistingResourceCenters().size());
        for (const auto& obj : objects.ExistingResourceCenters())
            condition_non_targets.push_back(obj.second);
    }

    void AddShipSet(const ObjectMap& objects, Condition::ObjectSet& condition_non_targets) {
        condition_non_targets.reserve(condition_non_targets.size() + objects.ExistingShips().size());
        for (const auto& obj : objects.ExistingShips())
            condition_non_targets.push_back(obj.second);
    }

    void AddSystemSet(const ObjectMap& objects, Condition::ObjectSet& condition_non_targets) {
        condition_non_targets.reserve(condition_non_targets.size() + objects.ExistingSystems().size());
        for (const auto& obj : objects.ExistingSystems())
            condition_non_targets.push_back(obj.second);
    }

    /** Used by 4-parameter Condition::Eval function, and some of its
      * overrides, to scan through \a matches or \a non_matches set and apply
      * \a pred to each object, to test if it should remain in its current set
      * or be transferred from the \a search_domain specified set into the
      * other. */
    template <typename Pred>
    void EvalImpl(Condition::ObjectSet& matches, Condition::ObjectSet& non_matches,
                  Condition::SearchDomain search_domain, const Pred& pred)
    {
        bool domain_matches = search_domain == Condition::SearchDomain::MATCHES;
        auto& from_set = domain_matches ? matches : non_matches;
        auto& to_set = domain_matches ? non_matches : matches;

        // move objects into to_set (resets moved-from pointers)
        std::copy_if(std::make_move_iterator(from_set.begin()),
                     std::make_move_iterator(from_set.end()),
                     std::back_inserter(to_set),
                     [pred, domain_matches](const auto& o) { return pred(o) != domain_matches; });

        // erase the reset-pointer entries in from_set
        from_set.erase(std::remove_if(from_set.begin(), from_set.end(), [](const auto& o) { return !o; }),
                       from_set.end());
    }

    [[nodiscard]] std::vector<const Condition::Condition*> FlattenAndNestedConditions(
        const std::vector<const Condition::Condition*>& input_conditions)
    {
        std::vector<const Condition::Condition*> retval;
        retval.reserve(input_conditions.size() * 2);    // bit extra for some subconditions
        for (const Condition::Condition* condition : input_conditions) {
            if (const Condition::And* and_condition = dynamic_cast<const Condition::And*>(condition)) {
                auto flattened_operands = FlattenAndNestedConditions(and_condition->Operands());
                retval.insert(retval.end(), flattened_operands.begin(), flattened_operands.end());
            } else if (condition) {
                retval.push_back(condition);
            }
        }
        return retval;
    }

    [[nodiscard]] std::map<std::string, bool> ConditionDescriptionAndTest(
        const std::vector<const Condition::Condition*>& conditions,
        const ScriptingContext& parent_context,
        std::shared_ptr<const UniverseObject> candidate_object/* = nullptr*/)
    {
        std::map<std::string, bool> retval;

        std::vector<const Condition::Condition*> flattened_conditions;
        if (conditions.empty())
            return retval;
        else if (conditions.size() > 1 || dynamic_cast<const Condition::And*>(*conditions.begin()))
            flattened_conditions = FlattenAndNestedConditions(conditions);
        //else if (dynamic_cast<const Condition::Or*>(*conditions.begin()))
        //    flattened_conditions = FlattenOrNestedConditions(conditions);
        else
            flattened_conditions = conditions;

        for (const Condition::Condition* condition : flattened_conditions)
            retval.emplace(condition->Description(), condition->Eval(parent_context, candidate_object));
        return retval;
    }
}

namespace Condition {
[[nodiscard]] std::string ConditionFailedDescription(const std::vector<const Condition*>& conditions,
                                                     std::shared_ptr<const UniverseObject> candidate_object/* = nullptr*/,
                                                     std::shared_ptr<const UniverseObject> source_object/* = nullptr*/)
{
    if (conditions.empty())
        return UserString("NONE");

    std::string retval;

    // test candidate against all input conditions, and store descriptions of each
    ScriptingContext context{std::move(source_object)};
    for (const auto& result : ConditionDescriptionAndTest(conditions, context, std::move(candidate_object))) {
        if (!result.second)
             retval += UserString("FAILED") + " <rgba 255 0 0 255>" + result.first +"</rgba>\n";
    }

    // remove empty line from the end of the string
    retval = retval.substr(0, retval.length() - 1);

    return retval;
}

std::string ConditionDescription(const std::vector<const Condition*>& conditions,
                                 std::shared_ptr<const UniverseObject> candidate_object/* = nullptr*/,
                                 std::shared_ptr<const UniverseObject> source_object/* = nullptr*/)
{
    if (conditions.empty())
        return UserString("NONE");

    // test candidate against all input conditions, and store descriptions of each
    ScriptingContext context{std::move(source_object)};
    auto condition_description_and_test_results =
        ConditionDescriptionAndTest(conditions, context, std::move(candidate_object));

    bool all_conditions_match_candidate = true, at_least_one_condition_matches_candidate = false;
    for (const auto& result : condition_description_and_test_results) {
        all_conditions_match_candidate = all_conditions_match_candidate && result.second;
        at_least_one_condition_matches_candidate = at_least_one_condition_matches_candidate || result.second;
    }

    // concatenate (non-duplicated) single-description results
    std::string retval;
    if (conditions.size() > 1 || dynamic_cast<const And*>(*conditions.begin())) {
        retval += UserString("ALL_OF") + " ";
        retval += (all_conditions_match_candidate ? UserString("PASSED") : UserString("FAILED")) + "\n";
    } else if (dynamic_cast<const Or*>(*conditions.begin())) {
        retval += UserString("ANY_OF") + " ";
        retval += (at_least_one_condition_matches_candidate ? UserString("PASSED") : UserString("FAILED")) + "\n";
    }
    // else just output single condition description and PASS/FAIL text

    for (const auto& result : condition_description_and_test_results) {
        retval += (result.second ? UserString("PASSED") : UserString("FAILED"));
        retval += " " + result.first + "\n";
    }
    return retval;
}

#define CHECK_COND_VREF_MEMBER(m_ptr) { if (m_ptr == rhs_.m_ptr) {              \
                                            /* check next member */             \
                                        } else if (!m_ptr || !rhs_.m_ptr) {     \
                                            return false;                       \
                                        } else {                                \
                                            if (*m_ptr != *(rhs_.m_ptr))        \
                                                return false;                   \
                                        }   }

///////////////////////////////////////////////////////////
// Condition                                             //
///////////////////////////////////////////////////////////
struct Condition::MatchHelper {
    MatchHelper(const Condition* this_, const ScriptingContext& parent_context) :
        m_this(this_),
        m_parent_context(parent_context)
    {}

    bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
        ScriptingContext context{m_parent_context, candidate};
        return m_this->Match(context);
    }

    const Condition* m_this = nullptr;
    const ScriptingContext& m_parent_context;
};

Condition::~Condition() = default;

bool Condition::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;

    if (typeid(*this) != typeid(rhs))
        return false;

    return true;
}

void Condition::Eval(const ScriptingContext& parent_context,
                     ObjectSet& matches, ObjectSet& non_matches,
                     SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{ EvalImpl(matches, non_matches, search_domain, MatchHelper(this, parent_context)); }

void Condition::Eval(const ScriptingContext& parent_context,
                     Effect::TargetSet& matches, Effect::TargetSet& non_matches,
                     SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    if (!GetOptionsDB().Get<bool>("effects.move.test")) {
        // reinterpret sets of mutable objects as sets of non-mutable objects.
        auto& matches_as_objectset = reinterpret_cast<ObjectSet&>(matches);
        auto& non_matches_as_objectset = reinterpret_cast<ObjectSet&>(non_matches);
        this->Eval(parent_context, matches_as_objectset, non_matches_as_objectset, search_domain);

    } else {
        ObjectSet matches_as_objectset{std::make_move_iterator(matches.begin()),
                                       std::make_move_iterator(matches.end())};
        ObjectSet non_matches_as_objectset{std::make_move_iterator(non_matches.begin()),
                                           std::make_move_iterator(non_matches.end())};
        matches.clear();
        non_matches.clear();

        this->Eval(parent_context, matches_as_objectset, non_matches_as_objectset, search_domain);

        std::transform(std::make_move_iterator(matches_as_objectset.begin()),
                       std::make_move_iterator(matches_as_objectset.end()),
                       std::back_inserter(matches),
                       [](auto&& o) { return std::const_pointer_cast<UniverseObject>(std::move(o)); });
        std::transform(std::make_move_iterator(non_matches_as_objectset.begin()),
                       std::make_move_iterator(non_matches_as_objectset.end()),
                       std::back_inserter(non_matches),
                       [](auto&& o) { return std::const_pointer_cast<UniverseObject>(std::move(o)); });
    }
}

void Condition::Eval(const ScriptingContext& parent_context,
                     ObjectSet& matches) const
{
    matches.clear();
    ObjectSet condition_initial_candidates;

    // evaluate condition only on objects that could potentially be matched by the condition
    GetDefaultInitialCandidateObjects(parent_context, condition_initial_candidates);

    matches.reserve(condition_initial_candidates.size());
    Eval(parent_context, matches, condition_initial_candidates);
}

void Condition::Eval(const ScriptingContext& parent_context,
                     Effect::TargetSet& matches) const
{
    if (!GetOptionsDB().Get<bool>("effects.move.test")) {
        // reinterpret sets of mutable objects as sets of non-mutable objects.
        auto& matches_as_objectset = reinterpret_cast<ObjectSet&>(matches);
        this->Eval(parent_context, matches_as_objectset);

    } else {
        ObjectSet matches_as_objectset{std::make_move_iterator(matches.begin()),
                                       std::make_move_iterator(matches.end())};
        matches.clear();

        this->Eval(parent_context, matches_as_objectset);

        std::transform(std::make_move_iterator(matches_as_objectset.begin()),
                       std::make_move_iterator(matches_as_objectset.end()),
                       std::back_inserter(matches),
                       [](auto&& o) { return std::const_pointer_cast<UniverseObject>(std::move(o)); });
    }
}

bool Condition::Eval(const ScriptingContext& parent_context,
                     std::shared_ptr<const UniverseObject> candidate) const
{
    if (!candidate)
        return false;
    ObjectSet non_matches{std::move(candidate)}, matches;
    Eval(parent_context, matches, non_matches);
    return non_matches.empty(); // if candidate has been matched, non_matches will now be empty
}

void Condition::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                  ObjectSet& condition_non_targets) const
{ AddAllObjectsSet(parent_context.ContextObjects(), condition_non_targets); }

std::string Condition::Description(bool negated/* = false*/) const
{ return ""; }

std::string Condition::Dump(unsigned short ntabs) const
{ return ""; }

bool Condition::Match(const ScriptingContext& local_context) const
{ return false; }

///////////////////////////////////////////////////////////
// Number                                                //
///////////////////////////////////////////////////////////
Number::Number(std::unique_ptr<ValueRef::ValueRef<int>>&& low,
               std::unique_ptr<ValueRef::ValueRef<int>>&& high,
               std::unique_ptr<Condition>&& condition) :
    m_low(std::move(low)),
    m_high(std::move(high)),
    m_condition(std::move(condition))
{
    std::array<const ValueRef::ValueRefBase*, 2> operands = {{m_low.get(), m_high.get()}};
    m_root_candidate_invariant =
        m_condition->RootCandidateInvariant() &&
        boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant =
        m_condition->TargetInvariant() &&
        boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant =
        m_condition->SourceInvariant() &&
        boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

bool Number::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const Number& rhs_ = static_cast<const Number&>(rhs);

    CHECK_COND_VREF_MEMBER(m_low)
    CHECK_COND_VREF_MEMBER(m_high)
    CHECK_COND_VREF_MEMBER(m_condition)

    return true;
}

std::string Number::Description(bool negated/* = false*/) const {
    std::string low_str = (m_low ? (m_low->ConstantExpr() ?
                                    std::to_string(m_low->Eval()) :
                                    m_low->Description())
                                 : "0");
    std::string high_str = (m_high ? (m_high->ConstantExpr() ?
                                      std::to_string(m_high->Eval()) :
                                      m_high->Description())
                                   : std::to_string(INT_MAX));

    const std::string& description_str = (!negated)
        ? UserString("DESC_NUMBER")
        : UserString("DESC_NUMBER_NOT");
    return str(FlexibleFormat(description_str)
               % low_str
               % high_str
               % m_condition->Description());
}

std::string Number::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "Number";
    if (m_low)
        retval += " low = " + m_low->Dump(ntabs);
    if (m_high)
        retval += " high = " + m_high->Dump(ntabs);
    retval += " condition =\n";
    retval += m_condition->Dump(ntabs+1);
    return retval;
}

void Number::Eval(const ScriptingContext& parent_context,
                  ObjectSet& matches, ObjectSet& non_matches,
                  SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    // Number does not have a single valid local candidate to be matched, as it
    // will match anything if the proper number of objects match the subcondition.

    if (!(
                (!m_low  || m_low->LocalCandidateInvariant())
             && (!m_high || m_high->LocalCandidateInvariant())
         )
       )
    {
        ErrorLogger() << "Condition::Number::Eval has local candidate-dependent ValueRefs, but no valid local candidate!";
    } else if (
                !parent_context.condition_root_candidate
                && !(
                        (!m_low  || m_low->RootCandidateInvariant())
                     && (!m_high || m_high->RootCandidateInvariant())
                    )
              )
    {
        ErrorLogger() << "Condition::Number::Eval has root candidate-dependent ValueRefs, but expects local candidate to be the root candidate, and has no valid local candidate!";
    }

    if (!parent_context.condition_root_candidate && !this->RootCandidateInvariant()) {
        // no externally-defined root candidate, so each object matched must
        // separately act as a root candidate, and sub-condition must be re-
        // evaluated for each tested object and the number of objects matched
        // checked for each object being tested
        Condition::Eval(parent_context, matches, non_matches, search_domain);

    } else {
        // Matching for this condition doesn't need to check each candidate object against
        // the number of subcondition matches, so don't need to use EvalImpl
        bool in_range = Match(parent_context);

        // transfer objects to or from candidate set, according to whether
        // number of matches was within the requested range.
        if (search_domain == SearchDomain::MATCHES && !in_range) {
            // move all objects from matches to non_matches
            std::move(matches.begin(), matches.end(), std::back_inserter(non_matches));
            matches.clear();
        } else if (search_domain == SearchDomain::NON_MATCHES && in_range) {
            // move all objects from non_matches to matches
            std::move(non_matches.begin(), non_matches.end(), std::back_inserter(matches));
            non_matches.clear();
        }
    }
}

bool Number::Match(const ScriptingContext& local_context) const {
    // get set of all UniverseObjects that satisfy m_condition
    ObjectSet condition_matches;
    m_condition->Eval(local_context, condition_matches);

    // compare number of objects that satisfy m_condition to the acceptable range of such objects
    int matched = condition_matches.size();

    // get acceptable range of subcondition matches for candidate
    int low = (m_low ? std::max(0, m_low->Eval(local_context)) : 0);
    if (low > matched)
        return false;
    int high = (m_high ? std::min(m_high->Eval(local_context), INT_MAX) : INT_MAX);
    return matched <= high;
}

void Number::SetTopLevelContent(const std::string& content_name) {
    if (m_low)
        m_low->SetTopLevelContent(content_name);
    if (m_high)
        m_high->SetTopLevelContent(content_name);
    if (m_condition)
        m_condition->SetTopLevelContent(content_name);
}

unsigned int Number::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Number");
    CheckSums::CheckSumCombine(retval, m_low);
    CheckSums::CheckSumCombine(retval, m_high);
    CheckSums::CheckSumCombine(retval, m_condition);

    TraceLogger() << "GetCheckSum(Number): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Number::Clone() const {
    return std::make_unique<Number>(ValueRef::CloneUnique(m_low),
                                    ValueRef::CloneUnique(m_high),
                                    ValueRef::CloneUnique(m_condition));
}

///////////////////////////////////////////////////////////
// Turn                                                  //
///////////////////////////////////////////////////////////
Turn::Turn(std::unique_ptr<ValueRef::ValueRef<int>>&& low,
           std::unique_ptr<ValueRef::ValueRef<int>>&& high) :
    m_low(std::move(low)),
    m_high(std::move(high))
{
    std::array<const ValueRef::ValueRefBase*, 2> operands = {{m_low.get(), m_high.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

bool Turn::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const Turn& rhs_ = static_cast<const Turn&>(rhs);

    CHECK_COND_VREF_MEMBER(m_low)
    CHECK_COND_VREF_MEMBER(m_high)

    return true;
}

void Turn::Eval(const ScriptingContext& parent_context,
                ObjectSet& matches, ObjectSet& non_matches,
                SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    // if ValueRef for low or high range limits depend on local candidate, then
    // they must be evaluated per-candidate.
    // if there already is a root candidate, then this condition's parameters
    // can be evaluated assuming it will not change.
    // if there is no root candidate in the parent context, then this
    // condition's candidates will be the root candidates, and this condition's
    // parameters must be root candidate invariant or else must be evaluated
    // per-candidate
    bool simple_eval_safe = ((!m_low || m_low->LocalCandidateInvariant()) &&
                             (!m_high || m_high->LocalCandidateInvariant()) &&
                             (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // Matching for this condition doesn't need to check each candidate object against
        // the turn number separately, so don't need to use EvalImpl
        bool match = Match(parent_context);

        // transfer objects to or from candidate set, according to whether the
        // current turn was within the requested range.
        if (search_domain == SearchDomain::MATCHES && !match) {
            // move all objects from matches to non_matches
            std::move(matches.begin(), matches.end(), std::back_inserter(non_matches));
            matches.clear();
        } else if (search_domain == SearchDomain::NON_MATCHES && match) {
            // move all objects from non_matches to matches
            std::move(non_matches.begin(), non_matches.end(), std::back_inserter(matches));
            non_matches.clear();
        }
    } else {
        // re-evaluate allowed turn range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string Turn::Description(bool negated/* = false*/) const {
    std::string low_str;
    if (m_low)
        low_str = (m_low->ConstantExpr() ?
                   std::to_string(m_low->Eval()) :
                   m_low->Description());
    std::string high_str;
    if (m_high)
        high_str = (m_high->ConstantExpr() ?
                    std::to_string(m_high->Eval()) :
                    m_high->Description());
    std::string description_str;

    if (m_low && m_high) {
        description_str = (!negated)
            ? UserString("DESC_TURN")
            : UserString("DESC_TURN_NOT");
        return str(FlexibleFormat(description_str)
                   % low_str
                   % high_str);

    } else if (m_low) {
        description_str = (!negated)
            ? UserString("DESC_TURN_MIN_ONLY")
            : UserString("DESC_TURN_MIN_ONLY_NOT");
        return str(FlexibleFormat(description_str)
                   % low_str);

    } else if (m_high) {
        description_str = (!negated)
            ? UserString("DESC_TURN_MAX_ONLY")
            : UserString("DESC_TURN_MAX_ONLY_NOT");
        return str(FlexibleFormat(description_str)
                   % high_str);

    } else {
        return (!negated)
            ? UserString("DESC_TURN_ANY")
            : UserString("DESC_TURN_ANY_NOT");
    }
}

std::string Turn::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "Turn";
    if (m_low)
        retval += " low = " + m_low->Dump(ntabs);
    if (m_high)
        retval += " high = " + m_high->Dump(ntabs);
    retval += "\n";
    return retval;
}

bool Turn::Match(const ScriptingContext& local_context) const {
    int turn = local_context.current_turn;
    int low =  (m_low ?  std::max(BEFORE_FIRST_TURN,           m_low->Eval(local_context)) : BEFORE_FIRST_TURN);
    if (low > turn)
        return false;
    int high = (m_high ? std::min(m_high->Eval(local_context), IMPOSSIBLY_LARGE_TURN) :      IMPOSSIBLY_LARGE_TURN);
    return turn <= high;
}

void Turn::SetTopLevelContent(const std::string& content_name) {
    if (m_low)
        m_low->SetTopLevelContent(content_name);
    if (m_high)
        m_high->SetTopLevelContent(content_name);
}

unsigned int Turn::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Turn");
    CheckSums::CheckSumCombine(retval, m_low);
    CheckSums::CheckSumCombine(retval, m_high);

    TraceLogger() << "GetCheckSum(Turn): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Turn::Clone() const {
    return std::make_unique<Turn>(ValueRef::CloneUnique(m_low),
                                  ValueRef::CloneUnique(m_high));
}

///////////////////////////////////////////////////////////
// SortedNumberOf                                        //
///////////////////////////////////////////////////////////
SortedNumberOf::SortedNumberOf(std::unique_ptr<ValueRef::ValueRef<int>>&& number,
                               std::unique_ptr<Condition>&& condition) :
    SortedNumberOf(std::move(number), nullptr, SortingMethod::SORT_RANDOM, std::move(condition))
{}

SortedNumberOf::SortedNumberOf(std::unique_ptr<ValueRef::ValueRef<int>>&& number,
                               std::unique_ptr<ValueRef::ValueRef<double>>&& sort_key_ref,
                               SortingMethod sorting_method,
                               std::unique_ptr<Condition>&& condition) :
    m_number(std::move(number)),
    m_sort_key(std::move(sort_key_ref)),
    m_sorting_method(sorting_method),
    m_condition(std::move(condition))
{
    std::array<const ValueRef::ValueRefBase*, 2> operands = {{m_number.get(), m_sort_key.get()}};
    m_root_candidate_invariant =
        boost::algorithm::all_of(operands, [](const auto& e) { return !e || e->RootCandidateInvariant(); }) &&
        (!m_condition || m_condition->RootCandidateInvariant());
    m_target_invariant =
        boost::algorithm::all_of(operands, [](const auto& e) { return !e || e->TargetInvariant(); }) &&
        (!m_condition || m_condition->TargetInvariant());
    m_source_invariant =
        boost::algorithm::all_of(operands, [](const auto& e) { return !e || e->SourceInvariant(); }) &&
        (!m_condition || m_condition->SourceInvariant());
}

bool SortedNumberOf::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const SortedNumberOf& rhs_ = static_cast<const SortedNumberOf&>(rhs);

    if (m_sorting_method != rhs_.m_sorting_method)
        return false;

    CHECK_COND_VREF_MEMBER(m_number)
    CHECK_COND_VREF_MEMBER(m_sort_key)
    CHECK_COND_VREF_MEMBER(m_condition)

    return true;
}

namespace {
    /** Transfers the indicated \a number of objects, randomly selected from from_set to to_set */
    void TransferRandomObjects(unsigned int number, ObjectSet& from_set, ObjectSet& to_set) {
        // ensure number of objects to be moved is within reasonable range
        number = std::min<unsigned int>(number, from_set.size());
        if (number == 0)
            return;

        // create list of bool flags to indicate whether each item in from_set
        // with corresponding place in iteration order should be transfered
        std::vector<bool> transfer_flags(from_set.size(), false);   // initialized to all false

        // set first  number  flags to true
        std::fill_n(transfer_flags.begin(), number, true);

        // shuffle flags to randomize which flags are set
        RandomShuffle(transfer_flags);

        // transfer objects that have been flagged
        int i = 0;
        for (auto it = from_set.begin(); it != from_set.end(); ++i) {
            if (transfer_flags[i]) {
                to_set.push_back(std::move(*it));
                *it = std::move(from_set.back());
                from_set.pop_back();
            } else {
                ++it;
            }
        }
    }

    /** Transfers the indicated \a number of objects, selected from \a from_set
      * into \a to_set.  The objects transferred are selected based on the value
      * of \a sort_key evaluated on them, with the largest / smallest / most
      * common sort keys chosen, or a random selection chosen, depending on the
      * specified \a sorting_method */
    void TransferSortedObjects(unsigned int number, ValueRef::ValueRef<double>* sort_key,
                               const ScriptingContext& context, SortingMethod sorting_method,
                               ObjectSet& from_set, ObjectSet& to_set)
    {
        // handle random case, which doesn't need sorting key
        if (sorting_method == SortingMethod::SORT_RANDOM) {
            TransferRandomObjects(number, from_set, to_set);
            return;
        }

        // for other SoringMethods, need sort key values
        if (!sort_key) {
            ErrorLogger() << "TransferSortedObjects given null sort_key";
            return;
        }

        // get sort key values for all objects in from_set, and sort by inserting into map
        std::multimap<float, std::shared_ptr<const UniverseObject>> sort_key_objects;
        for (auto& from : from_set) {
            ScriptingContext source_context{context, from};
            float sort_value = sort_key->Eval(source_context);
            sort_key_objects.emplace(sort_value, from);
        }

        // how many objects to select?
        number = std::min<unsigned int>(number, sort_key_objects.size());
        if (number == 0)
            return;
        unsigned int number_transferred(0);

        // pick max / min / most common values
        if (sorting_method == SortingMethod::SORT_MIN) {
            // move (number) objects with smallest sort key (at start of map)
            // from the from_set into the to_set.
            for ([[maybe_unused]] auto& [ignored_float, object_to_transfer] : sort_key_objects) {
                (void)ignored_float;    // quiet unused variable warning
                auto from_it = std::find(from_set.begin(), from_set.end(), object_to_transfer);
                if (from_it != from_set.end()) {
                    *from_it = std::move(from_set.back());
                    from_set.pop_back();
                    to_set.push_back(std::move(object_to_transfer));
                    number_transferred++;
                    if (number_transferred >= number)
                        return;
                }
            }

        } else if (sorting_method == SortingMethod::SORT_MAX) {
            // move (number) objects with largest sort key (at end of map)
            // from the from_set into the to_set.
            for (auto sorted_it = sort_key_objects.rbegin();  // would use const_reverse_iterator but this causes a compile error in some compilers
                 sorted_it != sort_key_objects.rend(); ++sorted_it)
            {
                auto& object_to_transfer = sorted_it->second;
                auto from_it = std::find(from_set.begin(), from_set.end(), object_to_transfer);
                if (from_it != from_set.end()) {
                    *from_it = std::move(from_set.back());
                    from_set.pop_back();
                    to_set.push_back(std::move(object_to_transfer));
                    number_transferred++;
                    if (number_transferred >= number)
                        return;
                }
            }

        } else if (sorting_method == SortingMethod::SORT_MODE) {
            // compile histogram of of number of times each sort key occurs
            std::map<float, unsigned int> histogram;
            for ([[maybe_unused]] auto& [key, ignored_object] : sort_key_objects) {
                (void)ignored_object;
                histogram[key]++;
            }

            // invert histogram to index by number of occurances
            std::multimap<unsigned int, float> inv_histogram;
            for (const auto& [key, count] : histogram)
                inv_histogram.emplace(count, key);

            // reverse-loop through inverted histogram to find which sort keys
            // occurred most frequently, and transfer objects with those sort
            // keys from from_set to to_set.
            for (auto inv_hist_it = inv_histogram.rbegin();  // would use const_reverse_iterator but this causes a compile error in some compilers
                 inv_hist_it != inv_histogram.rend(); ++inv_hist_it)
            {
                float cur_sort_key = inv_hist_it->second;

                // get range of objects with the current sort key
                auto key_range = sort_key_objects.equal_range(cur_sort_key);

                // loop over range, selecting objects to transfer from from_set to to_set
                for (auto sorted_it = key_range.first;
                     sorted_it != key_range.second; ++sorted_it)
                {
                    auto& object_to_transfer = sorted_it->second;
                    auto from_it = std::find(from_set.begin(), from_set.end(), object_to_transfer);
                    if (from_it != from_set.end()) {
                        *from_it = from_set.back();
                        from_set.pop_back();
                        to_set.push_back(std::move(object_to_transfer));
                        number_transferred++;
                        if (number_transferred >= number)
                            return;
                    }
                }
            }

        } else {
             ErrorLogger() << "TransferSortedObjects given unknown sort method";
        }
    }
}

void SortedNumberOf::Eval(const ScriptingContext& parent_context,
                          ObjectSet& matches, ObjectSet& non_matches,
                          SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    // Most conditions match objects independently of the other objects being
    // tested, but the number parameter for NumberOf conditions makes things
    // more complicated.  In order to match some number of the potential
    // matches property, both the matches and non_matches need to be checked
    // against the subcondition, and the total number of subcondition matches
    // counted.
    // Then, when searching NON_MATCHES, non_matches may be moved into matches
    // so that the number of subcondition matches in matches is equal to the
    // requested number.  There may also be subcondition non-matches in
    // matches, but these are not counted or affected by this condition.
    // Or, when searching MATCHES, matches may be moved into non_matches so
    // that the number of subcondition matches in matches is equal to the
    // requested number.  There again may be subcondition non-matches in
    // matches, but these are also not counted or affected by this condition.

    // SortedNumberOf does not have a valid local candidate to be matched
    // before the subcondition is evaluated, so the local context that is
    // passed to the subcondition should have a null local candidate.
    std::shared_ptr<const UniverseObject> no_object;
    ScriptingContext local_context{parent_context, no_object};

    // which input matches match the subcondition?
    ObjectSet subcondition_matching_matches;
    subcondition_matching_matches.reserve(matches.size());
    m_condition->Eval(local_context, subcondition_matching_matches, matches, SearchDomain::NON_MATCHES);

    // remaining input matches don't match the subcondition...
    ObjectSet subcondition_non_matching_matches = matches;
    matches.clear();    // to be refilled later

    // which input non_matches match the subcondition?
    ObjectSet subcondition_matching_non_matches;
    subcondition_matching_non_matches.reserve(non_matches.size());
    m_condition->Eval(local_context, subcondition_matching_non_matches, non_matches, SearchDomain::NON_MATCHES);

    // remaining input non_matches don't match the subcondition...
    ObjectSet subcondition_non_matching_non_matches = non_matches;
    non_matches.clear();    // to be refilled later

    // assemble single set of subcondition matching objects
    ObjectSet all_subcondition_matches;
    all_subcondition_matches.reserve(subcondition_matching_matches.size() + subcondition_matching_non_matches.size());
    all_subcondition_matches.insert(all_subcondition_matches.end(),
                                    subcondition_matching_matches.begin(),
                                    subcondition_matching_matches.end());
    all_subcondition_matches.insert(all_subcondition_matches.end(),
                                    subcondition_matching_non_matches.begin(),
                                    subcondition_matching_non_matches.end());

    // how many subcondition matches to select as matches to this condition
    int number = m_number->Eval(local_context);

    // compile single set of all objects that are matched by this condition.
    // these are the objects that should be transferred from non_matches into
    // matches, or those left in matches while the rest are moved into non_matches
    ObjectSet matched_objects;
    matched_objects.reserve(number);
    TransferSortedObjects(number, m_sort_key.get(), parent_context, m_sorting_method,
                          all_subcondition_matches, matched_objects);

    // put objects back into matches and non_target sets as output...

    if (search_domain == SearchDomain::NON_MATCHES) {
        // put matched objects that are in subcondition_matching_non_matches into matches
        for (auto& matched_object : matched_objects) {
            // is this matched object in subcondition_matching_non_matches?
            auto smnt_it = std::find(subcondition_matching_non_matches.begin(),
                                     subcondition_matching_non_matches.end(), matched_object);
            if (smnt_it != subcondition_matching_non_matches.end()) {
                // yes; move object to matches
                *smnt_it = subcondition_matching_non_matches.back();
                subcondition_matching_non_matches.pop_back();
                matches.push_back(matched_object);   // TODO: can I std::move ?
            }
        }

        // put remaining (non-matched) objects in subcondition_matching_non_matches back into non_matches
        non_matches.insert( non_matches.end(), subcondition_matching_non_matches.begin(),      subcondition_matching_non_matches.end());
        // put objects in subcondition_non_matching_non_matches back into non_matches
        non_matches.insert( non_matches.end(), subcondition_non_matching_non_matches.begin(),  subcondition_non_matching_non_matches.end());
        // put objects in subcondition_matching_matches and subcondition_non_matching_matches back into matches
        matches.insert(     matches.end(),     subcondition_matching_matches.begin(),          subcondition_matching_matches.end());
        matches.insert(     matches.end(),     subcondition_non_matching_matches.begin(),      subcondition_non_matching_matches.end());
        // this leaves the original contents of matches unchanged, other than
        // possibly having transferred some objects into matches from non_matches

    } else { /*(search_domain == SearchDomain::MATCHES)*/
        // put matched objecs that are in subcondition_matching_matches back into matches
        for (auto& matched_object : matched_objects) {
            // is this matched object in subcondition_matching_matches?
            auto smt_it = std::find(subcondition_matching_matches.begin(),
                                    subcondition_matching_matches.end(), matched_object);
            if (smt_it != subcondition_matching_matches.end()) {
                // yes; move back into matches
                *smt_it = subcondition_matching_matches.back();
                subcondition_matching_matches.pop_back();
                matches.push_back(matched_object);   // TODO: can I std::move ?
            }
        }

        // put remaining (non-matched) objects in subcondition_matching_matches) into non_matches
        non_matches.insert( non_matches.end(), subcondition_matching_matches.begin(),          subcondition_matching_matches.end());
        // put objects in subcondition_non_matching_matches into non_matches
        non_matches.insert( non_matches.end(), subcondition_non_matching_matches.begin(),      subcondition_non_matching_matches.end());
        // put objects in subcondition_matching_non_matches and subcondition_non_matching_non_matches back into non_matches
        non_matches.insert( non_matches.end(), subcondition_matching_non_matches.begin(),      subcondition_matching_non_matches.end());
        non_matches.insert( non_matches.end(), subcondition_non_matching_non_matches.begin(),  subcondition_non_matching_non_matches.end());
        // this leaves the original contents of non_matches unchanged, other than
        // possibly having transferred some objects into non_matches from matches
    }
}

std::string SortedNumberOf::Description(bool negated/* = false*/) const {
    std::string number_str = m_number->ConstantExpr() ? m_number->Dump() : m_number->Description();

    if (m_sorting_method == SortingMethod::SORT_RANDOM) {
        return str(FlexibleFormat((!negated)
                                  ? UserString("DESC_NUMBER_OF")
                                  : UserString("DESC_NUMBER_OF_NOT")
                                 )
                   % number_str
                   % m_condition->Description());
    } else {
        std::string sort_key_str = m_sort_key->ConstantExpr() ? m_sort_key->Dump() : m_sort_key->Description();

        std::string description_str;
        switch (m_sorting_method) {
        case SortingMethod::SORT_MAX:
            description_str = (!negated)
                ? UserString("DESC_MAX_NUMBER_OF")
                : UserString("DESC_MAX_NUMBER_OF_NOT");
            break;

        case SortingMethod::SORT_MIN:
            description_str = (!negated)
                ? UserString("DESC_MIN_NUMBER_OF")
                : UserString("DESC_MIN_NUMBER_OF_NOT");
            break;

        case SortingMethod::SORT_MODE:
            description_str = (!negated)
                ? UserString("DESC_MODE_NUMBER_OF")
                : UserString("DESC_MODE_NUMBER_OF_NOT");
            break;
        default:
            break;
        }

        return str(FlexibleFormat(description_str)
                   % number_str
                   % sort_key_str
                   % m_condition->Description());
    }
}

std::string SortedNumberOf::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs);
    switch (m_sorting_method) {
    case SortingMethod::SORT_RANDOM:
        retval += "NumberOf";   break;
    case SortingMethod::SORT_MAX:
        retval += "MaximumNumberOf";  break;
    case SortingMethod::SORT_MIN:
        retval += "MinimumNumberOf"; break;
    case SortingMethod::SORT_MODE:
        retval += "ModeNumberOf"; break;
    default:
        retval += "??NumberOf??"; break;
    }

    retval += " number = " + m_number->Dump(ntabs);

    if (m_sort_key)
         retval += " sortby = " + m_sort_key->Dump(ntabs);

    retval += " condition =\n";
    retval += m_condition->Dump(ntabs+1);

    return retval;
}

void SortedNumberOf::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                       ObjectSet& condition_non_targets) const
{
    if (m_condition) {
        m_condition->GetDefaultInitialCandidateObjects(parent_context, condition_non_targets);
    } else {
        Condition::GetDefaultInitialCandidateObjects(parent_context, condition_non_targets);
    }
}

void SortedNumberOf::SetTopLevelContent(const std::string& content_name) {
    if (m_number)
        m_number->SetTopLevelContent(content_name);
    if (m_sort_key)
        m_sort_key->SetTopLevelContent(content_name);
    if (m_condition)
        m_condition->SetTopLevelContent(content_name);
}

unsigned int SortedNumberOf::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::SortedNumberOf");
    CheckSums::CheckSumCombine(retval, m_number);
    CheckSums::CheckSumCombine(retval, m_sort_key);
    CheckSums::CheckSumCombine(retval, m_sorting_method);
    CheckSums::CheckSumCombine(retval, m_condition);

    TraceLogger() << "GetCheckSum(SortedNumberOf): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> SortedNumberOf::Clone() const {
    return std::make_unique<SortedNumberOf>(ValueRef::CloneUnique(m_number),
                                            ValueRef::CloneUnique(m_sort_key),
                                            m_sorting_method,
                                            ValueRef::CloneUnique(m_condition));
}

///////////////////////////////////////////////////////////
// All                                                   //
///////////////////////////////////////////////////////////
All::All() :
    Condition()
{
    m_root_candidate_invariant = true;
    m_target_invariant = true;
    m_source_invariant = true;
}

void All::Eval(const ScriptingContext& parent_context,
               ObjectSet& matches, ObjectSet& non_matches,
               SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    if (search_domain == SearchDomain::NON_MATCHES) {
        // move all objects from non_matches to matches
        std::move(non_matches.begin(), non_matches.end(), std::back_inserter(matches));
        non_matches.clear();
    }
    // if search_comain is MATCHES, do nothing: all objects in matches set
    // match this condition, so should remain in matches set
}

bool All::operator==(const Condition& rhs) const
{ return Condition::operator==(rhs); }

std::string All::Description(bool negated/* = false*/) const {
    return (!negated)
        ? UserString("DESC_ALL")
        : UserString("DESC_ALL_NOT");
}

std::string All::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "All\n"; }

unsigned int All::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::All");

    TraceLogger() << "GetCheckSum(All): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> All::Clone() const
{ return std::make_unique<All>(); }

///////////////////////////////////////////////////////////
// None                                                  //
///////////////////////////////////////////////////////////
None::None() :
    Condition()
{
    m_root_candidate_invariant = true;
    m_target_invariant = true;
    m_source_invariant = true;
}

void None::Eval(const ScriptingContext& parent_context,
                ObjectSet& matches, ObjectSet& non_matches,
                SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    if (search_domain == SearchDomain::MATCHES) {
        // move all objects from matches to non_matches
        std::move(matches.begin(), matches.end(), std::back_inserter(non_matches));
        matches.clear();
    }
    // if search domain is non_matches, no need to do anything since none of them match None.
}

bool None::operator==(const Condition& rhs) const
{ return Condition::operator==(rhs); }

std::string None::Description(bool negated/* = false*/) const {
    return (!negated)
        ? UserString("DESC_NONE")
        : UserString("DESC_NONE_NOT");
}

std::string None::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "None\n"; }

unsigned int None::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::None");

    TraceLogger() << "GetCheckSum(None): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> None::Clone() const
{ return std::make_unique<None>(); }

///////////////////////////////////////////////////////////
// NoOp                                                  //
///////////////////////////////////////////////////////////
NoOp::NoOp() :
    Condition()
{
    m_root_candidate_invariant = true;
    m_target_invariant = true;
    m_source_invariant = true;
}

void NoOp::Eval(const ScriptingContext& parent_context,
                ObjectSet& matches, ObjectSet& non_matches,
                SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    // does not modify input ObjectSets
    DebugLogger() << "NoOp::Eval(" << matches.size() << " input matches, " << non_matches.size() << " input non-matches)";
}

bool NoOp::operator==(const Condition& rhs) const
{ return Condition::operator==(rhs); }

std::string NoOp::Description(bool negated/* = false*/) const
{ return UserString("DESC_NOOP"); }

std::string NoOp::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "NoOp\n"; }

unsigned int NoOp::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::NoOp");

    TraceLogger(conditions) << "GetCheckSum(NoOp): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> NoOp::Clone() const
{ return std::make_unique<NoOp>(); }

///////////////////////////////////////////////////////////
// EmpireAffiliation                                     //
///////////////////////////////////////////////////////////
EmpireAffiliation::EmpireAffiliation(std::unique_ptr<ValueRef::ValueRef<int>>&& empire_id,
                                     EmpireAffiliationType affiliation) :
    m_empire_id(std::move(empire_id)),
    m_affiliation(affiliation)
{
    m_root_candidate_invariant = !m_empire_id || m_empire_id->RootCandidateInvariant();
    m_target_invariant = !m_empire_id || m_empire_id->TargetInvariant();
    m_source_invariant = !m_empire_id || m_empire_id->SourceInvariant();
}

EmpireAffiliation::EmpireAffiliation(std::unique_ptr<ValueRef::ValueRef<int>>&& empire_id) :
    EmpireAffiliation(std::move(empire_id), EmpireAffiliationType::AFFIL_SELF)
{}

EmpireAffiliation::EmpireAffiliation(EmpireAffiliationType affiliation) :
    EmpireAffiliation(nullptr, affiliation)
{}

bool EmpireAffiliation::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const EmpireAffiliation& rhs_ = static_cast<const EmpireAffiliation&>(rhs);

    if (m_affiliation != rhs_.m_affiliation)
        return false;

    CHECK_COND_VREF_MEMBER(m_empire_id)

    return true;
}

namespace {
    struct EmpireAffiliationSimpleMatch {
        EmpireAffiliationSimpleMatch(int empire_id, EmpireAffiliationType affiliation,
                                     const ScriptingContext& context) :
            m_empire_id(empire_id),
            m_affiliation(affiliation),
            m_context(context)
        {}

        EmpireAffiliationSimpleMatch(EmpireAffiliationType affiliation,
                                     const ScriptingContext& context) :
            m_affiliation(affiliation),
            m_context(context)
        {}

        // is it necessary to evaluate and pass in a non-default empire id?
        static bool AffiliationTypeUsesEmpireID(EmpireAffiliationType affiliation) {
            switch (affiliation) {
            case EmpireAffiliationType::AFFIL_SELF:
            case EmpireAffiliationType::AFFIL_ENEMY:
            case EmpireAffiliationType::AFFIL_PEACE:
            case EmpireAffiliationType::AFFIL_ALLY:
                return true;
            case EmpireAffiliationType::AFFIL_ANY:
            case EmpireAffiliationType::AFFIL_NONE:
            case EmpireAffiliationType::AFFIL_CAN_SEE: // TODO: update once implemented below
            case EmpireAffiliationType::AFFIL_HUMAN:
            default:
                return false;
            }
        }

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            switch (m_affiliation) {
            case EmpireAffiliationType::AFFIL_SELF:
                return m_empire_id != ALL_EMPIRES && candidate->OwnedBy(m_empire_id);
                break;

            case EmpireAffiliationType::AFFIL_ENEMY: {
                if (m_empire_id == ALL_EMPIRES)
                    return true;
                if (m_empire_id == candidate->Owner())
                    return false;
                DiplomaticStatus status = m_context.ContextDiploStatus(m_empire_id, candidate->Owner());
                return (status == DiplomaticStatus::DIPLO_WAR);
                break;
            }

            case EmpireAffiliationType::AFFIL_PEACE: {
                if (m_empire_id == ALL_EMPIRES)
                    return false;
                if (m_empire_id == candidate->Owner())
                    return false;
                DiplomaticStatus status = m_context.ContextDiploStatus(m_empire_id, candidate->Owner());
                return (status == DiplomaticStatus::DIPLO_PEACE);
                break;
            }

            case EmpireAffiliationType::AFFIL_ALLY: {
                if (m_empire_id == ALL_EMPIRES)
                    return false;
                if (m_empire_id == candidate->Owner())
                    return false;
                DiplomaticStatus status = m_context.ContextDiploStatus(m_empire_id, candidate->Owner());
                return (status >= DiplomaticStatus::DIPLO_ALLIED);
                break;
            }

            case EmpireAffiliationType::AFFIL_ANY:
                return !candidate->Unowned();
                break;

            case EmpireAffiliationType::AFFIL_NONE:
                return candidate->Unowned();
                break;

            case EmpireAffiliationType::AFFIL_CAN_SEE: {
                return false; // TODO
                break;
            }

            case EmpireAffiliationType::AFFIL_HUMAN: {
                if (candidate->Unowned())
                    return false;
                if (GetEmpireClientType(candidate->Owner()) == Networking::ClientType::CLIENT_TYPE_HUMAN_PLAYER)
                    return true;
                return false;
                break;
            }

            default:
                return false;
                break;
            }
        }

        int m_empire_id = ALL_EMPIRES;
        EmpireAffiliationType m_affiliation;
        const ScriptingContext& m_context;
    };
}

void EmpireAffiliation::Eval(const ScriptingContext& parent_context,
                             ObjectSet& matches, ObjectSet& non_matches,
                             SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = (!m_empire_id || m_empire_id->ConstantExpr()) ||
                            ((!m_empire_id || m_empire_id->LocalCandidateInvariant()) &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // evaluate empire id once, and use to check all candidate objects
        int empire_id = m_empire_id ? m_empire_id->Eval(parent_context) : ALL_EMPIRES;
        EvalImpl(matches, non_matches, search_domain, EmpireAffiliationSimpleMatch(empire_id, m_affiliation,
                                                                                   parent_context));
    } else {
        // re-evaluate empire id for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string EmpireAffiliation::Description(bool negated/* = false*/) const {
    std::string empire_str;
    if (m_empire_id) {
        int empire_id = ALL_EMPIRES;
        if (m_empire_id->ConstantExpr())
            empire_id = m_empire_id->Eval();
        if (const Empire* empire = GetEmpire(empire_id))
            empire_str = empire->Name();
        else
            empire_str = m_empire_id->Description();
    }

    if (m_affiliation == EmpireAffiliationType::AFFIL_SELF) {
        return str(FlexibleFormat((!negated)
            ? UserString("DESC_EMPIRE_AFFILIATION_SELF")
            : UserString("DESC_EMPIRE_AFFILIATION_SELF_NOT")) % empire_str);
    } else if (m_affiliation == EmpireAffiliationType::AFFIL_ANY) {
        return (!negated)
            ? UserString("DESC_EMPIRE_AFFILIATION_ANY")
            : UserString("DESC_EMPIRE_AFFILIATION_ANY_NOT");
    } else if (m_affiliation == EmpireAffiliationType::AFFIL_NONE) {
        return (!negated)
            ? UserString("DESC_EMPIRE_AFFILIATION_ANY_NOT")
            : UserString("DESC_EMPIRE_AFFILIATION_ANY");
    } else {
        return str(FlexibleFormat((!negated)
            ? UserString("DESC_EMPIRE_AFFILIATION")
            : UserString("DESC_EMPIRE_AFFILIATION_NOT"))
                   % UserString(boost::lexical_cast<std::string>(m_affiliation))
                   % empire_str);
    }
}

std::string EmpireAffiliation::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs);
    if (m_affiliation == EmpireAffiliationType::AFFIL_SELF) {
        retval += "OwnedBy";
        if (m_empire_id)
            retval += " empire = " + m_empire_id->Dump(ntabs);

    } else if (m_affiliation == EmpireAffiliationType::AFFIL_ANY) {
        retval += "OwnedBy affiliation = AnyEmpire";

    } else if (m_affiliation == EmpireAffiliationType::AFFIL_NONE) {
        retval += "Unowned";

    } else if (m_affiliation == EmpireAffiliationType::AFFIL_ENEMY) {
        retval += "OwnedBy affiliation = EnemyOf";
        if (m_empire_id)
            retval += " empire = " + m_empire_id->Dump(ntabs);

    } else if (m_affiliation == EmpireAffiliationType::AFFIL_PEACE) {
        retval += "OwnedBy affiliation = PeaceWith";
        if (m_empire_id)
            retval += " empire = " + m_empire_id->Dump(ntabs);

    } else if (m_affiliation == EmpireAffiliationType::AFFIL_ALLY) {
        retval += "OwnedBy affiliation = AllyOf";
        if (m_empire_id)
            retval += " empire = " + m_empire_id->Dump(ntabs);

    } else if (m_affiliation == EmpireAffiliationType::AFFIL_CAN_SEE) {
        retval += "OwnedBy affiliation = CanSee";

    } else if (m_affiliation == EmpireAffiliationType::AFFIL_HUMAN) {
        retval += "OwnedBy affiliation = Human";

    } else {
        retval += "OwnedBy ??";
    }

    retval += "\n";
    return retval;
}

bool EmpireAffiliation::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "EmpireAffiliation::Match passed no candidate object";
        return false;
    }
    if (EmpireAffiliationSimpleMatch::AffiliationTypeUsesEmpireID(m_affiliation) && m_empire_id) {
        int empire_id = m_empire_id->Eval(local_context);
        return EmpireAffiliationSimpleMatch(empire_id, m_affiliation, local_context)(candidate);
    } else {
        return EmpireAffiliationSimpleMatch(m_affiliation, local_context)(candidate);
    }
}

void EmpireAffiliation::SetTopLevelContent(const std::string& content_name) {
    if (m_empire_id)
        m_empire_id->SetTopLevelContent(content_name);
}

unsigned int EmpireAffiliation::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::EmpireAffiliation");
    CheckSums::CheckSumCombine(retval, m_empire_id);
    CheckSums::CheckSumCombine(retval, m_affiliation);

    TraceLogger() << "GetCheckSum(EmpireAffiliation): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> EmpireAffiliation::Clone() const {
    return std::make_unique<EmpireAffiliation>(ValueRef::CloneUnique(m_empire_id),
                                               m_affiliation);
}

///////////////////////////////////////////////////////////
// Source                                                //
///////////////////////////////////////////////////////////
Source::Source() :
    Condition()
{
    m_root_candidate_invariant = true;
    m_target_invariant = true;
    m_source_invariant = false;
}

bool Source::operator==(const Condition& rhs) const
{ return Condition::operator==(rhs); }

std::string Source::Description(bool negated/* = false*/) const {
    return (!negated)
        ? UserString("DESC_SOURCE")
        : UserString("DESC_SOURCE_NOT");
}

std::string Source::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "Source\n"; }

bool Source::Match(const ScriptingContext& local_context) const {
    if (!local_context.source)
        return false;
    return local_context.source == local_context.condition_local_candidate;
}

void Source::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                               ObjectSet& condition_non_targets) const
{
    if (parent_context.source)
        condition_non_targets.push_back(parent_context.source);
}

unsigned int Source::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Source");

    TraceLogger() << "GetCheckSum(Source): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Source::Clone() const
{ return std::make_unique<Source>(); }

///////////////////////////////////////////////////////////
// RootCandidate                                         //
///////////////////////////////////////////////////////////
RootCandidate::RootCandidate() :
    Condition()
{
    m_root_candidate_invariant = false;
    m_target_invariant = true;
    m_source_invariant = true;
}

bool RootCandidate::operator==(const Condition& rhs) const
{ return Condition::operator==(rhs); }

std::string RootCandidate::Description(bool negated/* = false*/) const {
    return (!negated)
        ? UserString("DESC_ROOT_CANDIDATE")
        : UserString("DESC_ROOT_CANDIDATE_NOT");
}

std::string RootCandidate::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "RootCandidate\n"; }

bool RootCandidate::Match(const ScriptingContext& local_context) const {
    if (!local_context.condition_root_candidate)
        return false;
    return local_context.condition_root_candidate == local_context.condition_local_candidate;
}

void RootCandidate::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                      ObjectSet& condition_non_targets) const
{
    if (parent_context.condition_root_candidate)
        condition_non_targets.push_back(parent_context.condition_root_candidate);
}

unsigned int RootCandidate::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::RootCandidate");

    TraceLogger() << "GetCheckSum(RootCandidate): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> RootCandidate::Clone() const
{ return std::make_unique<RootCandidate>(); }

///////////////////////////////////////////////////////////
// Target                                                //
///////////////////////////////////////////////////////////
Target::Target() :
    Condition()
{
    m_root_candidate_invariant = true;
    m_target_invariant = false;
    m_source_invariant = true;
}

bool Target::operator==(const Condition& rhs) const
{ return Condition::operator==(rhs); }

std::string Target::Description(bool negated/* = false*/) const {
    return (!negated)
        ? UserString("DESC_TARGET")
        : UserString("DESC_TARGET_NOT");
}

std::string Target::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "Target\n"; }

bool Target::Match(const ScriptingContext& local_context) const {
    if (!local_context.effect_target)
        return false;
    return local_context.effect_target == local_context.condition_local_candidate;
}

void Target::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                               ObjectSet& condition_non_targets) const
{
    if (parent_context.effect_target)
        condition_non_targets.push_back(parent_context.effect_target);
}

unsigned int Target::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Target");

    TraceLogger() << "GetCheckSum(Target): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Target::Clone() const
{ return std::unique_ptr<Target>(); }

///////////////////////////////////////////////////////////
// Homeworld                                             //
///////////////////////////////////////////////////////////
Homeworld::Homeworld() :
    Homeworld(std::vector<std::unique_ptr<ValueRef::ValueRef<std::string>>>{})
{}

Homeworld::Homeworld(std::vector<std::unique_ptr<ValueRef::ValueRef<std::string>>>&& names) :
    Condition(),
    m_names(std::move(names))
{
    m_root_candidate_invariant = boost::algorithm::all_of(m_names, [](auto& e){ return e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(m_names, [](auto& e){ return e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(m_names, [](auto& e){ return e->SourceInvariant(); });
}

bool Homeworld::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const Homeworld& rhs_ = static_cast<const Homeworld&>(rhs);

    if (m_names.size() != rhs_.m_names.size())
        return false;
    for (unsigned int i = 0; i < m_names.size(); ++i) {
        CHECK_COND_VREF_MEMBER(m_names.at(i))
    }

    return true;
}

namespace {
    struct HomeworldSimpleMatch {
        HomeworldSimpleMatch(std::vector<std::string> names, const ObjectMap& objects,
                             const SpeciesManager& species) :
            m_names(std::move(names)),
            m_objects(objects),
            m_species_homeworlds(species.GetSpeciesHomeworldsMap())
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            int planet_id = INVALID_OBJECT_ID;
            if (candidate->ObjectType() == UniverseObjectType::OBJ_PLANET) {
                planet_id = candidate->ID();
            } else if (candidate->ObjectType() == UniverseObjectType::OBJ_BUILDING) {
                auto* building = static_cast<const ::Building*>(candidate.get());
                planet_id = building->PlanetID();
            }
            if (planet_id == INVALID_OBJECT_ID)
                return false;

            if (m_names.empty()) {
                // match homeworlds for any species
                for ([[maybe_unused]] auto& [ignored_name, ids] : m_species_homeworlds) {
                    (void)ignored_name; // quieting unused variable warning
                    if (ids.count(planet_id))
                        return true;
                }

            } else {
                // match any of the species specified
                for (const std::string& name : m_names) {
                    auto it = m_species_homeworlds.find(name);
                    if (it == m_species_homeworlds.end())
                        continue;
                    const auto& planet_ids = it->second;
                    auto planet_count = planet_ids.count(planet_id);
                    if (planet_count > 0)
                        return true;
                }
            }

            return false;
        }

        const std::vector<std::string> m_names;
        const ObjectMap& m_objects;
        const std::map<std::string, std::set<int>> m_species_homeworlds;
    };
}

void Homeworld::Eval(const ScriptingContext& parent_context,
                     ObjectSet& matches, ObjectSet& non_matches,
                     SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = parent_context.condition_root_candidate || RootCandidateInvariant();
    if (simple_eval_safe) {
        // check each valueref for invariance to local candidate
        for (auto& name : m_names) {
            if (!name->LocalCandidateInvariant()) {
                simple_eval_safe = false;
                break;
            }
        }
    }
    if (simple_eval_safe) {
        // evaluate names once, and use to check all candidate objects
        std::vector<std::string> names;
        names.reserve(m_names.size());
        // get all names from valuerefs
        for (auto& name : m_names)
            names.push_back(name->Eval(parent_context));
        HomeworldSimpleMatch hsm{std::move(names), parent_context.ContextObjects(), parent_context.species};
        EvalImpl(matches, non_matches, search_domain, hsm);
    } else {
        // re-evaluate allowed names for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string Homeworld::Description(bool negated/* = false*/) const {
    std::string values_str;
    for (unsigned int i = 0; i < m_names.size(); ++i) {
        values_str += m_names[i]->ConstantExpr() ?
                        UserString(m_names[i]->Eval()) :
                        m_names[i]->Description();
        if (2 <= m_names.size() && i < m_names.size() - 2) {
            values_str += ", ";
        } else if (i == m_names.size() - 2) {
            values_str += m_names.size() < 3 ? " " : ", ";
            values_str += UserString("OR");
            values_str += " ";
        }
    }
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_HOMEWORLD")
        : UserString("DESC_HOMEWORLD_NOT"))
        % values_str);
}

std::string Homeworld::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "HomeWorld";
    if (m_names.size() == 1) {
        retval += " name = " + m_names[0]->Dump(ntabs);
    } else if (!m_names.empty()) {
        retval += " name = [ ";
        for (auto& name : m_names) {
            retval += name->Dump(ntabs) + " ";
        }
        retval += "]";
    }
    return retval;
}

bool Homeworld::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "Homeworld::Match passed no candidate object";
        return false;
    }

    // is it a planet or a building on a planet?
    int planet_id = INVALID_OBJECT_ID;
    if (candidate->ObjectType() == UniverseObjectType::OBJ_PLANET) {
        planet_id = candidate->ID();
    } else if (candidate->ObjectType() == UniverseObjectType::OBJ_BUILDING) {
        auto* building = static_cast<const ::Building*>(candidate.get());
        planet_id = building->PlanetID();
    }
    if (planet_id == INVALID_OBJECT_ID)
        return false;

    if (m_names.empty()) {
        // match homeworlds for any species
        for (const auto& entry : local_context.species.GetSpeciesHomeworldsMap()) {
            if (entry.second.count(planet_id))
                return true;
        }

    } else {
        // match any of the species specified
        const auto homeworlds = local_context.species.GetSpeciesHomeworldsMap();
        for (const auto& name_ref : m_names) {
            const auto species_name = name_ref->Eval(local_context);
            if (homeworlds.count(species_name) && homeworlds.at(species_name).count(planet_id))
                return true;
        }
    }

    return false;
}

void Homeworld::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                  ObjectSet& condition_non_targets) const
{ AddPlanetSet(parent_context.ContextObjects(), condition_non_targets); }

void Homeworld::SetTopLevelContent(const std::string& content_name) {
    for (auto& name : m_names) {
        if (name)
            name->SetTopLevelContent(content_name);
    }
}

unsigned int Homeworld::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Homeworld");
    CheckSums::CheckSumCombine(retval, m_names);

    TraceLogger() << "GetCheckSum(Homeworld): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Homeworld::Clone() const
{ return std::make_unique<Homeworld>(ValueRef::CloneUnique(m_names)); }

///////////////////////////////////////////////////////////
// Capital                                               //
///////////////////////////////////////////////////////////
Capital::Capital() :
    Condition()
{
    m_root_candidate_invariant = true;
    m_target_invariant = true;
    m_source_invariant = true;
}

bool Capital::operator==(const Condition& rhs) const
{ return Condition::operator==(rhs); }

namespace {
    struct CapitalSimpleMatch {
        CapitalSimpleMatch(const EmpireManager::const_container_type& empires) :
            m_capital_ids{[empires]() -> std::vector<int> {
                // collect capitals of all empires
                std::vector<int> retval;
                retval.reserve(empires.size());
                for (auto& [empire_id, empire] : empires) {
                    (void)empire_id;
                    auto id = empire->CapitalID();
                    if (id != INVALID_OBJECT_ID)
                        retval.push_back(id);
                }
                return retval;
            }()}
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;
            return std::find(m_capital_ids.begin(), m_capital_ids.end(), candidate->ID()) != m_capital_ids.end();
        }

        const std::vector<int> m_capital_ids;
    };
}

void Capital::Eval(const ScriptingContext& parent_context,
                   ObjectSet& matches, ObjectSet& non_matches,
                   SearchDomain search_domain) const
{ EvalImpl(matches, non_matches, search_domain, CapitalSimpleMatch{parent_context.Empires()}); }

std::string Capital::Description(bool negated/* = false*/) const {
    return (!negated)
        ? UserString("DESC_CAPITAL")
        : UserString("DESC_CAPITAL_NOT");
}

std::string Capital::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "Capital\n"; }

bool Capital::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "Capital::Match passed no candidate object";
        return false;
    }
    int candidate_id = candidate->ID();

    // check if any empire's capital's ID is that candidate object's id.
    // if it is, the candidate object is a capital.
    for ([[maybe_unused]] auto& [ignored_id, empire] : local_context.Empires()) {
        (void)ignored_id;
        if (empire->CapitalID() == candidate_id)
            return true;
    }
    return false;
}

void Capital::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                ObjectSet& condition_non_targets) const
{
    if constexpr(false) {
        auto capital_ids{[empires{parent_context.Empires()}]() -> std::set<int> {
            // collect capitals of all empires
            std::set<int> retval;
            for (auto& [empire_id, empire] : empires) {
                (void)empire_id;
                auto id = empire->CapitalID();
                if (id != INVALID_OBJECT_ID)
                    retval.insert(id);
            }
            return retval;
        }()};

        condition_non_targets.reserve(condition_non_targets.size() + capital_ids.size());
        for (auto& obj : parent_context.ContextObjects().find(capital_ids))
            condition_non_targets.push_back(std::move(obj));
    } else {
        AddPlanetSet(parent_context.ContextObjects(), condition_non_targets);
    }
}

unsigned int Capital::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Capital");

    TraceLogger() << "GetCheckSum(Capital): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Capital::Clone() const
{ return std::make_unique<Capital>(); }

///////////////////////////////////////////////////////////
// Monster                                               //
///////////////////////////////////////////////////////////
Monster::Monster() :
    Condition()
{
    m_root_candidate_invariant = true;
    m_target_invariant = true;
    m_source_invariant = true;
}

bool Monster::operator==(const Condition& rhs) const
{ return Condition::operator==(rhs); }

std::string Monster::Description(bool negated/* = false*/) const {
    return (!negated)
        ? UserString("DESC_MONSTER")
        : UserString("DESC_MONSTER_NOT");
}

std::string Monster::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "Monster\n"; }

bool Monster::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "Monster::Match passed no candidate object";
        return false;
    }

    if (candidate->ObjectType() == UniverseObjectType::OBJ_SHIP) {
        auto* ship = static_cast<const Ship*>(candidate.get());
        if (ship->IsMonster(local_context.ContextUniverse()))
            return true;
    }

    return false;
}

void Monster::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                ObjectSet& condition_non_targets) const
{ AddShipSet(parent_context.ContextObjects(), condition_non_targets); }

unsigned int Monster::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Monster");

    TraceLogger() << "GetCheckSum(Monster): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Monster::Clone() const
{ return std::make_unique<Monster>(); }

///////////////////////////////////////////////////////////
// Armed                                                 //
///////////////////////////////////////////////////////////
Armed::Armed() :
    Condition()
{
    m_root_candidate_invariant = true;
    m_target_invariant = true;
    m_source_invariant = true;
}

bool Armed::operator==(const Condition& rhs) const
{ return Condition::operator==(rhs); }

std::string Armed::Description(bool negated/* = false*/) const {
    return (!negated)
        ? UserString("DESC_ARMED")
        : UserString("DESC_ARMED_NOT");
}

std::string Armed::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "Armed\n"; }

bool Armed::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "Armed::Match passed no candidate object";
        return false;
    }

    if (candidate->ObjectType() == UniverseObjectType::OBJ_SHIP) {
        auto* ship = static_cast<const Ship*>(candidate.get());
        if (ship->IsArmed(local_context))
            return true;
    }

    return false;
}

unsigned int Armed::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Armed");

    TraceLogger() << "GetCheckSum(Armed): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Armed::Clone() const
{ return std::make_unique<Armed>(); }

///////////////////////////////////////////////////////////
// Type                                                  //
///////////////////////////////////////////////////////////
Type::Type(std::unique_ptr<ValueRef::ValueRef<UniverseObjectType>>&& type) :
    Condition(),
    m_type(std::move(type))
{
    m_root_candidate_invariant = !m_type || m_type->RootCandidateInvariant();
    m_target_invariant = !m_type || m_type->TargetInvariant();
    m_source_invariant = !m_type || m_type->SourceInvariant();
}

Type::Type(UniverseObjectType type) :
    Type(std::make_unique<ValueRef::Constant<UniverseObjectType>>(type))
{}

bool Type::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const Type& rhs_ = static_cast<const Type&>(rhs);

    CHECK_COND_VREF_MEMBER(m_type)

    return true;
}

namespace {
    struct TypeSimpleMatch {
        TypeSimpleMatch(UniverseObjectType type) :
            m_type(type)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            switch (m_type) {
            case UniverseObjectType::OBJ_BUILDING:
            case UniverseObjectType::OBJ_SHIP:
            case UniverseObjectType::OBJ_FLEET:
            case UniverseObjectType::OBJ_PLANET:
            case UniverseObjectType::OBJ_SYSTEM:
            case UniverseObjectType::OBJ_FIELD:
            case UniverseObjectType::OBJ_FIGHTER:
                return candidate->ObjectType() == m_type;
                break;
            case UniverseObjectType::OBJ_POP_CENTER:
                return (bool)dynamic_cast<const PopCenter*>(candidate.get());
                break;
            case UniverseObjectType::OBJ_PROD_CENTER:
                return (bool)dynamic_cast<const ResourceCenter*>(candidate.get());
                break;
            default:
                break;
            }
            return false;
        }

        UniverseObjectType m_type;
    };
}

void Type::Eval(const ScriptingContext& parent_context,
                ObjectSet& matches, ObjectSet& non_matches,
                SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = m_type->ConstantExpr() ||
                            (m_type->LocalCandidateInvariant() &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        UniverseObjectType type = m_type->Eval(parent_context);
        EvalImpl(matches, non_matches, search_domain, TypeSimpleMatch(type));
    } else {
        // re-evaluate allowed turn range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string Type::Description(bool negated/* = false*/) const {
    std::string value_str = m_type->ConstantExpr() ?
                                UserString(boost::lexical_cast<std::string>(m_type->Eval())) :
                                m_type->Description();
    return str(FlexibleFormat((!negated)
           ? UserString("DESC_TYPE")
           : UserString("DESC_TYPE_NOT"))
           % value_str);
}

std::string Type::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs);
    if (dynamic_cast<ValueRef::Constant<UniverseObjectType>*>(m_type.get())) {
        switch (m_type->Eval()) {
        case UniverseObjectType::OBJ_BUILDING:    retval += "Building\n"; break;
        case UniverseObjectType::OBJ_SHIP:        retval += "Ship\n"; break;
        case UniverseObjectType::OBJ_FLEET:       retval += "Fleet\n"; break;
        case UniverseObjectType::OBJ_PLANET:      retval += "Planet\n"; break;
        case UniverseObjectType::OBJ_POP_CENTER:  retval += "PopulationCenter\n"; break;
        case UniverseObjectType::OBJ_PROD_CENTER: retval += "ProductionCenter\n"; break;
        case UniverseObjectType::OBJ_SYSTEM:      retval += "System\n"; break;
        case UniverseObjectType::OBJ_FIELD:       retval += "Field\n"; break;
        case UniverseObjectType::OBJ_FIGHTER:     retval += "Fighter\n"; break;
        default: retval += "?\n"; break;
        }
    } else {
        retval += "ObjectType type = " + m_type->Dump(ntabs) + "\n";
    }
    return retval;
}

bool Type::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "Type::Match passed no candidate object";
        return false;
    }

    return TypeSimpleMatch(m_type->Eval(local_context))(candidate);
}

void Type::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                             ObjectSet& condition_non_targets) const
{
    // Ships, Fleets and default checks for current objects only
    bool found_type = false;
    if (m_type) {
        found_type = true;
        //std::vector<std::shared_ptr<const T>> this_base;
        switch (m_type->Eval()) {
            case UniverseObjectType::OBJ_BUILDING:
                AddBuildingSet(parent_context.ContextObjects(), condition_non_targets);
                break;
            case UniverseObjectType::OBJ_FIELD:
                AddFieldSet(parent_context.ContextObjects(), condition_non_targets);
                break;
            case UniverseObjectType::OBJ_FLEET:
                AddFleetSet(parent_context.ContextObjects(), condition_non_targets);
                break;
            case UniverseObjectType::OBJ_PLANET:
                AddPlanetSet(parent_context.ContextObjects(), condition_non_targets);
                break;
            case UniverseObjectType::OBJ_POP_CENTER:
                AddPopCenterSet(parent_context.ContextObjects(), condition_non_targets);
                break;
            case UniverseObjectType::OBJ_PROD_CENTER:
                AddResCenterSet(parent_context.ContextObjects(), condition_non_targets);
                break;
            case UniverseObjectType::OBJ_SHIP:
                AddShipSet(parent_context.ContextObjects(), condition_non_targets);
                break;
            case UniverseObjectType::OBJ_SYSTEM:
                AddSystemSet(parent_context.ContextObjects(), condition_non_targets);
                break;
            case UniverseObjectType::OBJ_FIGHTER:   // shouldn't exist outside of combat as a separate object
            default:
                found_type = false;
                break;
        }
    }
    if (found_type) {
        //if (int(condition_non_targets.size()) < parent_context.ContextObjects().size()) {
        //    DebugLogger() << "Type::GetBaseNonMatches will provide " << condition_non_targets.size()
        //                  << " objects of type " << GetType()->Eval() << " rather than "
        //                  << parent_context.ContextObjects().size() << " total objects";
        //}
    } else {
        Condition::GetDefaultInitialCandidateObjects(parent_context, condition_non_targets);
    }
}

void Type::SetTopLevelContent(const std::string& content_name) {
    if (m_type)
        m_type->SetTopLevelContent(content_name);
}

unsigned int Type::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Type");
    CheckSums::CheckSumCombine(retval, m_type);

    TraceLogger() << "GetCheckSum(Type): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Type::Clone() const
{ return std::make_unique<Type>(ValueRef::CloneUnique(m_type)); }

///////////////////////////////////////////////////////////
// Building                                              //
///////////////////////////////////////////////////////////
Building::Building(std::vector<std::unique_ptr<ValueRef::ValueRef<std::string>>>&& names) :
    Condition(),
    m_names(std::move(names))
{
    m_root_candidate_invariant = boost::algorithm::all_of(m_names, [](auto& e){ return e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(m_names, [](auto& e){ return e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(m_names, [](auto& e){ return e->SourceInvariant(); });
}

bool Building::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const Building& rhs_ = static_cast<const Building&>(rhs);

    if (m_names.size() != rhs_.m_names.size())
        return false;
    for (unsigned int i = 0; i < m_names.size(); ++i) {
        CHECK_COND_VREF_MEMBER(m_names.at(i))
    }

    return true;
}

namespace {
    struct BuildingSimpleMatch {
        BuildingSimpleMatch(const std::vector<std::string>& names) :
            m_names(names)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            // is it a building?
            if (candidate->ObjectType() != UniverseObjectType::OBJ_BUILDING)
                return false;
            auto* building = static_cast<const ::Building*>(candidate.get());

            // if no name supplied, match any building
            if (m_names.empty())
                return true;

            // is it one of the specified building types?
            return std::count(m_names.begin(), m_names.end(), building->BuildingTypeName());
        }

        const std::vector<std::string>& m_names;
    };
}

void Building::Eval(const ScriptingContext& parent_context,
                    ObjectSet& matches, ObjectSet& non_matches,
                    SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = parent_context.condition_root_candidate || RootCandidateInvariant();
    if (simple_eval_safe) {
        // check each valueref for invariance to local candidate
        for (auto& name : m_names) {
            if (!name->LocalCandidateInvariant()) {
                simple_eval_safe = false;
                break;
            }
        }
    }
    if (simple_eval_safe) {
        // evaluate names once, and use to check all candidate objects
        std::vector<std::string> names;
        names.reserve(m_names.size());
        // get all names from valuerefs
        for (auto& name : m_names)
            names.push_back(name->Eval(parent_context));
        EvalImpl(matches, non_matches, search_domain, BuildingSimpleMatch(names));
    } else {
        // re-evaluate allowed building types range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string Building::Description(bool negated/* = false*/) const {
    std::string values_str;
    for (unsigned int i = 0; i < m_names.size(); ++i) {
        values_str += m_names[i]->ConstantExpr() ?
                        UserString(m_names[i]->Eval()) :
                        m_names[i]->Description();
        if (2 <= m_names.size() && i < m_names.size() - 2) {
            values_str += ", ";
        } else if (i == m_names.size() - 2) {
            values_str += m_names.size() < 3 ? " " : ", ";
            values_str += UserString("OR");
            values_str += " ";
        }
    }
    return str(FlexibleFormat((!negated)
           ? UserString("DESC_BUILDING")
           : UserString("DESC_BUILDING_NOT"))
           % values_str);
}

std::string Building::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "Building name = ";
    if (m_names.size() == 1) {
        retval += m_names[0]->Dump(ntabs) + "\n";
    } else {
        retval += "[ ";
        for (auto& name : m_names) {
            retval += name->Dump(ntabs) + " ";
        }
        retval += "]\n";
    }
    return retval;
}

void Building::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                 ObjectSet& condition_non_targets) const
{ AddBuildingSet(parent_context.ContextObjects(), condition_non_targets); }

bool Building::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "Building::Match passed no candidate object";
        return false;
    }

    // is it a building?
    if (candidate->ObjectType() != UniverseObjectType::OBJ_BUILDING)
        return false;
    auto* building = static_cast<const ::Building*>(candidate.get());

    // match any building type?
    if (m_names.empty())
        return true;

    // match one of the specified building types
    const auto& btn{building->BuildingTypeName()};
    for (auto& name : m_names) {
        if (name->Eval(local_context) == btn)
            return true;
    }

    return false;
}

void Building::SetTopLevelContent(const std::string& content_name) {
    for (auto& name : m_names) {
        if (name)
            name->SetTopLevelContent(content_name);
    }
}

unsigned int Building::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Building");
    CheckSums::CheckSumCombine(retval, m_names);

    TraceLogger() << "GetCheckSum(Building): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Building::Clone() const
{ return std::make_unique<Building>(ValueRef::CloneUnique(m_names)); }

///////////////////////////////////////////////////////////
// Field                                                 //
///////////////////////////////////////////////////////////
Field::Field(std::vector<std::unique_ptr<ValueRef::ValueRef<std::string>>>&& names) :
    Condition(),
    m_names(std::move(names))
{
    m_root_candidate_invariant = boost::algorithm::all_of(m_names, [](auto& e){ return e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(m_names, [](auto& e){ return e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(m_names, [](auto& e){ return e->SourceInvariant(); });
}

bool Field::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const Field& rhs_ = static_cast<const Field&>(rhs);

    if (m_names.size() != rhs_.m_names.size())
        return false;
    for (unsigned int i = 0; i < m_names.size(); ++i) {
        CHECK_COND_VREF_MEMBER(m_names.at(i))
    }

    return true;
}

namespace {
    struct FieldSimpleMatch {
        FieldSimpleMatch(const std::vector<std::string>& names) :
            m_names(names)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            // is it a field?
            if (candidate->ObjectType() != UniverseObjectType::OBJ_FIELD)
                return false;
            auto* field = static_cast<const ::Field*>(candidate.get());

            // if no name supplied, match any field
            if (m_names.empty())
                return true;

            // is it one of the specified field types?
            return std::count(m_names.begin(), m_names.end(), field->FieldTypeName());
        }

        const std::vector<std::string>& m_names;
    };
}

void Field::Eval(const ScriptingContext& parent_context,
                 ObjectSet& matches, ObjectSet& non_matches,
                 SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = parent_context.condition_root_candidate || RootCandidateInvariant();
    if (simple_eval_safe) {
        // check each valueref for invariance to local candidate
        for (auto& name : m_names) {
            if (!name->LocalCandidateInvariant()) {
                simple_eval_safe = false;
                break;
            }
        }
    }
    if (simple_eval_safe) {
        // evaluate names once, and use to check all candidate objects
        std::vector<std::string> names;
        names.reserve(m_names.size());
        // get all names from valuerefs
        for (auto& name : m_names)
            names.push_back(name->Eval(parent_context));
        EvalImpl(matches, non_matches, search_domain, FieldSimpleMatch(names));
    } else {
        // re-evaluate allowed field types range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string Field::Description(bool negated/* = false*/) const {
    std::string values_str;
    for (unsigned int i = 0; i < m_names.size(); ++i) {
        values_str += m_names[i]->ConstantExpr() ?
                        UserString(m_names[i]->Eval()) :
                        m_names[i]->Description();
        if (2 <= m_names.size() && i < m_names.size() - 2) {
            values_str += ", ";
        } else if (i == m_names.size() - 2) {
            values_str += m_names.size() < 3 ? " " : ", ";
            values_str += UserString("OR");
            values_str += " ";
        }
    }
    return str(FlexibleFormat((!negated)
           ? UserString("DESC_FIELD")
           : UserString("DESC_FIELD_NOT"))
           % values_str);
}

std::string Field::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "Field name = ";
    if (m_names.size() == 1) {
        retval += m_names[0]->Dump(ntabs) + "\n";
    } else {
        retval += "[ ";
        for (auto& name : m_names) {
            retval += name->Dump(ntabs) + " ";
        }
        retval += "]\n";
    }
    return retval;
}

void Field::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                              ObjectSet& condition_non_targets) const
{ AddFieldSet(parent_context.ContextObjects(), condition_non_targets); }

bool Field::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "Field::Match passed no candidate object";
        return false;
    }

    // is it a field?
    if (candidate->ObjectType() != UniverseObjectType::OBJ_FIELD)
        return false;
    auto* field = static_cast<const ::Field*>(candidate.get());

    // match any field type?
    if (m_names.empty())
        return true;

    // match one of the specified field names
    for (auto& name : m_names) {
        if (name->Eval(local_context) == field->FieldTypeName())
            return true;
    }

    return false;
}

void Field::SetTopLevelContent(const std::string& content_name) {
    for (auto& name : m_names) {
        if (name)
            name->SetTopLevelContent(content_name);
    }
}

unsigned int Field::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Field");
    CheckSums::CheckSumCombine(retval, m_names);

    TraceLogger() << "GetCheckSum(Field): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Field::Clone() const
{ return std::make_unique<Field>(ValueRef::CloneUnique(m_names)); }

///////////////////////////////////////////////////////////
// HasSpecial                                            //
///////////////////////////////////////////////////////////
HasSpecial::HasSpecial() :
    HasSpecial(nullptr,
               std::unique_ptr<ValueRef::ValueRef<int>>{},
               std::unique_ptr<ValueRef::ValueRef<int>>{})
{}

HasSpecial::HasSpecial(std::string name) :
    HasSpecial(std::make_unique<ValueRef::Constant<std::string>>(std::move(name)),
               std::unique_ptr<ValueRef::ValueRef<int>>{},
               std::unique_ptr<ValueRef::ValueRef<int>>{})
{}

HasSpecial::HasSpecial(std::unique_ptr<ValueRef::ValueRef<std::string>>&& name) :
    HasSpecial(std::move(name),
               std::unique_ptr<ValueRef::ValueRef<int>>{},
               std::unique_ptr<ValueRef::ValueRef<int>>{})
{}

HasSpecial::HasSpecial(std::unique_ptr<ValueRef::ValueRef<std::string>>&& name,
                       std::unique_ptr<ValueRef::ValueRef<int>>&& since_turn_low,
                       std::unique_ptr<ValueRef::ValueRef<int>>&& since_turn_high) :
    Condition(),
    m_name(std::move(name)),
    m_since_turn_low(std::move(since_turn_low)),
    m_since_turn_high(std::move(since_turn_high))
{
    std::array<const ValueRef::ValueRefBase*, 3> operands =
        {{m_name.get(), m_since_turn_low.get(), m_since_turn_high.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

HasSpecial::HasSpecial(std::unique_ptr<ValueRef::ValueRef<std::string>>&& name,
                       std::unique_ptr<ValueRef::ValueRef<double>>&& capacity_low,
                       std::unique_ptr<ValueRef::ValueRef<double>>&& capacity_high) :
    Condition(),
    m_name(std::move(name)),
    m_capacity_low(std::move(capacity_low)),
    m_capacity_high(std::move(capacity_high))
{
    std::array<const ValueRef::ValueRefBase*, 3> operands =
        {{m_name.get(), m_capacity_low.get(), m_capacity_high.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

HasSpecial::HasSpecial(const HasSpecial& rhs) :
    Condition(rhs),
    m_name(ValueRef::CloneUnique(rhs.m_name)),
    m_capacity_low(ValueRef::CloneUnique(rhs.m_capacity_low)),
    m_capacity_high(ValueRef::CloneUnique(rhs.m_capacity_high)),
    m_since_turn_low(ValueRef::CloneUnique(rhs.m_since_turn_low)),
    m_since_turn_high(ValueRef::CloneUnique(rhs.m_since_turn_high))
{}

bool HasSpecial::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const HasSpecial& rhs_ = static_cast<const HasSpecial&>(rhs);

    CHECK_COND_VREF_MEMBER(m_name)
    CHECK_COND_VREF_MEMBER(m_capacity_low)
    CHECK_COND_VREF_MEMBER(m_capacity_high)
    CHECK_COND_VREF_MEMBER(m_since_turn_low)
    CHECK_COND_VREF_MEMBER(m_since_turn_high)

    return true;
}

namespace {
    struct HasSpecialSimpleMatch {
        HasSpecialSimpleMatch(const std::string& name, float low_cap, float high_cap,
                              int low_turn, int high_turn) :
            m_name(name),
            m_low_cap(low_cap),
            m_high_cap(high_cap),
            m_low_turn(low_turn),
            m_high_turn(high_turn)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            if (m_name.empty())
                return !candidate->Specials().empty();

            auto it = candidate->Specials().find(m_name);
            if (it == candidate->Specials().end())
                return false;

            int special_since_turn = it->second.first;
            float special_capacity = it->second.second;
            return m_low_turn <= special_since_turn
                && special_since_turn <= m_high_turn
                && m_low_cap <= special_capacity
                && special_capacity <= m_high_cap;
        }

        const std::string&  m_name;
        float               m_low_cap;
        float               m_high_cap;
        int                 m_low_turn;
        int                 m_high_turn;
    };
}

void HasSpecial::Eval(const ScriptingContext& parent_context,
                      ObjectSet& matches, ObjectSet& non_matches,
                      SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = ((!m_name || m_name->LocalCandidateInvariant()) &&
                             (!m_capacity_low || m_capacity_low->LocalCandidateInvariant()) &&
                             (!m_capacity_high || m_capacity_high->LocalCandidateInvariant()) &&
                             (!m_since_turn_low || m_since_turn_low->LocalCandidateInvariant()) &&
                             (!m_since_turn_high || m_since_turn_high->LocalCandidateInvariant()) &&
                             (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // evaluate turn limits and capacities once, pass to simple match for all candidates
        std::string name = (m_name ? m_name->Eval(parent_context) : "");
        float low_cap = (m_capacity_low ? m_capacity_low->Eval(parent_context) : -FLT_MAX);
        float high_cap = (m_capacity_high ? m_capacity_high->Eval(parent_context) : FLT_MAX);
        int low_turn = (m_since_turn_low ? m_since_turn_low->Eval(parent_context) : BEFORE_FIRST_TURN);
        int high_turn = (m_since_turn_high ? m_since_turn_high->Eval(parent_context) : IMPOSSIBLY_LARGE_TURN);
        EvalImpl(matches, non_matches, search_domain, HasSpecialSimpleMatch(name, low_cap, high_cap, low_turn, high_turn));
    } else {
        // re-evaluate allowed turn range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string HasSpecial::Description(bool negated/* = false*/) const {
    std::string name_str;
    if (m_name) {
        name_str = m_name->Description();
        if (m_name->ConstantExpr() && UserStringExists(name_str))
            name_str = UserString(name_str);
    }

    if (m_since_turn_low || m_since_turn_high) {
        // turn range has been specified; must indicate in description
        std::string low_str = std::to_string(BEFORE_FIRST_TURN);
        if (m_since_turn_low)
            low_str = m_since_turn_low->Description();

        std::string high_str = std::to_string(IMPOSSIBLY_LARGE_TURN);
        if (m_since_turn_high)
            high_str = m_since_turn_high->Description();

        return str(FlexibleFormat((!negated)
            ? UserString("DESC_SPECIAL_TURN_RANGE")
            : UserString("DESC_SPECIAL_TURN_RANGE_NOT"))
                % name_str
                % low_str
                % high_str);
    }

    if (m_capacity_low || m_capacity_high) {
        // capacity range has been specified; must indicate in description
        std::string low_str = std::to_string(-FLT_MAX);
        if (m_capacity_low)
            low_str = m_capacity_low->Description();

        std::string high_str = std::to_string(FLT_MAX);
        if (m_capacity_high)
            high_str = m_capacity_high->Description();

        return str(FlexibleFormat((!negated)
            ? UserString("DESC_SPECIAL_CAPACITY_RANGE")
            : UserString("DESC_SPECIAL_CAPACITY_RANGE_NOT"))
                % name_str
                % low_str
                % high_str);
    }

    return str(FlexibleFormat((!negated)
        ? UserString("DESC_SPECIAL")
        : UserString("DESC_SPECIAL_NOT"))
                % name_str);
}

std::string HasSpecial::Dump(unsigned short ntabs) const {
    std::string name_str = (m_name ? m_name->Dump(ntabs) : "");

    if (m_since_turn_low || m_since_turn_high) {
        std::string low_dump = (m_since_turn_low ? m_since_turn_low->Dump(ntabs) : std::to_string(BEFORE_FIRST_TURN));
        std::string high_dump = (m_since_turn_high ? m_since_turn_high->Dump(ntabs) : std::to_string(IMPOSSIBLY_LARGE_TURN));
        return DumpIndent(ntabs) + "HasSpecialSinceTurn name = \"" + name_str + "\" low = " + low_dump + " high = " + high_dump;
    }

    if (m_capacity_low || m_capacity_high) {
        std::string low_dump = (m_capacity_low ? m_capacity_low->Dump(ntabs) : std::to_string(-FLT_MAX));
        std::string high_dump = (m_capacity_high ? m_capacity_high->Dump(ntabs) : std::to_string(FLT_MAX));
        return DumpIndent(ntabs) + "HasSpecialCapacity name = \"" + name_str + "\" low = " + low_dump + " high = " + high_dump;
    }

    return DumpIndent(ntabs) + "HasSpecial name = \"" + name_str + "\"\n";
}

bool HasSpecial::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "HasSpecial::Match passed no candidate object";
        return false;
    }
    std::string name = (m_name ? m_name->Eval(local_context) : "");
    float low_cap = (m_capacity_low ? m_capacity_low->Eval(local_context) : -FLT_MAX);
    float high_cap = (m_capacity_high ? m_capacity_high->Eval(local_context) : FLT_MAX);
    int low_turn = (m_since_turn_low ? m_since_turn_low->Eval(local_context) : BEFORE_FIRST_TURN);
    int high_turn = (m_since_turn_high ? m_since_turn_high->Eval(local_context) : IMPOSSIBLY_LARGE_TURN);

    return HasSpecialSimpleMatch(name, low_cap, high_cap, low_turn, high_turn)(candidate);
}

void HasSpecial::SetTopLevelContent(const std::string& content_name) {
    if (m_name)
        m_name->SetTopLevelContent(content_name);
    if (m_capacity_low)
        m_capacity_low->SetTopLevelContent(content_name);
    if (m_capacity_high)
        m_capacity_high->SetTopLevelContent(content_name);
    if (m_since_turn_low)
        m_since_turn_low->SetTopLevelContent(content_name);
    if (m_since_turn_high)
        m_since_turn_high->SetTopLevelContent(content_name);
}

unsigned int HasSpecial::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::HasSpecial");
    CheckSums::CheckSumCombine(retval, m_name);
    CheckSums::CheckSumCombine(retval, m_capacity_low);
    CheckSums::CheckSumCombine(retval, m_capacity_high);
    CheckSums::CheckSumCombine(retval, m_since_turn_low);
    CheckSums::CheckSumCombine(retval, m_since_turn_high);

    TraceLogger() << "GetCheckSum(HasSpecial): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> HasSpecial::Clone() const
{ return std::make_unique<HasSpecial>(*this); }

///////////////////////////////////////////////////////////
// HasTag                                                //
///////////////////////////////////////////////////////////
HasTag::HasTag() :
    HasTag(std::unique_ptr<ValueRef::ValueRef<std::string>>{})
{}

HasTag::HasTag(std::string name) :
    HasTag(std::make_unique<ValueRef::Constant<std::string>>(std::move(name)))
{}

HasTag::HasTag(std::unique_ptr<ValueRef::ValueRef<std::string>>&& name) :
    Condition(),
    m_name(std::move(name))
{
    m_root_candidate_invariant = !m_name || m_name->RootCandidateInvariant();
    m_target_invariant = !m_name || m_name->TargetInvariant();
    m_source_invariant = !m_name || m_name->SourceInvariant();
}

bool HasTag::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const HasTag& rhs_ = static_cast<const HasTag&>(rhs);

    CHECK_COND_VREF_MEMBER(m_name)

    return true;
}

namespace {
    struct HasTagSimpleMatch {
        HasTagSimpleMatch(const ScriptingContext& context) :
            m_any_tag_ok(true),
            m_name(EMPTY_STRING),
            m_context(context)
        {}

        HasTagSimpleMatch(const std::string& name, const ScriptingContext& context) :
            m_any_tag_ok(false),
            m_name(name),
            m_context(context)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            if (m_any_tag_ok && !candidate->Tags(m_context).empty())
                return true;

            return candidate->HasTag(m_name, m_context);
        }

        bool                    m_any_tag_ok;
        const std::string&      m_name;
        const ScriptingContext& m_context;
    };
}

void HasTag::Eval(const ScriptingContext& parent_context,
                  ObjectSet& matches, ObjectSet& non_matches,
                  SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = (!m_name || m_name->LocalCandidateInvariant()) &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant());
    if (simple_eval_safe) {
        // evaluate number limits once, use to match all candidates
        if (!m_name) {
            EvalImpl(matches, non_matches, search_domain, HasTagSimpleMatch(parent_context));
        } else {
            std::string name = boost::to_upper_copy<std::string>(m_name->Eval(parent_context));
            EvalImpl(matches, non_matches, search_domain, HasTagSimpleMatch(name, parent_context));
        }
    } else {
        // re-evaluate allowed turn range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string HasTag::Description(bool negated/* = false*/) const {
    std::string name_str;
    if (m_name) {
        name_str = m_name->Description();
        if (m_name->ConstantExpr() && UserStringExists(name_str))
            name_str = UserString(name_str);
    }
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_HAS_TAG")
        : UserString("DESC_HAS_TAG_NOT"))
        % name_str);
}

std::string HasTag::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "HasTag";
    if (m_name)
        retval += " name = " + m_name->Dump(ntabs);
    retval += "\n";
    return retval;
}

bool HasTag::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "HasTag::Match passed no candidate object";
        return false;
    }

    if (!m_name)
        return HasTagSimpleMatch(local_context)(candidate);

    std::string name = boost::to_upper_copy<std::string>(m_name->Eval(local_context));
    return HasTagSimpleMatch(name, local_context)(candidate);
}

void HasTag::SetTopLevelContent(const std::string& content_name) {
    if (m_name)
        m_name->SetTopLevelContent(content_name);
}

unsigned int HasTag::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::HasTag");
    CheckSums::CheckSumCombine(retval, m_name);

    TraceLogger() << "GetCheckSum(HasTag): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> HasTag::Clone() const
{ return std::make_unique<HasTag>(ValueRef::CloneUnique(m_name)); }

///////////////////////////////////////////////////////////
// CreatedOnTurn                                         //
///////////////////////////////////////////////////////////
CreatedOnTurn::CreatedOnTurn(std::unique_ptr<ValueRef::ValueRef<int>>&& low,
                             std::unique_ptr<ValueRef::ValueRef<int>>&& high) :
    Condition(),
    m_low(std::move(low)),
    m_high(std::move(high))
{
    std::array<const ValueRef::ValueRefBase*, 2> operands = {{m_low.get(), m_high.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

bool CreatedOnTurn::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const CreatedOnTurn& rhs_ = static_cast<const CreatedOnTurn&>(rhs);

    CHECK_COND_VREF_MEMBER(m_low)
    CHECK_COND_VREF_MEMBER(m_high)

    return true;
}

namespace {
    struct CreatedOnTurnSimpleMatch {
        CreatedOnTurnSimpleMatch(int low, int high) :
            m_low(low),
            m_high(high)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;
            int turn = candidate->CreationTurn();
            return m_low <= turn && turn <= m_high;
        }

        int m_low;
        int m_high;
    };
}

void CreatedOnTurn::Eval(const ScriptingContext& parent_context,
                         ObjectSet& matches, ObjectSet& non_matches,
                         SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = ((!m_low || m_low->LocalCandidateInvariant()) &&
                             (!m_high || m_high->LocalCandidateInvariant()) &&
                             (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        int low = (m_low ? m_low->Eval(parent_context) : BEFORE_FIRST_TURN);
        int high = (m_high ? m_high->Eval(parent_context) : IMPOSSIBLY_LARGE_TURN);
        EvalImpl(matches, non_matches, search_domain, CreatedOnTurnSimpleMatch(low, high));
    } else {
        // re-evaluate allowed turn range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string CreatedOnTurn::Description(bool negated/* = false*/) const {
    std::string low_str = (m_low ? (m_low->ConstantExpr() ?
                                    std::to_string(m_low->Eval()) :
                                    m_low->Description())
                                 : std::to_string(BEFORE_FIRST_TURN));
    std::string high_str = (m_high ? (m_high->ConstantExpr() ?
                                      std::to_string(m_high->Eval()) :
                                      m_high->Description())
                                   : std::to_string(IMPOSSIBLY_LARGE_TURN));
    return str(FlexibleFormat((!negated)
            ? UserString("DESC_CREATED_ON_TURN")
            : UserString("DESC_CREATED_ON_TURN_NOT"))
               % low_str
               % high_str);
}

std::string CreatedOnTurn::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "CreatedOnTurn";
    if (m_low)
        retval += " low = " + m_low->Dump(ntabs);
    if (m_high)
        retval += " high = " + m_high->Dump(ntabs);
    retval += "\n";
    return retval;
}

bool CreatedOnTurn::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "CreatedOnTurn::Match passed no candidate object";
        return false;
    }
    int turn = candidate->CreationTurn();
    int low = (m_low ? std::max(0, m_low->Eval(local_context)) : BEFORE_FIRST_TURN);
    if (low > turn)
        return false;
    int high = (m_high ? std::min(m_high->Eval(local_context), IMPOSSIBLY_LARGE_TURN) : IMPOSSIBLY_LARGE_TURN);
    return turn <= high;
}

void CreatedOnTurn::SetTopLevelContent(const std::string& content_name) {
    if (m_low)
        m_low->SetTopLevelContent(content_name);
    if (m_high)
        m_high->SetTopLevelContent(content_name);
}

unsigned int CreatedOnTurn::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::CreatedOnTurn");
    CheckSums::CheckSumCombine(retval, m_low);
    CheckSums::CheckSumCombine(retval, m_high);

    TraceLogger() << "GetCheckSum(CreatedOnTurn): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> CreatedOnTurn::Clone() const {
    return std::make_unique<CreatedOnTurn>(ValueRef::CloneUnique(m_low),
                                           ValueRef::CloneUnique(m_high));
}

///////////////////////////////////////////////////////////
// Contains                                              //
///////////////////////////////////////////////////////////
Contains::Contains(std::unique_ptr<Condition>&& condition) :
    Condition(),
    m_condition(std::move(condition))
{
    m_root_candidate_invariant = m_condition->RootCandidateInvariant();
    m_target_invariant = m_condition->TargetInvariant();
    m_source_invariant = m_condition->SourceInvariant();
}

bool Contains::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const Contains& rhs_ = static_cast<const Contains&>(rhs);

    CHECK_COND_VREF_MEMBER(m_condition)

    return true;
}

namespace {
    struct ContainsSimpleMatch {
        ContainsSimpleMatch(const ObjectSet& subcondition_matches) :
            m_subcondition_matches_ids()
        {
            // We need a sorted container for efficiently intersecting
            // subcondition_matches with the set of objects contained in some
            // candidate object.
            // We only need ids, not objects, so we can do that conversion
            // here as well, simplifying later code.
            // Note that this constructor is called only once per
            // Contains::Eval(), its work cannot help performance when executed
            // for each candidate.
            m_subcondition_matches_ids.reserve(subcondition_matches.size());
            // gather the ids
            for (auto& obj : subcondition_matches) {
                if (obj)
                    m_subcondition_matches_ids.push_back(obj->ID());
            }
            // sort them
            std::sort(m_subcondition_matches_ids.begin(), m_subcondition_matches_ids.end());
        }

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            bool match = false;
            const auto& candidate_elements = candidate->ContainedObjectIDs(); // guaranteed O(1)

            // We need to test whether candidate_elements and m_subcondition_matches_ids have a common element.
            // We choose the strategy that is more efficient by comparing the sizes of both sets.
            if (candidate_elements.size() < m_subcondition_matches_ids.size()) {
                // candidate_elements is smaller, so we iterate it and look up each candidate element in m_subcondition_matches_ids
                for (int id : candidate_elements) {
                    // std::lower_bound requires m_subcondition_matches_ids to be sorted
                    auto matching_it = std::lower_bound(m_subcondition_matches_ids.begin(), m_subcondition_matches_ids.end(), id);

                    if (matching_it != m_subcondition_matches_ids.end() && *matching_it == id) {
                        match = true;
                        break;
                    }
                }
            } else {
                // m_subcondition_matches_ids is smaller, so we iterate it and look up each subcondition match in the set of candidate's elements
                for (int id : m_subcondition_matches_ids) {
                    // candidate->Contains() may have a faster implementation than candidate_elements->find()
                    if (candidate->Contains(id)) {
                        match = true;
                        break;
                    }
                }
            }

            return match;
        }

        std::vector<int> m_subcondition_matches_ids;
    };
}

void Contains::Eval(const ScriptingContext& parent_context,
                    ObjectSet& matches, ObjectSet& non_matches,
                    SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    unsigned int search_domain_size = (search_domain == SearchDomain::MATCHES ? matches.size() : non_matches.size());
    bool simple_eval_safe = parent_context.condition_root_candidate ||
                            RootCandidateInvariant() ||
                            search_domain_size < 2;
    if (!simple_eval_safe) {
        // re-evaluate contained objects for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
        return;
    }

    // how complicated is this containment test?
    if (((search_domain == SearchDomain::MATCHES) && matches.empty()) ||
        ((search_domain == SearchDomain::NON_MATCHES) && non_matches.empty()))
    {
        // don't need to evaluate anything...

    } else if (search_domain_size == 1) {
        // evaluate subcondition on objects contained by the candidate
        ScriptingContext local_context{
            parent_context, search_domain == SearchDomain::MATCHES ? *matches.begin() : *non_matches.begin()};

        // initialize subcondition candidates from local candidate's contents
        const ObjectMap& objects = parent_context.ContextObjects();
        ObjectSet subcondition_matches = objects.find(local_context.condition_local_candidate->ContainedObjectIDs());

        // apply subcondition to candidates
        if (!subcondition_matches.empty()) {
            ObjectSet dummy;
            m_condition->Eval(local_context, subcondition_matches, dummy, SearchDomain::MATCHES);
        }

        // move single local candidate as appropriate...
        if (search_domain == SearchDomain::MATCHES && subcondition_matches.empty()) {
            // move to non_matches
            matches.clear();
            non_matches.push_back(local_context.condition_local_candidate);
        } else if (search_domain == SearchDomain::NON_MATCHES && !subcondition_matches.empty()) {
            // move to matches
            non_matches.clear();
            matches.push_back(local_context.condition_local_candidate);
        }

    } else {
        // evaluate contained objects once using default initial candidates
        // of subcondition to find all subcondition matches in the Universe
        std::shared_ptr<const UniverseObject> no_object;
        ScriptingContext local_context{parent_context, no_object};
        ObjectSet subcondition_matches;
        m_condition->Eval(local_context, subcondition_matches);

        // check all candidates to see if they contain any subcondition matches
        EvalImpl(matches, non_matches, search_domain, ContainsSimpleMatch(subcondition_matches));
    }
}

std::string Contains::Description(bool negated/* = false*/) const {
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_CONTAINS")
        : UserString("DESC_CONTAINS_NOT"))
        % m_condition->Description());
}

std::string Contains::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "Contains condition =\n";
    retval += m_condition->Dump(ntabs+1);
    return retval;
}

void Contains::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                 ObjectSet& condition_non_targets) const
{
    // objects that can contain other objects: systems, fleets, planets
    AddSystemSet(parent_context.ContextObjects(), condition_non_targets);
    AddFleetSet(parent_context.ContextObjects(), condition_non_targets);
    AddPlanetSet(parent_context.ContextObjects(), condition_non_targets);
}

bool Contains::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "Contains::Match passed no candidate object";
        return false;
    }

    // get subcondition matches
    ObjectSet subcondition_matches;
    m_condition->Eval(local_context, subcondition_matches);

    // does candidate object contain any subcondition matches?
    for (auto& obj : subcondition_matches)
        if (candidate->Contains(obj->ID()))
            return true;

    return false;
}

void Contains::SetTopLevelContent(const std::string& content_name) {
    if (m_condition)
        m_condition->SetTopLevelContent(content_name);
}

unsigned int Contains::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Contains");
    CheckSums::CheckSumCombine(retval, m_condition);

    TraceLogger() << "GetCheckSum(Contains): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Contains::Clone() const
{ return std::make_unique<Contains>(ValueRef::CloneUnique(m_condition)); }

///////////////////////////////////////////////////////////
// ContainedBy                                           //
///////////////////////////////////////////////////////////
ContainedBy::ContainedBy(std::unique_ptr<Condition>&& condition) :
    Condition(),
    m_condition(std::move(condition))
{
    m_root_candidate_invariant = m_condition->RootCandidateInvariant();
    m_target_invariant = m_condition->TargetInvariant();
    m_source_invariant = m_condition->SourceInvariant();
}

bool ContainedBy::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const ContainedBy& rhs_ = static_cast<const ContainedBy&>(rhs);

    CHECK_COND_VREF_MEMBER(m_condition)

    return true;
}

namespace {
    struct ContainedBySimpleMatch {
        ContainedBySimpleMatch(const ObjectSet& subcondition_matches) {
            // We need a sorted container for efficiently intersecting
            // subcondition_matches with the set of objects containing some
            // candidate object.
            // We only need ids, not objects, so we can do that conversion
            // here as well, simplifying later code.
            // Note that this constructor is called only once per
            // ContainedBy::Eval(), its work cannot help performance when
            // executed for each candidate.
            m_subcondition_matches_ids.reserve(subcondition_matches.size());
            // gather the ids
            for (auto& obj : subcondition_matches) {
                if (obj)
                    m_subcondition_matches_ids.push_back(obj->ID());
            }
            // sort them
            std::sort(m_subcondition_matches_ids.begin(), m_subcondition_matches_ids.end());
        }

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            bool match = false;
            // gather the objects containing candidate
            std::vector<int> candidate_containers;
            const int candidate_id = candidate->ID();
            const int    system_id = candidate->SystemID();
            const int container_id = candidate->ContainerObjectID();
            if (   system_id != INVALID_OBJECT_ID &&    system_id != candidate_id) candidate_containers.push_back(   system_id);
            if (container_id != INVALID_OBJECT_ID && container_id !=    system_id) candidate_containers.push_back(container_id);
            // FIXME: currently, direct container and system will do. In the future, we might need a way to retrieve containers of containers

            // We need to test whether candidate_containers and m_subcondition_matches_ids have a common element.
            // We choose the strategy that is more efficient by comparing the sizes of both sets.
            if (candidate_containers.size() < m_subcondition_matches_ids.size()) {
                // candidate_containers is smaller, so we iterate it and look up each candidate container in m_subcondition_matches_ids
                for (int id : candidate_containers) {
                    // std::lower_bound requires m_subcondition_matches_ids to be sorted
                    auto matching_it = std::lower_bound(m_subcondition_matches_ids.begin(), m_subcondition_matches_ids.end(), id);

                    if (matching_it != m_subcondition_matches_ids.end() && *matching_it == id) {
                        match = true;
                        break;
                    }
                }
            } else {
                // m_subcondition_matches_ids is smaller, so we iterate it and look up each subcondition match in the set of candidate's containers
                for (int id : m_subcondition_matches_ids) {
                    // candidate->ContainedBy() may have a faster implementation than candidate_containers->find()
                    if (candidate->ContainedBy(id)) {
                        match = true;
                        break;
                    }
                }
            }

            return match;
        }

        std::vector<int> m_subcondition_matches_ids;
    };
}

void ContainedBy::Eval(const ScriptingContext& parent_context,
                       ObjectSet& matches, ObjectSet& non_matches,
                       SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    unsigned int search_domain_size = (search_domain == SearchDomain::MATCHES ? matches.size() : non_matches.size());
    bool simple_eval_safe = parent_context.condition_root_candidate ||
                            RootCandidateInvariant() ||
                            search_domain_size < 2;

    if (!simple_eval_safe) {
        // re-evaluate container objects for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
        return;
    }

    // how complicated is this containment test?
    if (((search_domain == SearchDomain::MATCHES) && matches.empty()) ||
        ((search_domain == SearchDomain::NON_MATCHES) && non_matches.empty()))
    {
        // don't need to evaluate anything...

    } else if (search_domain_size == 1) {
        // evaluate subcondition on objects that contain the candidate
        ScriptingContext local_context{
            parent_context, search_domain == SearchDomain::MATCHES ? *matches.begin() : *non_matches.begin()};

        // initialize subcondition candidates from local candidate's containers
        std::set<int> container_object_ids;
        if (local_context.condition_local_candidate->ContainerObjectID() != INVALID_OBJECT_ID)
            container_object_ids.insert(local_context.condition_local_candidate->ContainerObjectID());
        if (local_context.condition_local_candidate->SystemID() != INVALID_OBJECT_ID)
            container_object_ids.insert(local_context.condition_local_candidate->SystemID());

        const ObjectMap& objects = parent_context.ContextObjects();
        ObjectSet subcondition_matches = objects.find(container_object_ids);

        // apply subcondition to candidates
        if (!subcondition_matches.empty()) {
            ObjectSet dummy;
            m_condition->Eval(local_context, subcondition_matches, dummy, SearchDomain::MATCHES);
        }

        // move single local candidate as appropriate...
        if (search_domain == SearchDomain::MATCHES && subcondition_matches.empty()) {
            // move to non_matches
            matches.clear();
            non_matches.push_back(std::move(local_context.condition_local_candidate));
        } else if (search_domain == SearchDomain::NON_MATCHES && !subcondition_matches.empty()) {
            // move to matches
            non_matches.clear();
            matches.push_back(std::move(local_context.condition_local_candidate));
        }

    } else {
        // evaluate container objects once using default initial candidates
        // of subcondition to find all subcondition matches in the Universe
        std::shared_ptr<const UniverseObject> no_object;
        ScriptingContext local_context{parent_context, std::move(no_object)};
        ObjectSet subcondition_matches;
        m_condition->Eval(local_context, subcondition_matches);

        // check all candidates to see if they contain any subcondition matches
        EvalImpl(matches, non_matches, search_domain, ContainedBySimpleMatch(subcondition_matches));
    }
}

std::string ContainedBy::Description(bool negated/* = false*/) const {
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_CONTAINED_BY")
        : UserString("DESC_CONTAINED_BY_NOT"))
        % m_condition->Description());
}

std::string ContainedBy::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "ContainedBy condition =\n";
    retval += m_condition->Dump(ntabs+1);
    return retval;
}

void ContainedBy::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                    ObjectSet& condition_non_targets) const
{
    // objects that can be contained by other objects: fleets, planets, ships, buildings
    AddFleetSet(parent_context.ContextObjects(), condition_non_targets);
    AddPlanetSet(parent_context.ContextObjects(), condition_non_targets);
    AddShipSet(parent_context.ContextObjects(), condition_non_targets);
    AddBuildingSet(parent_context.ContextObjects(), condition_non_targets);
}

bool ContainedBy::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "ContainedBy::Match passed no candidate object";
        return false;
    }

    // get containing objects
    std::set<int> containers;
    if (candidate->SystemID() != INVALID_OBJECT_ID)
        containers.insert(candidate->SystemID());
    if (candidate->ContainerObjectID() != INVALID_OBJECT_ID && candidate->ContainerObjectID() != candidate->SystemID())
        containers.insert(candidate->ContainerObjectID());

    ObjectSet container_objects = local_context.ContextObjects().find<const UniverseObject>(containers);
    if (container_objects.empty())
        return false;   // if no containers, don't need to check them

    // do any containers match the subcondition?
    m_condition->Eval(local_context, container_objects);

    return !container_objects.empty(); 
}

void ContainedBy::SetTopLevelContent(const std::string& content_name) {
    if (m_condition)
        m_condition->SetTopLevelContent(content_name);
}

unsigned int ContainedBy::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::ContainedBy");
    CheckSums::CheckSumCombine(retval, m_condition);

    TraceLogger() << "GetCheckSum(ContainedBy): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> ContainedBy::Clone() const
{ return std::make_unique<ContainedBy>(ValueRef::CloneUnique(m_condition)); }

///////////////////////////////////////////////////////////
// InOrIsSystem                                          //
///////////////////////////////////////////////////////////
InOrIsSystem::InOrIsSystem(std::unique_ptr<ValueRef::ValueRef<int>>&& system_id) :
    Condition(),
    m_system_id(std::move(system_id))
{
    m_root_candidate_invariant = !m_system_id || m_system_id->RootCandidateInvariant();
    m_target_invariant = !m_system_id || m_system_id->TargetInvariant();
    m_source_invariant = !m_system_id || m_system_id->SourceInvariant();
}

bool InOrIsSystem::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const InOrIsSystem& rhs_ = static_cast<const InOrIsSystem&>(rhs);

    CHECK_COND_VREF_MEMBER(m_system_id)

    return true;
}

namespace {
    struct InSystemSimpleMatch {
        InSystemSimpleMatch(int system_id) :
            m_system_id(system_id)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;
            if (m_system_id == INVALID_OBJECT_ID)
                return candidate->SystemID() != INVALID_OBJECT_ID;  // match objects in any system (including any system itself)
            else
                return candidate->SystemID() == m_system_id;        // match objects in specified system (including that system itself)
        }

        int m_system_id;
    };
}

void InOrIsSystem::Eval(const ScriptingContext& parent_context,
                        ObjectSet& matches, ObjectSet& non_matches,
                        SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = !m_system_id || m_system_id->ConstantExpr() ||
                            (m_system_id->LocalCandidateInvariant() &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // evaluate system id once, and use to check all candidate objects
        int system_id = (m_system_id ? m_system_id->Eval(parent_context) : INVALID_OBJECT_ID);
        EvalImpl(matches, non_matches, search_domain, InSystemSimpleMatch(system_id));
    } else {
        // re-evaluate empire id for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string InOrIsSystem::Description(bool negated/* = false*/) const {
    std::string system_str;
    int system_id = INVALID_OBJECT_ID;
    if (m_system_id && m_system_id->ConstantExpr())
        system_id = m_system_id->Eval();
    if (auto system = Objects().get<System>(system_id))
        system_str = system->Name();
    else if (m_system_id)
        system_str = m_system_id->Description();

    std::string description_str;
    if (!system_str.empty())
        description_str = (!negated)
            ? UserString("DESC_IN_SYSTEM")
            : UserString("DESC_IN_SYSTEM_NOT");
    else
        description_str = (!negated)
            ? UserString("DESC_IN_SYSTEM_SIMPLE")
            : UserString("DESC_IN_SYSTEM_SIMPLE_NOT");

    return str(FlexibleFormat(description_str) % system_str);
}

std::string InOrIsSystem::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "InSystem";
    if (m_system_id)
        retval += " id = " + m_system_id->Dump(ntabs);
    retval += "\n";
    return retval;
}

void InOrIsSystem::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                     ObjectSet& condition_non_targets) const
{
    if (!m_system_id) {
        // can match objects in any system, or any system
        AddAllObjectsSet(parent_context.ContextObjects(), condition_non_targets);
        return;
    }

    bool simple_eval_safe = m_system_id->ConstantExpr() ||
                            (m_system_id->LocalCandidateInvariant() &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant()));

    if (!simple_eval_safe) {
        // almost anything can be in a system, and can also match the system itself
        AddAllObjectsSet(parent_context.ContextObjects(), condition_non_targets);
        return;
    }

    // simple case of a single specified system id; can add just objects in that system
    int system_id = m_system_id->Eval(parent_context);
    auto system = parent_context.ContextObjects().get<System>(system_id);
    if (!system)
        return;

    // could assign directly to condition_non_targets but don't want to assume it will be initially empty
    auto sys_objs = parent_context.ContextObjects().find(system->ObjectIDs());  // excludes system itself

    // insert all objects that have the specified system id
    condition_non_targets.reserve(sys_objs.size() + 1);
    std::copy(std::make_move_iterator(sys_objs.begin()),
              std::make_move_iterator(sys_objs.end()),
              std::back_inserter(condition_non_targets));
    // also insert system itself
    condition_non_targets.push_back(std::move(system));
}

bool InOrIsSystem::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "InOrIsSystem::Match passed no candidate object";
        return false;
    }
    int system_id = (m_system_id ? m_system_id->Eval(local_context) : INVALID_OBJECT_ID);
    return InSystemSimpleMatch(system_id)(candidate);
}

void InOrIsSystem::SetTopLevelContent(const std::string& content_name) {
    if (m_system_id)
        m_system_id->SetTopLevelContent(content_name);
}

unsigned int InOrIsSystem::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::InOrIsSystem");
    CheckSums::CheckSumCombine(retval, m_system_id);

    TraceLogger() << "GetCheckSum(InOrIsSystem): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> InOrIsSystem::Clone() const
{ return std::make_unique<InOrIsSystem>(ValueRef::CloneUnique(m_system_id)); }

///////////////////////////////////////////////////////////
// OnPlanet                                              //
///////////////////////////////////////////////////////////
OnPlanet::OnPlanet(std::unique_ptr<ValueRef::ValueRef<int>>&& planet_id) :
    Condition(),
    m_planet_id(std::move(planet_id))
{
    m_root_candidate_invariant = !m_planet_id || m_planet_id->RootCandidateInvariant();
    m_target_invariant = !m_planet_id || m_planet_id->TargetInvariant();
    m_source_invariant = !m_planet_id || m_planet_id->SourceInvariant();
}

bool OnPlanet::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const OnPlanet& rhs_ = static_cast<const OnPlanet&>(rhs);

    CHECK_COND_VREF_MEMBER(m_planet_id)

    return true;
}

namespace {
    struct OnPlanetSimpleMatch {
        OnPlanetSimpleMatch(int planet_id) :
            m_planet_id(planet_id)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;
            if (candidate->ObjectType() != UniverseObjectType::OBJ_BUILDING)
                return false;
            auto* building = static_cast<const ::Building*>(candidate.get());

            if (m_planet_id == INVALID_OBJECT_ID)
                return building->PlanetID() != INVALID_OBJECT_ID;  // match objects on any planet
            else
                return building->PlanetID() == m_planet_id;        // match objects on specified planet
        }

        int m_planet_id;
    };
}

void OnPlanet::Eval(const ScriptingContext& parent_context,
                    ObjectSet& matches, ObjectSet& non_matches,
                    SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = !m_planet_id || m_planet_id->ConstantExpr() ||
                            (m_planet_id->LocalCandidateInvariant() &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // evaluate planet id once, and use to check all candidate objects
        int planet_id = (m_planet_id ? m_planet_id->Eval(parent_context) : INVALID_OBJECT_ID);
        EvalImpl(matches, non_matches, search_domain, OnPlanetSimpleMatch(planet_id));
    } else {
        // re-evaluate empire id for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string OnPlanet::Description(bool negated/* = false*/) const {
    std::string planet_str;
    int planet_id = INVALID_OBJECT_ID;
    if (m_planet_id && m_planet_id->ConstantExpr())
        planet_id = m_planet_id->Eval();
    if (auto planet = Objects().get<Planet>(planet_id))
        planet_str = planet->Name();
    else if (m_planet_id)
        planet_str = m_planet_id->Description();

    std::string description_str;
    if (!planet_str.empty())
        description_str = (!negated)
            ? UserString("DESC_ON_PLANET")
            : UserString("DESC_ON_PLANET_NOT");
    else
        description_str = (!negated)
            ? UserString("DESC_ON_PLANET_SIMPLE")
            : UserString("DESC_ON_PLANET_SIMPLE_NOT");

    return str(FlexibleFormat(description_str) % planet_str);
}

std::string OnPlanet::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "OnPlanet";
    if (m_planet_id)
        retval += " id = " + m_planet_id->Dump(ntabs);
    retval += "\n";
    return retval;
}

void OnPlanet::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                 ObjectSet& condition_non_targets) const
{
    if (!m_planet_id) {
        // only buildings can be on planets
        AddBuildingSet(parent_context.ContextObjects(), condition_non_targets);
        return;
    }

    bool simple_eval_safe = m_planet_id->ConstantExpr() ||
                            (m_planet_id->LocalCandidateInvariant() &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant()));

    if (!simple_eval_safe) {
        // only buildings can be on planets
        AddBuildingSet(parent_context.ContextObjects(), condition_non_targets);
        return;
    }

    // simple case of a single specified system id; can add just objects in that system
    int planet_id = m_planet_id->Eval(parent_context);
    auto planet = parent_context.ContextObjects().get<Planet>(planet_id);
    if (!planet)
        return;

    // insert all objects that have the specified planet id
    condition_non_targets = parent_context.ContextObjects().find(planet->BuildingIDs());
}

bool OnPlanet::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "OnPlanet::Match passed no candidate object";
        return false;
    }
    int planet_id = (m_planet_id ? m_planet_id->Eval(local_context) : INVALID_OBJECT_ID);
    return OnPlanetSimpleMatch(planet_id)(candidate);
}

void OnPlanet::SetTopLevelContent(const std::string& content_name) {
    if (m_planet_id)
        m_planet_id->SetTopLevelContent(content_name);
}

unsigned int OnPlanet::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::OnPlanet");
    CheckSums::CheckSumCombine(retval, m_planet_id);

    TraceLogger() << "GetCheckSum(OnPlanet): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> OnPlanet::Clone() const
{ return std::make_unique<OnPlanet>(ValueRef::CloneUnique(m_planet_id)); }

///////////////////////////////////////////////////////////
// ObjectID                                              //
///////////////////////////////////////////////////////////
ObjectID::ObjectID(std::unique_ptr<ValueRef::ValueRef<int>>&& object_id) :
    Condition(),
    m_object_id(std::move(object_id))
{
    m_root_candidate_invariant = !m_object_id || m_object_id->RootCandidateInvariant();
    m_target_invariant = !m_object_id || m_object_id->TargetInvariant();
    m_source_invariant = !m_object_id || m_object_id->SourceInvariant();
}

bool ObjectID::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const ObjectID& rhs_ = static_cast<const ObjectID&>(rhs);

    CHECK_COND_VREF_MEMBER(m_object_id)

    return true;
}

namespace {
    struct ObjectIDSimpleMatch {
        ObjectIDSimpleMatch(int object_id) :
            m_object_id(object_id)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            return candidate &&
                m_object_id != INVALID_OBJECT_ID &&
                candidate->ID() == m_object_id;
        }

        int m_object_id;
    };
}

void ObjectID::Eval(const ScriptingContext& parent_context,
                    ObjectSet& matches, ObjectSet& non_matches,
                    SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = !m_object_id || m_object_id->ConstantExpr() ||
                            (m_object_id->LocalCandidateInvariant() &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // evaluate empire id once, and use to check all candidate objects
        int object_id = (m_object_id ? m_object_id->Eval(parent_context) : INVALID_OBJECT_ID);
        EvalImpl(matches, non_matches, search_domain, ObjectIDSimpleMatch(object_id));
    } else {
        // re-evaluate empire id for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string ObjectID::Description(bool negated/* = false*/) const {
    std::string object_str;
    int object_id = INVALID_OBJECT_ID;
    if (m_object_id && m_object_id->ConstantExpr())
        object_id = m_object_id->Eval();
    if (auto system = Objects().get<System>(object_id))
        object_str = system->Name();
    else if (m_object_id)
        object_str = m_object_id->Description();
    else
        object_str = UserString("ERROR");   // should always have a valid ID for this condition

    return str(FlexibleFormat((!negated)
        ? UserString("DESC_OBJECT_ID")
        : UserString("DESC_OBJECT_ID_NOT"))
               % object_str);
}

std::string ObjectID::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "Object id = " + m_object_id->Dump(ntabs) + "\n"; }

void ObjectID::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                 ObjectSet& condition_non_targets) const
{
    if (!m_object_id)
        return;

    bool simple_eval_safe = m_object_id->ConstantExpr() ||
                            (m_object_id->LocalCandidateInvariant() &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant()));

    if (!simple_eval_safe) {
        AddAllObjectsSet(parent_context.ContextObjects(), condition_non_targets);
        return;
    }

    // simple case of a single specified id; can add just that object
    int object_id = m_object_id->Eval(parent_context);
    if (object_id == INVALID_OBJECT_ID)
        return;

    if (auto obj = parent_context.ContextObjects().ExistingObject(object_id))
        condition_non_targets.push_back(std::move(obj));
}

bool ObjectID::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "ObjectID::Match passed no candidate object";
        return false;
    }

    return ObjectIDSimpleMatch(m_object_id->Eval(local_context))(candidate);
}

void ObjectID::SetTopLevelContent(const std::string& content_name) {
    if (m_object_id)
        m_object_id->SetTopLevelContent(content_name);
}

unsigned int ObjectID::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::ObjectID");
    CheckSums::CheckSumCombine(retval, m_object_id);

    TraceLogger() << "GetCheckSum(ObjectID): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> ObjectID::Clone() const
{ return std::make_unique<ObjectID>(ValueRef::CloneUnique(m_object_id)); }

///////////////////////////////////////////////////////////
// PlanetType                                            //
///////////////////////////////////////////////////////////
PlanetType::PlanetType(std::vector<std::unique_ptr<ValueRef::ValueRef< ::PlanetType>>>&& types) :
    Condition(),
    m_types(std::move(types))
{
    m_root_candidate_invariant = boost::algorithm::all_of(m_types, [](auto& e){ return e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(m_types, [](auto& e){ return e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(m_types, [](auto& e){ return e->SourceInvariant(); });
}

bool PlanetType::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const PlanetType& rhs_ = static_cast<const PlanetType&>(rhs);

    if (m_types.size() != rhs_.m_types.size())
        return false;
    for (unsigned int i = 0; i < m_types.size(); ++i) {
        CHECK_COND_VREF_MEMBER(m_types.at(i))
    }

    return true;
}

namespace {
    struct PlanetTypeSimpleMatch {
        PlanetTypeSimpleMatch(const std::vector< ::PlanetType>& types, const ObjectMap& objects) :
            m_types(types),
            m_objects(objects)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            // is it a planet or on a planet?
            auto planet = std::dynamic_pointer_cast<const Planet>(candidate);
            std::shared_ptr<const ::Building> building;
            if (!planet && (building = std::dynamic_pointer_cast<const ::Building>(candidate)))
                planet = m_objects.get<Planet>(building->PlanetID());
            if (planet) {
                // is it one of the specified building types?
                return std::count(m_types.begin(), m_types.end(), planet->Type());
            }

            return false;
        }

        const std::vector< ::PlanetType>& m_types;
        const ObjectMap& m_objects;
    };
}

void PlanetType::Eval(const ScriptingContext& parent_context,
                      ObjectSet& matches, ObjectSet& non_matches,
                      SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = parent_context.condition_root_candidate || RootCandidateInvariant();
    if (simple_eval_safe) {
        // check each valueref for invariance to local candidate
        for (auto& type : m_types) {
            if (!type->LocalCandidateInvariant()) {
                simple_eval_safe = false;
                break;
            }
        }
    }
    if (simple_eval_safe) {
        // evaluate types once, and use to check all candidate objects
        std::vector< ::PlanetType> types;
        types.reserve(m_types.size());
        // get all types from valuerefs
        for (auto& type : m_types)
            types.push_back(type->Eval(parent_context));
        EvalImpl(matches, non_matches, search_domain, PlanetTypeSimpleMatch(types, parent_context.ContextObjects()));
    } else {
        // re-evaluate contained objects for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string PlanetType::Description(bool negated/* = false*/) const {
    std::string values_str;
    for (unsigned int i = 0; i < m_types.size(); ++i) {
        values_str += m_types[i]->ConstantExpr() ?
                        UserString(boost::lexical_cast<std::string>(m_types[i]->Eval())) :
                        m_types[i]->Description();
        if (2 <= m_types.size() && i < m_types.size() - 2) {
            values_str += ", ";
        } else if (i == m_types.size() - 2) {
            values_str += m_types.size() < 3 ? " " : ", ";
            values_str += UserString("OR");
            values_str += " ";
        }
    }
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_PLANET_TYPE")
        : UserString("DESC_PLANET_TYPE_NOT"))
        % values_str);
}

std::string PlanetType::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "Planet type = ";
    if (m_types.size() == 1) {
        retval += m_types[0]->Dump(ntabs) + "\n";
    } else {
        retval += "[ ";
        for (auto& type : m_types) {
            retval += type->Dump(ntabs) + " ";
        }
        retval += "]\n";
    }
    return retval;
}

void PlanetType::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                   ObjectSet& condition_non_targets) const
{
    AddPlanetSet(parent_context.ContextObjects(), condition_non_targets);
    AddBuildingSet(parent_context.ContextObjects(), condition_non_targets);
}

bool PlanetType::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "PlanetType::Match passed no candidate object";
        return false;
    }

    auto planet = std::dynamic_pointer_cast<const Planet>(candidate);
    std::shared_ptr<const ::Building> building;
    if (!planet && (building = std::dynamic_pointer_cast<const ::Building>(candidate)))
        planet = local_context.ContextObjects().get<Planet>(building->PlanetID());

    if (planet) {
        for (auto& type : m_types) {
            if (type->Eval(local_context) == planet->Type())
                return true;
        }
    }
    return false;
}

void PlanetType::SetTopLevelContent(const std::string& content_name) {
    for (auto& type : m_types) {
        if (type)
            type->SetTopLevelContent(content_name);
    }
}

unsigned int PlanetType::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::PlanetType");
    CheckSums::CheckSumCombine(retval, m_types);

    TraceLogger() << "GetCheckSum(PlanetType): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> PlanetType::Clone() const
{ return std::make_unique<PlanetType>(ValueRef::CloneUnique(m_types)); }

///////////////////////////////////////////////////////////
// PlanetSize                                            //
///////////////////////////////////////////////////////////
PlanetSize::PlanetSize(std::vector<std::unique_ptr<ValueRef::ValueRef< ::PlanetSize>>>&& sizes) :
    Condition(),
    m_sizes(std::move(sizes))
{
    m_root_candidate_invariant = boost::algorithm::all_of(m_sizes, [](auto& e){ return e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(m_sizes, [](auto& e){ return e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(m_sizes, [](auto& e){ return e->SourceInvariant(); });
}

bool PlanetSize::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const PlanetSize& rhs_ = static_cast<const PlanetSize&>(rhs);

    if (m_sizes.size() != rhs_.m_sizes.size())
        return false;
    for (unsigned int i = 0; i < m_sizes.size(); ++i) {
        CHECK_COND_VREF_MEMBER(m_sizes.at(i))
    }

    return true;
}

namespace {
    struct PlanetSizeSimpleMatch {
        PlanetSizeSimpleMatch(const std::vector< ::PlanetSize>& sizes, const ObjectMap& objects) :
            m_sizes(sizes),
            m_objects(objects)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            // is it a planet or on a planet? TODO: This concept should be generalized and factored out.
            auto planet = std::dynamic_pointer_cast<const Planet>(candidate);
            std::shared_ptr<const ::Building> building;
            if (!planet && (building = std::dynamic_pointer_cast<const ::Building>(candidate)))
                planet = m_objects.get<Planet>(building->PlanetID());
            if (planet) {
                // is it one of the specified building types?
                for (auto size : m_sizes) {
                    if (planet->Size() == size)
                        return true;
                }
            }

            return false;
        }

        const std::vector< ::PlanetSize>& m_sizes;
        const ObjectMap& m_objects;
    };
}

void PlanetSize::Eval(const ScriptingContext& parent_context,
                      ObjectSet& matches, ObjectSet& non_matches,
                      SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = parent_context.condition_root_candidate || RootCandidateInvariant();
    if (simple_eval_safe) {
        // check each valueref for invariance to local candidate
        for (auto& size : m_sizes) {
            if (!size->LocalCandidateInvariant()) {
                simple_eval_safe = false;
                break;
            }
        }
    }
    if (simple_eval_safe) {
        // evaluate types once, and use to check all candidate objects
        std::vector< ::PlanetSize> sizes;
        sizes.reserve(m_sizes.size());
        // get all types from valuerefs  TODO: could lazy-evaluate m_sizes vs. find all then pass in...?
        for (auto& size : m_sizes)
            sizes.push_back(size->Eval(parent_context));
        EvalImpl(matches, non_matches, search_domain, PlanetSizeSimpleMatch(sizes, parent_context.ContextObjects()));
    } else {
        // re-evaluate contained objects for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string PlanetSize::Description(bool negated/* = false*/) const {
    std::string values_str;
    for (unsigned int i = 0; i < m_sizes.size(); ++i) {
        values_str += m_sizes[i]->ConstantExpr() ?
                        UserString(boost::lexical_cast<std::string>(m_sizes[i]->Eval())) :
                        m_sizes[i]->Description();
        if (2 <= m_sizes.size() && i < m_sizes.size() - 2) {
            values_str += ", ";
        } else if (i == m_sizes.size() - 2) {
            values_str += m_sizes.size() < 3 ? " " : ", ";
            values_str += UserString("OR");
            values_str += " ";
        }
    }
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_PLANET_SIZE")
        : UserString("DESC_PLANET_SIZE_NOT"))
        % values_str);
}

std::string PlanetSize::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "Planet size = ";
    if (m_sizes.size() == 1) {
        retval += m_sizes[0]->Dump(ntabs) + "\n";
    } else {
        retval += "[ ";
        for (auto& size : m_sizes) {
            retval += size->Dump(ntabs) + " ";
        }
        retval += "]\n";
    }
    return retval;
}

void PlanetSize::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                   ObjectSet& condition_non_targets) const
{
    AddPlanetSet(parent_context.ContextObjects(), condition_non_targets);
    AddBuildingSet(parent_context.ContextObjects(), condition_non_targets);
}

bool PlanetSize::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "PlanetSize::Match passed no candidate object";
        return false;
    }

    auto planet = std::dynamic_pointer_cast<const Planet>(candidate);
    std::shared_ptr<const ::Building> building;
    if (!planet && (building = std::dynamic_pointer_cast<const ::Building>(candidate)))
        planet = local_context.ContextObjects().get<Planet>(building->PlanetID());

    if (planet) {
        for (auto& size : m_sizes) {
            if (size->Eval(local_context) == planet->Size())
                return true;
        }
    }
    return false;
}

void PlanetSize::SetTopLevelContent(const std::string& content_name) {
    for (auto& size : m_sizes) {
        if (size)
            size->SetTopLevelContent(content_name);
    }
}

unsigned int PlanetSize::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::PlanetSize");
    CheckSums::CheckSumCombine(retval, m_sizes);

    TraceLogger() << "GetCheckSum(PlanetSize): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> PlanetSize::Clone() const
{ return std::make_unique<PlanetSize>(ValueRef::CloneUnique(m_sizes)); }

///////////////////////////////////////////////////////////
// PlanetEnvironment                                     //
///////////////////////////////////////////////////////////
PlanetEnvironment::PlanetEnvironment(std::vector<std::unique_ptr<ValueRef::ValueRef< ::PlanetEnvironment>>>&& environments,
                                     std::unique_ptr<ValueRef::ValueRef<std::string>>&& species_name_ref) :
    Condition(),
    m_environments(std::move(environments)),
    m_species_name(std::move(species_name_ref))
{
    m_root_candidate_invariant =
        (!m_species_name || m_species_name->RootCandidateInvariant()) &&
        boost::algorithm::all_of(m_environments, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant =
        (!m_species_name || m_species_name->TargetInvariant()) &&
        boost::algorithm::all_of(m_environments, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant =
        (!m_species_name || m_species_name->SourceInvariant()) &&
        boost::algorithm::all_of(m_environments, [](auto& e){ return !e || e->SourceInvariant(); });
}

bool PlanetEnvironment::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const PlanetEnvironment& rhs_ = static_cast<const PlanetEnvironment&>(rhs);

    CHECK_COND_VREF_MEMBER(m_species_name)

    if (m_environments.size() != rhs_.m_environments.size())
        return false;
    for (unsigned int i = 0; i < m_environments.size(); ++i) {
        CHECK_COND_VREF_MEMBER(m_environments.at(i))
    }

    return true;
}

namespace {
    struct PlanetEnvironmentSimpleMatch {
        PlanetEnvironmentSimpleMatch(const std::vector< ::PlanetEnvironment>& environments,
                                     const ObjectMap& objects,
                                     const std::string& species = "") :
            m_environments(environments),
            m_species(species),
            m_objects(objects)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            // is it a planet or on a planet? TODO: factor out
            std::shared_ptr<const Planet> planet;
            if (candidate->ObjectType() == UniverseObjectType::OBJ_PLANET)
                planet = std::static_pointer_cast<const ::Planet>(candidate);
            if (!planet && candidate->ObjectType() == UniverseObjectType::OBJ_BUILDING) {
                const auto* building = static_cast<const ::Building*>(candidate.get());
                planet = m_objects.get<Planet>(building->PlanetID());
            }
            if (!planet)
                return false;

            // if no species specified, use planet's own species
            const auto& species_to_check = m_species.empty() ? planet->SpeciesName() : m_species;
            // if no species specified and planet has no species, can't match
            if (species_to_check.empty())
                return false;

            // get plaent's environment for specified species, and check if it matches any of the indicated environments
            auto planet_env = planet->EnvironmentForSpecies(species_to_check);
            for (auto environment : m_environments) {
                if (environment == planet_env)
                    return true;
            }

            return false;
        }

        const std::vector< ::PlanetEnvironment>&    m_environments;
        const std::string&                          m_species;
        const ObjectMap&                            m_objects;
    };
}

void PlanetEnvironment::Eval(const ScriptingContext& parent_context,
                             ObjectSet& matches, ObjectSet& non_matches,
                             SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = ((!m_species_name || m_species_name->LocalCandidateInvariant()) &&
                             (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // check each valueref for invariance to local candidate
        for (auto& environment : m_environments) {
            if (!environment->LocalCandidateInvariant()) {
                simple_eval_safe = false;
                break;
            }
        }
    }
    if (simple_eval_safe) {
        // evaluate types once, and use to check all candidate objects
        std::vector< ::PlanetEnvironment> environments;
        environments.reserve(m_environments.size());
        // get all types from valuerefs
        for (auto& environment : m_environments)
            environments.push_back(environment->Eval(parent_context));
        std::string species_name{m_species_name ? m_species_name->Eval(parent_context) : ""};
        EvalImpl(matches, non_matches, search_domain,
                 PlanetEnvironmentSimpleMatch(environments, parent_context.ContextObjects(), species_name));
    } else {
        // re-evaluate contained objects for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string PlanetEnvironment::Description(bool negated/* = false*/) const {
    std::string values_str;
    for (unsigned int i = 0; i < m_environments.size(); ++i) {
        values_str += m_environments[i]->ConstantExpr() ?
                        UserString(boost::lexical_cast<std::string>(m_environments[i]->Eval())) :
                        m_environments[i]->Description();
        if (2 <= m_environments.size() && i < m_environments.size() - 2) {
            values_str += ", ";
        } else if (i == m_environments.size() - 2) {
            values_str += m_environments.size() < 3 ? " " : ", ";
            values_str += UserString("OR");
            values_str += " ";
        }
    }
    std::string species_str;
    if (m_species_name) {
        species_str = m_species_name->Description();
        if (m_species_name->ConstantExpr() && UserStringExists(species_str))
            species_str = UserString(species_str);
    }
    if (species_str.empty())
        species_str = UserString("DESC_PLANET_ENVIRONMENT_CUR_SPECIES");
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_PLANET_ENVIRONMENT")
        : UserString("DESC_PLANET_ENVIRONMENT_NOT"))
        % values_str
        % species_str);
}

std::string PlanetEnvironment::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "Planet environment = ";
    if (m_environments.size() == 1) {
        retval += m_environments[0]->Dump(ntabs);
    } else {
        retval += "[ ";
        for (auto& environment : m_environments) {
            retval += environment->Dump(ntabs) + " ";
        }
        retval += "]";
    }
    if (m_species_name)
        retval += " species = " + m_species_name->Dump(ntabs);
    retval += "\n";
    return retval;
}

void PlanetEnvironment::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                          ObjectSet& condition_non_targets) const
{
    AddPlanetSet(parent_context.ContextObjects(), condition_non_targets);
    AddBuildingSet(parent_context.ContextObjects(), condition_non_targets);
}

bool PlanetEnvironment::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "PlanetEnvironment::Match passed no candidate object";
        return false;
    }

    // is it a planet or on a planet? TODO: factor out
    auto planet = std::dynamic_pointer_cast<const Planet>(candidate);
    std::shared_ptr<const ::Building> building;
    if (!planet && (building = std::dynamic_pointer_cast<const ::Building>(candidate)))
        planet = local_context.ContextObjects().get<Planet>(building->PlanetID());
    if (!planet)
        return false;

    std::string species_name;
    if (m_species_name)
        species_name = m_species_name->Eval(local_context);

    auto env_for_planets_species = planet->EnvironmentForSpecies(species_name);
    for (auto& environment : m_environments) {
        if (environment->Eval(local_context) == env_for_planets_species)
            return true;
    }
    return false;
}

void PlanetEnvironment::SetTopLevelContent(const std::string& content_name) {
    if (m_species_name)
        m_species_name->SetTopLevelContent(content_name);
    for (auto& environment : m_environments) {
        if (environment)
            environment->SetTopLevelContent(content_name);
    }
}

unsigned int PlanetEnvironment::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::PlanetEnvironment");
    CheckSums::CheckSumCombine(retval, m_environments);
    CheckSums::CheckSumCombine(retval, m_species_name);

    TraceLogger() << "GetCheckSum(PlanetEnvironment): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> PlanetEnvironment::Clone() const {
    return std::make_unique<PlanetEnvironment>(ValueRef::CloneUnique(m_environments),
                                               ValueRef::CloneUnique(m_species_name));
}

///////////////////////////////////////////////////////////
// Species                                               //
///////////////////////////////////////////////////////////
Species::Species(std::vector<std::unique_ptr<ValueRef::ValueRef<std::string>>>&& names) :
    Condition(),
    m_names(std::move(names))
{
    m_root_candidate_invariant = boost::algorithm::all_of(m_names, [](auto& e){ return e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(m_names, [](auto& e){ return e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(m_names, [](auto& e){ return e->SourceInvariant(); });
}

Species::Species() :
    Species(std::vector<std::unique_ptr<ValueRef::ValueRef<std::string>>>{})
{}

bool Species::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const Species& rhs_ = static_cast<const Species&>(rhs);

    if (m_names.size() != rhs_.m_names.size())
        return false;
    for (unsigned int i = 0; i < m_names.size(); ++i) {
        CHECK_COND_VREF_MEMBER(m_names.at(i))
    }

    return true;
}

namespace {
    const std::string& GetCandidateSpecies(const UniverseObject* candidate,
                                           const ObjectMap& objects)
    {
        // is it a population centre?
        auto obj_type = candidate->ObjectType();
        if (obj_type == UniverseObjectType::OBJ_PLANET) {
            auto* pop = static_cast<const ::Planet*>(candidate);
            return pop->SpeciesName();
        }
        else if (obj_type == UniverseObjectType::OBJ_SHIP) {
            auto* ship = static_cast<const Ship*>(candidate);
            return ship->SpeciesName();
        }
        else if (obj_type == UniverseObjectType::OBJ_BUILDING) {
            // is it a building on a planet?
            auto* building = static_cast<const ::Building*>(candidate);
            if (auto planet = objects.get<Planet>(building->PlanetID()))
                return planet->SpeciesName();
        }
        return EMPTY_STRING;
    }

    struct SpeciesSimpleMatch {
        SpeciesSimpleMatch(const std::vector<std::string>& names, const ObjectMap& objects) :
            m_names(names),
            m_objects(objects)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            auto& species_name{GetCandidateSpecies(candidate.get(), m_objects)};
            return !species_name.empty() && (m_names.empty() || std::count(m_names.begin(), m_names.end(), species_name));
        }

        const std::vector<std::string>& m_names;
        const ObjectMap& m_objects;
    };
}

void Species::Eval(const ScriptingContext& parent_context,
                   ObjectSet& matches, ObjectSet& non_matches,
                   SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = parent_context.condition_root_candidate || RootCandidateInvariant();
    if (simple_eval_safe) {
        // check each valueref for invariance to local candidate
        for (auto& name : m_names) {
            if (!name->LocalCandidateInvariant()) {
                simple_eval_safe = false;
                break;
            }
        }
    }
    if (simple_eval_safe) {
        // evaluate names once, and use to check all candidate objects
        std::vector<std::string> names;
        names.reserve(m_names.size());
        // get all names from valuerefs
        for (auto& name : m_names)
            names.push_back(name->Eval(parent_context));
        EvalImpl(matches, non_matches, search_domain, SpeciesSimpleMatch(names, parent_context.ContextObjects()));
    } else {
        // re-evaluate allowed building types range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string Species::Description(bool negated/* = false*/) const {
    std::string values_str;
    if (m_names.empty())
        values_str = "(" + UserString("CONDITION_ANY") +")";
    for (unsigned int i = 0; i < m_names.size(); ++i) {
        values_str += m_names[i]->ConstantExpr() ?
                        UserString(m_names[i]->Eval()) :
                        m_names[i]->Description();
        if (2 <= m_names.size() && i < m_names.size() - 2) {
            values_str += ", ";
        } else if (i == m_names.size() - 2) {
            values_str += m_names.size() < 3 ? " " : ", ";
            values_str += UserString("OR");
            values_str += " ";
        }
    }
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_SPECIES")
        : UserString("DESC_SPECIES_NOT"))
        % values_str);
}

std::string Species::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "Species";
    if (m_names.empty()) {
        retval += "\n";
    } else if (m_names.size() == 1) {
        retval += " name = " + m_names[0]->Dump(ntabs) + "\n";
    } else {
        retval += " name = [ ";
        for (auto& name : m_names) {
            retval += name->Dump(ntabs) + " ";
        }
        retval += "]\n";
    }
    return retval;
}

void Species::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                ObjectSet& condition_non_targets) const
{
    AddPlanetSet(parent_context.ContextObjects(), condition_non_targets);
    AddBuildingSet(parent_context.ContextObjects(), condition_non_targets);
    AddShipSet(parent_context.ContextObjects(), condition_non_targets);
}

bool Species::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "Species::Match passed no candidate object";
        return false;
    }

    auto& species_name{GetCandidateSpecies(candidate.get(), local_context.ContextObjects())};

    if (m_names.empty())
        return !species_name.empty();

    for (auto& name : m_names) {
        if (name->Eval(local_context) == species_name)
            return true;
    }

    return false;
}

void Species::SetTopLevelContent(const std::string& content_name) {
    for (auto& name : m_names) {
        if (name)
            name->SetTopLevelContent(content_name);
    }
}

unsigned int Species::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Species");
    CheckSums::CheckSumCombine(retval, m_names);

    TraceLogger() << "GetCheckSum(Species): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Species::Clone() const
{ return std::make_unique<Species>(ValueRef::CloneUnique(m_names)); }

///////////////////////////////////////////////////////////
// Enqueued                                              //
///////////////////////////////////////////////////////////
Enqueued::Enqueued(std::unique_ptr<ValueRef::ValueRef<int>>&& design_id,
                   std::unique_ptr<ValueRef::ValueRef<int>>&& empire_id,
                   std::unique_ptr<ValueRef::ValueRef<int>>&& low,
                   std::unique_ptr<ValueRef::ValueRef<int>>&& high) :
    Condition(),
    m_build_type(BuildType::BT_SHIP),
    m_design_id(std::move(design_id)),
    m_empire_id(std::move(empire_id)),
    m_low(std::move(low)),
    m_high(std::move(high))
{
    std::array<const ValueRef::ValueRefBase*, 4> operands =
        {{m_design_id.get(), m_empire_id.get(), m_low.get(), m_high.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

Enqueued::Enqueued() :
    Enqueued(BuildType::BT_NOT_BUILDING, nullptr, nullptr, nullptr)
{}

Enqueued::Enqueued(BuildType build_type,
                   std::unique_ptr<ValueRef::ValueRef<std::string>>&& name,
                   std::unique_ptr<ValueRef::ValueRef<int>>&& empire_id,
                   std::unique_ptr<ValueRef::ValueRef<int>>&& low,
                   std::unique_ptr<ValueRef::ValueRef<int>>&& high) :
    Condition(),
    m_build_type(build_type),
    m_name(std::move(name)),
    m_empire_id(std::move(empire_id)),
    m_low(std::move(low)),
    m_high(std::move(high))
{
    std::array<const ValueRef::ValueRefBase*, 4> operands =
        {{m_name.get(), m_empire_id.get(), m_low.get(), m_high.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

Enqueued::Enqueued(const Enqueued& rhs) :
    Condition(rhs),
    m_build_type(rhs.m_build_type),
    m_name(ValueRef::CloneUnique(rhs.m_name)),
    m_design_id(ValueRef::CloneUnique(rhs.m_design_id)),
    m_empire_id(ValueRef::CloneUnique(rhs.m_empire_id)),
    m_low(ValueRef::CloneUnique(rhs.m_low)),
    m_high(ValueRef::CloneUnique(rhs.m_high))
{
}

bool Enqueued::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const Enqueued& rhs_ = static_cast<const Enqueued&>(rhs);

    if (m_build_type != rhs_.m_build_type)
        return false;

    CHECK_COND_VREF_MEMBER(m_name)
    CHECK_COND_VREF_MEMBER(m_design_id)
    CHECK_COND_VREF_MEMBER(m_empire_id)
    CHECK_COND_VREF_MEMBER(m_low)
    CHECK_COND_VREF_MEMBER(m_high)

    return true;
}

namespace {
    int NumberOnQueue(const ProductionQueue& queue, BuildType build_type, const int location_id,
                      const Universe& universe,
                      const std::string& name = "", int design_id = INVALID_DESIGN_ID)
    {
        int retval = 0;
        for (const auto& element : queue) {
            if (!(build_type == BuildType::INVALID_BUILD_TYPE || build_type == element.item.build_type))
                continue;
            if (location_id != element.location)
                continue;
            if (build_type == BuildType::BT_BUILDING) {
                // if looking for buildings, accept specifically named building
                // or any building if no name specified
                if (!name.empty() && element.item.name != name)
                    continue;
            } else if (build_type == BuildType::BT_SHIP) {
                if (design_id != INVALID_DESIGN_ID) {
                    // if looking for ships, accept design by id number...
                    if (design_id != element.item.design_id)
                        continue;
                } else if (!name.empty()) {
                    // ... or accept design by predefined name
                    const ShipDesign* design = universe.GetShipDesign(element.item.design_id);
                    if (!design || name != design->Name(false))
                        continue;
                }
            } // else: looking for any production item

            retval += element.blocksize;
        }
        return retval;
    }

    struct EnqueuedSimpleMatch {
        EnqueuedSimpleMatch(BuildType build_type, const std::string& name, int design_id,
                            int empire_id, int low, int high, const ScriptingContext& context) :
            m_build_type(build_type),
            m_name(name),
            m_design_id(design_id),
            m_empire_id(empire_id),
            m_low(low),
            m_high(high),
            m_context(context)
        {}
        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            int count = 0;

            if (m_empire_id == ALL_EMPIRES) {
                for ([[maybe_unused]] auto& [ignored_empire_id, empire] : m_context.Empires()) {
                    (void)ignored_empire_id; // quiet unused variable warning
                    count += NumberOnQueue(empire->GetProductionQueue(), m_build_type,
                                           candidate->ID(), m_context.ContextUniverse(),
                                           m_name, m_design_id);
                }

            } else {
                auto empire = m_context.GetEmpire(m_empire_id);
                if (!empire) return false;
                count = NumberOnQueue(empire->GetProductionQueue(), m_build_type,
                                      candidate->ID(), m_context.ContextUniverse(),
                                      m_name, m_design_id);
            }

            return (m_low <= count && count <= m_high);
        }

        BuildType               m_build_type;
        const std::string&      m_name;
        int                     m_design_id;
        int                     m_empire_id;
        int                     m_low;
        int                     m_high;
        const ScriptingContext& m_context;
    };
}

void Enqueued::Eval(const ScriptingContext& parent_context,
                    ObjectSet& matches, ObjectSet& non_matches,
                    SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = parent_context.condition_root_candidate || RootCandidateInvariant();
    if (simple_eval_safe) {
        // check each valueref for invariance to local candidate
        if ((m_name &&      !m_name->LocalCandidateInvariant()) ||
            (m_design_id && !m_design_id->LocalCandidateInvariant()) ||
            (m_empire_id && !m_empire_id->LocalCandidateInvariant()) ||
            (m_low &&       !m_low->LocalCandidateInvariant()) ||
            (m_high &&      !m_high->LocalCandidateInvariant()))
        { simple_eval_safe = false; }
    }

    if (simple_eval_safe) {
        // evaluate valuerefs once, and use to check all candidate objects
        std::string name =  (m_name ?       m_name->Eval(parent_context) :      "");
        int design_id =     (m_design_id ?  m_design_id->Eval(parent_context) : INVALID_DESIGN_ID);
        int empire_id =     (m_empire_id ?  m_empire_id->Eval(parent_context) : ALL_EMPIRES);
        int low =           (m_low ?        m_low->Eval(parent_context) :       0);
        int high =          (m_high ?       m_high->Eval(parent_context) :      INT_MAX);
        // special case: if neither low nor high is specified, default to a
        // minimum of 1, so that just matching "Enqueued (type) (name/id)" will
        // match places where at least one of the specified item is enqueued.
        // if a max or other minimum are specified, then default to 0 low, so
        // that just specifying a max will include anything below that max,
        // including 0.
        if (!m_low && !m_high)
            low = 1;

        // need to test each candidate separately using EvalImpl and EnqueuedSimpleMatch
        // because the test checks that something is enqueued at the candidate location
        EvalImpl(matches, non_matches, search_domain, EnqueuedSimpleMatch(m_build_type, name, design_id,
                                                                          empire_id, low, high,
                                                                          parent_context));
    } else {
        // re-evaluate allowed building types range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string Enqueued::Description(bool negated/* = false*/) const {
    std::string empire_str;
    if (m_empire_id) {
        int empire_id = ALL_EMPIRES;
        if (m_empire_id->ConstantExpr())
            empire_id = m_empire_id->Eval();
        if (const Empire* empire = GetEmpire(empire_id))
            empire_str = empire->Name();
        else
            empire_str = m_empire_id->Description();
    }
    std::string low_str = "1";
    if (m_low) {
        low_str = m_low->ConstantExpr() ?
                    std::to_string(m_low->Eval()) :
                    m_low->Description();
    }
    std::string high_str = std::to_string(INT_MAX);
    if (m_high) {
        high_str = m_high->ConstantExpr() ?
                    std::to_string(m_high->Eval()) :
                    m_high->Description();
    }
    std::string what_str;
    if (m_name) {
        what_str = m_name->Description();
        if (m_name->ConstantExpr() && UserStringExists(what_str))
            what_str = UserString(what_str);
    } else if (m_design_id) {
        what_str = m_design_id->ConstantExpr() ?
                    std::to_string(m_design_id->Eval()) :
                    m_design_id->Description();
    }
    std::string description_str;
    switch (m_build_type) {
    case BuildType::BT_BUILDING:    description_str = (!negated)
                                    ? UserString("DESC_ENQUEUED_BUILDING")
                                    : UserString("DESC_ENQUEUED_BUILDING_NOT");
    break;
    case BuildType::BT_SHIP:        description_str = (!negated)
                                    ? UserString("DESC_ENQUEUED_DESIGN")
                                    : UserString("DESC_ENQUEUED_DESIGN_NOT");
    break;
    default:                        description_str = (!negated)
                                    ? UserString("DESC_ENQUEUED")
                                    : UserString("DESC_ENQUEUED_NOT");
    break;
    }
    return str(FlexibleFormat(description_str)
               % empire_str
               % low_str
               % high_str
               % what_str);
}

std::string Enqueued::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "Enqueued";

    if (m_build_type == BuildType::BT_BUILDING) {
        retval += " type = Building";
        if (m_name)
            retval += " name = " + m_name->Dump(ntabs);
    } else if (m_build_type == BuildType::BT_SHIP) {
        retval += " type = Ship";
        if (m_name)
            retval += " design = " + m_name->Dump(ntabs);
        else if (m_design_id)
            retval += " design = " + m_design_id->Dump(ntabs);
    }
    if (m_empire_id)
        retval += " empire = " + m_empire_id->Dump(ntabs);
    if (m_low)
        retval += " low = " + m_low->Dump(ntabs);
    if (m_high)
        retval += " high = " + m_high->Dump(ntabs);
    retval += "\n";
    return retval;
}

bool Enqueued::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "Enqueued::Match passed no candidate object";
        return false;
    }
    std::string name{   (m_name ?       m_name->Eval(local_context) :       "")};
    int empire_id =     (m_empire_id ?  m_empire_id->Eval(local_context) :  ALL_EMPIRES);
    int design_id =     (m_design_id ?  m_design_id->Eval(local_context) :  INVALID_DESIGN_ID);
    int low =           (m_low ?        m_low->Eval(local_context) :        0);
    int high =          (m_high ?       m_high->Eval(local_context) :       INT_MAX);
    return EnqueuedSimpleMatch(m_build_type, name, design_id, empire_id, low, high, local_context)(candidate);
}

void Enqueued::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                 ObjectSet& condition_non_targets) const
{
    AddPlanetSet(parent_context.ContextObjects(), condition_non_targets);
}

void Enqueued::SetTopLevelContent(const std::string& content_name) {
    if (m_name)
        m_name->SetTopLevelContent(content_name);
    if (m_design_id)
        m_design_id->SetTopLevelContent(content_name);
    if (m_empire_id)
        m_empire_id->SetTopLevelContent(content_name);
    if (m_low)
        m_low->SetTopLevelContent(content_name);
    if (m_high)
        m_high->SetTopLevelContent(content_name);
}

unsigned int Enqueued::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Enqueued");
    CheckSums::CheckSumCombine(retval, m_name);
    CheckSums::CheckSumCombine(retval, m_design_id);
    CheckSums::CheckSumCombine(retval, m_empire_id);
    CheckSums::CheckSumCombine(retval, m_low);
    CheckSums::CheckSumCombine(retval, m_high);

    TraceLogger() << "GetCheckSum(Enqueued): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Enqueued::Clone() const
{ return std::make_unique<Enqueued>(*this); }

///////////////////////////////////////////////////////////
// FocusType                                             //
///////////////////////////////////////////////////////////
FocusType::FocusType(std::vector<std::unique_ptr<ValueRef::ValueRef<std::string>>>&& names) :
    Condition(),
    m_names(std::move(names))
{
    m_root_candidate_invariant = boost::algorithm::all_of(m_names, [](auto& e){ return e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(m_names, [](auto& e){ return e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(m_names, [](auto& e){ return e->SourceInvariant(); });
}

bool FocusType::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const FocusType& rhs_ = static_cast<const FocusType&>(rhs);

    if (m_names.size() != rhs_.m_names.size())
        return false;
    for (unsigned int i = 0; i < m_names.size(); ++i) {
        CHECK_COND_VREF_MEMBER(m_names.at(i))
    }

    return true;
}

namespace {
    const std::string& GetCandidateFocus(const UniverseObject* candidate,
                                         const ObjectMap& objects)
    {
        // is it a population centre?
        auto obj_type = candidate->ObjectType();
        if (obj_type == UniverseObjectType::OBJ_PLANET) {
            auto* res = static_cast<const ::Planet*>(candidate);
            return res->Focus();
        }
        else if (obj_type == UniverseObjectType::OBJ_BUILDING) {
            // is it a building on a planet?
            auto* building = static_cast<const ::Building*>(candidate);
            if (auto planet = objects.get<Planet>(building->PlanetID()))
                return planet->Focus();
        }
        return EMPTY_STRING;
    }

    struct FocusTypeSimpleMatch {
        FocusTypeSimpleMatch(const std::vector<std::string>& names, const ObjectMap& objects) :
            m_names(names),
            m_objects(objects)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            auto& focus_name{GetCandidateFocus(candidate.get(), m_objects)};
            return !focus_name.empty() && (m_names.empty() || std::count(m_names.begin(), m_names.end(), focus_name));

            return false;
        }

        const std::vector<std::string>& m_names;
        const ObjectMap& m_objects;
    };
}

void FocusType::Eval(const ScriptingContext& parent_context,
                     ObjectSet& matches, ObjectSet& non_matches,
                     SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = parent_context.condition_root_candidate || RootCandidateInvariant();
    if (simple_eval_safe) {
        // check each valueref for invariance to local candidate
        for (auto& name : m_names) {
            if (!name->LocalCandidateInvariant()) {
                simple_eval_safe = false;
                break;
            }
        }
    }
    if (simple_eval_safe) {
        // evaluate names once, and use to check all candidate objects
        std::vector<std::string> names;
        names.reserve(m_names.size());
        // get all names from valuerefs TODO: could lazy evaluate names rather than evaluating all and passing...
        for (auto& name : m_names)
            names.push_back(name->Eval(parent_context));
        EvalImpl(matches, non_matches, search_domain, FocusTypeSimpleMatch(names, parent_context.ContextObjects()));
    } else {
        // re-evaluate allowed building types range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string FocusType::Description(bool negated/* = false*/) const {
    std::string values_str;
    for (unsigned int i = 0; i < m_names.size(); ++i) {
        values_str += m_names[i]->ConstantExpr() ?
            UserString(m_names[i]->Eval()) :
            m_names[i]->Description();
        if (2 <= m_names.size() && i < m_names.size() - 2) {
            values_str += ", ";
        } else if (i == m_names.size() - 2) {
            values_str += m_names.size() < 3 ? " " : ", ";
            values_str += UserString("OR");
            values_str += " ";
        }
    }
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_FOCUS_TYPE")
        : UserString("DESC_FOCUS_TYPE_NOT"))
        % values_str);
}

std::string FocusType::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "Focus name = ";
    if (m_names.size() == 1) {
        retval += m_names[0]->Dump(ntabs) + "\n";
    } else {
        retval += "[ ";
        for (auto& name : m_names) {
            retval += name->Dump(ntabs) + " ";
        }
        retval += "]\n";
    }
    return retval;
}

bool FocusType::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "FocusType::Match passed no candidate object";
        return false;
    }

    auto& focus_name{GetCandidateFocus(candidate.get(), local_context.ContextObjects())};

    if (m_names.empty())
        return !focus_name.empty();

    for (auto& name : m_names) {
        if (name->Eval(local_context) == focus_name)
            return true;
    }

    return false;
}

void FocusType::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                  ObjectSet& condition_non_targets) const
{
    AddPlanetSet(parent_context.ContextObjects(), condition_non_targets);
    AddBuildingSet(parent_context.ContextObjects(), condition_non_targets);
}

void FocusType::SetTopLevelContent(const std::string& content_name) {
    for (auto& name : m_names) {
        if (name)
            name->SetTopLevelContent(content_name);
    }
}

unsigned int FocusType::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::FocusType");
    CheckSums::CheckSumCombine(retval, m_names);

    TraceLogger() << "GetCheckSum(FocusType): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> FocusType::Clone() const
{ return std::make_unique<FocusType>(ValueRef::CloneUnique(m_names)); }

///////////////////////////////////////////////////////////
// StarType                                              //
///////////////////////////////////////////////////////////
StarType::StarType(std::vector<std::unique_ptr<ValueRef::ValueRef< ::StarType>>>&& types) :
    Condition(),
    m_types(std::move(types))
{
    m_root_candidate_invariant = boost::algorithm::all_of(m_types, [](auto& e){ return e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(m_types, [](auto& e){ return e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(m_types, [](auto& e){ return e->SourceInvariant(); });
}

bool StarType::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const StarType& rhs_ = static_cast<const StarType&>(rhs);

    if (m_types.size() != rhs_.m_types.size())
        return false;
    for (unsigned int i = 0; i < m_types.size(); ++i) {
        CHECK_COND_VREF_MEMBER(m_types.at(i))
    }

    return true;
}

namespace {
    struct StarTypeSimpleMatch {
        StarTypeSimpleMatch(const std::vector< ::StarType>& types, const ObjectMap& objects) :
            m_types(types),
            m_objects(objects)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate || m_types.empty())
                return false;

            if (candidate->ObjectType() == UniverseObjectType::OBJ_SYSTEM) {
                auto system = static_cast<const System*>(candidate.get());
                return std::count(m_types.begin(), m_types.end(), system->GetStarType());
            } else if (auto system = m_objects.get<System>(candidate->SystemID()).get()) {
                return std::count(m_types.begin(), m_types.end(), system->GetStarType());
            } else {
                return false;
            }
        }

        const std::vector< ::StarType>& m_types;
        const ObjectMap& m_objects;
    };
}

void StarType::Eval(const ScriptingContext& parent_context,
                    ObjectSet& matches, ObjectSet& non_matches,
                    SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = parent_context.condition_root_candidate || RootCandidateInvariant();
    if (simple_eval_safe) {
        // check each valueref for invariance to local candidate
        for (auto& type : m_types) {
            if (!type->LocalCandidateInvariant()) {
                simple_eval_safe = false;
                break;
            }
        }
    }
    if (simple_eval_safe) {
        // evaluate types once, and use to check all candidate objects
        std::vector< ::StarType> types;
        types.reserve(m_types.size());
        // get all types from valuerefs
        for (auto& type : m_types)
            types.push_back(type->Eval(parent_context));
        EvalImpl(matches, non_matches, search_domain, StarTypeSimpleMatch(types, parent_context.ContextObjects()));
    } else {
        // re-evaluate contained objects for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string StarType::Description(bool negated/* = false*/) const {
    std::string values_str;
    for (unsigned int i = 0; i < m_types.size(); ++i) {
        values_str += m_types[i]->ConstantExpr() ?
                        UserString(boost::lexical_cast<std::string>(m_types[i]->Eval())) :
                        m_types[i]->Description();
        if (2 <= m_types.size() && i < m_types.size() - 2) {
            values_str += ", ";
        } else if (i == m_types.size() - 2) {
            values_str += m_types.size() < 3 ? " " : ", ";
            values_str += UserString("OR");
            values_str += " ";
        }
    }
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_STAR_TYPE")
        : UserString("DESC_STAR_TYPE_NOT"))
        % values_str);
}

std::string StarType::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "Star type = ";
    if (m_types.size() == 1) {
        retval += m_types[0]->Dump(ntabs) + "\n";
    } else {
        retval += "[ ";
        for (auto& type : m_types) {
            retval += type->Dump(ntabs) + " ";
        }
        retval += "]\n";
    }
    return retval;
}

bool StarType::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "StarType::Match passed no candidate object";
        return false;
    }
    if (m_types.empty())
        return false;

    ::StarType star_type = ::StarType::INVALID_STAR_TYPE;
    if (candidate->ObjectType() == UniverseObjectType::OBJ_SYSTEM) {
        auto system = static_cast<const System*>(candidate.get());
        star_type = system->GetStarType();
    } else if (auto system = local_context.ContextObjects().get<System>(candidate->SystemID()).get()) {
        star_type = system->GetStarType();
    } else {
        return false;
    }

    for (auto& type : m_types) {
        if (type->Eval(local_context) == star_type)
            return true;
    }
    return false;
}

void StarType::SetTopLevelContent(const std::string& content_name) {
    for (auto& type : m_types) {
        if (type)
            (type)->SetTopLevelContent(content_name);
    }
}

unsigned int StarType::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::StarType");
    CheckSums::CheckSumCombine(retval, m_types);

    TraceLogger() << "GetCheckSum(StarType): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> StarType::Clone() const
{ return std::make_unique<StarType>(ValueRef::CloneUnique(m_types)); }

///////////////////////////////////////////////////////////
// DesignHasHull                                         //
///////////////////////////////////////////////////////////
DesignHasHull::DesignHasHull(std::unique_ptr<ValueRef::ValueRef<std::string>>&& name) :
    Condition(),
    m_name(std::move(name))
{
    m_root_candidate_invariant = !m_name || m_name->RootCandidateInvariant();
    m_target_invariant = !m_name || m_name->TargetInvariant();
    m_source_invariant = !m_name || m_name->SourceInvariant();
}

bool DesignHasHull::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const DesignHasHull& rhs_ = static_cast<const DesignHasHull&>(rhs);

    CHECK_COND_VREF_MEMBER(m_name);

    return true;
}

namespace {
    struct DesignHasHullSimpleMatch {
        DesignHasHullSimpleMatch(const std::string& name, const Universe& universe) :
            m_name(name),
            m_universe(universe)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            // is it a ship?
            if (candidate->ObjectType() != UniverseObjectType::OBJ_SHIP)
                return false;
            auto* ship = static_cast<const ::Ship*>(candidate.get());

            // with a valid design?
            const ShipDesign* design = m_universe.GetShipDesign(ship->DesignID());
            if (!design)
                return false;

            return design->Hull() == m_name;
        }

        const std::string& m_name;
        const Universe& m_universe;
    };
}

void DesignHasHull::Eval(const ScriptingContext& parent_context,
                         ObjectSet& matches, ObjectSet& non_matches,
                         SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = (!m_name || m_name->LocalCandidateInvariant()) &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant());
    if (simple_eval_safe) {
        // evaluate number limits once, use to match all candidates
        std::string name = (m_name ? m_name->Eval(parent_context) : "");

        // need to test each candidate separately using EvalImpl and because the
        // design of the candidate object is tested
        EvalImpl(matches, non_matches, search_domain, DesignHasHullSimpleMatch(name, parent_context.ContextUniverse()));
    } else {
        // re-evaluate allowed turn range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string DesignHasHull::Description(bool negated/* = false*/) const {
    std::string name_str;
    if (m_name) {
        name_str = m_name->Description();
        if (m_name->ConstantExpr() && UserStringExists(name_str))
            name_str = UserString(name_str);
    }
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_DESIGN_HAS_HULL")
        : UserString("DESC_DESIGN_HAS_HULL_NOT"))
        % name_str);
}

std::string DesignHasHull::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "DesignHasHull";
    if (m_name)
        retval += " name = " + m_name->Dump(ntabs);
    retval += "\n";
    return retval;
}

bool DesignHasHull::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "DesignHasHull::Match passed no candidate object";
        return false;
    }

    std::string name = (m_name ? m_name->Eval(local_context) : "");

    return DesignHasHullSimpleMatch(name, local_context.ContextUniverse())(candidate);
}

void DesignHasHull::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                      ObjectSet& condition_non_targets) const
{
    AddShipSet(parent_context.ContextObjects(), condition_non_targets);
}

void DesignHasHull::SetTopLevelContent(const std::string& content_name) {
    if (m_name)
        m_name->SetTopLevelContent(content_name);
}

unsigned int DesignHasHull::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::DesignHasHull");
    CheckSums::CheckSumCombine(retval, m_name);

    TraceLogger() << "GetCheckSum(DesignHasHull): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> DesignHasHull::Clone() const
{ return std::make_unique<DesignHasHull>(ValueRef::CloneUnique(m_name)); }

///////////////////////////////////////////////////////////
// DesignHasPart                                         //
///////////////////////////////////////////////////////////
DesignHasPart::DesignHasPart(std::unique_ptr<ValueRef::ValueRef<std::string>>&& name,
                             std::unique_ptr<ValueRef::ValueRef<int>>&& low,
                             std::unique_ptr<ValueRef::ValueRef<int>>&& high) :
    Condition(),
    m_low(std::move(low)),
    m_high(std::move(high)),
    m_name(std::move(name))
{
    std::array<const ValueRef::ValueRefBase*, 3> operands = {{m_name.get(), m_low.get(), m_high.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

bool DesignHasPart::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const DesignHasPart& rhs_ = static_cast<const DesignHasPart&>(rhs);

    CHECK_COND_VREF_MEMBER(m_name);
    CHECK_COND_VREF_MEMBER(m_low)
    CHECK_COND_VREF_MEMBER(m_high)

    return true;
}

namespace {
    struct DesignHasPartSimpleMatch {
        DesignHasPartSimpleMatch(int low, int high, const std::string& name, const Universe& universe) :
            m_low(low),
            m_high(high),
            m_name(name),
            m_universe(universe)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            const Ship* ship = nullptr;
            if (candidate->ObjectType() == UniverseObjectType::OBJ_FIGHTER) {
                auto* fighter = static_cast<const ::Fighter*>(candidate.get());
                ship = m_universe.Objects().get<Ship>(fighter->LaunchedFrom()).get();
            } else if (candidate->ObjectType() == UniverseObjectType::OBJ_SHIP) {
                ship = static_cast<const ::Ship*>(candidate.get());
            }
            if (!ship)
                return false;

            // with a valid design?
            const ShipDesign* design = m_universe.GetShipDesign(ship->DesignID());
            if (!design)
                return false;

            int count = 0;
            for (const std::string& name : design->Parts()) {
                if (name == m_name || (m_name.empty() && !name.empty()))
                    // number of copies of specified part,
                    // or total number of parts if no part name specified
                    ++count;
            }
            return (m_low <= count && count <= m_high);
        }

        int                 m_low;
        int                 m_high;
        const std::string&  m_name;
        const Universe&     m_universe;
    };
}

void DesignHasPart::Eval(const ScriptingContext& parent_context,
                         ObjectSet& matches, ObjectSet& non_matches,
                         SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = (!m_low || m_low->LocalCandidateInvariant()) &&
                            (!m_high || m_high->LocalCandidateInvariant()) &&
                            (!m_name || m_name->LocalCandidateInvariant()) &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant());
    if (simple_eval_safe) {
        // evaluate number limits once, use to match all candidates
        std::string name = (m_name ? m_name->Eval(parent_context) : "");
        int low =          (m_low ? std::max(0, m_low->Eval(parent_context)) : 1);
        int high =         (m_high ? std::min(m_high->Eval(parent_context), INT_MAX) : INT_MAX);

        // need to test each candidate separately using EvalImpl and because the
        // design of the candidate object is tested
        EvalImpl(matches, non_matches, search_domain, DesignHasPartSimpleMatch(low, high, name, parent_context.ContextUniverse()));
    } else {
        // re-evaluate allowed turn range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string DesignHasPart::Description(bool negated/* = false*/) const {
    std::string low_str = "1";
    if (m_low) {
        low_str = m_low->ConstantExpr() ?
                    std::to_string(m_low->Eval()) :
                    m_low->Description();
    }
    std::string high_str = std::to_string(INT_MAX);
    if (m_high) {
        high_str = m_high->ConstantExpr() ?
                    std::to_string(m_high->Eval()) :
                    m_high->Description();
    };
    std::string name_str;
    if (m_name) {
        name_str = m_name->Description();
        if (m_name->ConstantExpr() && UserStringExists(name_str))
            name_str = UserString(name_str);
    }
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_DESIGN_HAS_PART")
        : UserString("DESC_DESIGN_HAS_PART_NOT"))
        % low_str
        % high_str
        % name_str);
}

std::string DesignHasPart::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "DesignHasPart";
    if (m_low)
        retval += "low = " + m_low->Dump(ntabs);
    if (m_high)
        retval += " high = " + m_high->Dump(ntabs);
    if (m_name)
        retval += " name = " + m_name->Dump(ntabs);
    retval += "\n";
    return retval;
}

bool DesignHasPart::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "DesignHasPart::Match passed no candidate object";
        return false;
    }

    int low =  (m_low ? std::max(0, m_low->Eval(local_context)) : 0);
    int high = (m_high ? std::min(m_high->Eval(local_context), IMPOSSIBLY_LARGE_TURN) : IMPOSSIBLY_LARGE_TURN);
    std::string name = (m_name ? m_name->Eval(local_context) : "");

    return DesignHasPartSimpleMatch(low, high, name, local_context.ContextUniverse())(candidate);
}

void DesignHasPart::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                      ObjectSet& condition_non_targets) const
{
    AddShipSet(parent_context.ContextObjects(), condition_non_targets);
}

void DesignHasPart::SetTopLevelContent(const std::string& content_name) {
    if (m_low)
        m_low->SetTopLevelContent(content_name);
    if (m_high)
        m_high->SetTopLevelContent(content_name);
    if (m_name)
        m_name->SetTopLevelContent(content_name);
}

unsigned int DesignHasPart::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::DesignHasPart");
    CheckSums::CheckSumCombine(retval, m_low);
    CheckSums::CheckSumCombine(retval, m_high);
    CheckSums::CheckSumCombine(retval, m_name);

    TraceLogger() << "GetCheckSum(DesignHasPart): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> DesignHasPart::Clone() const {
    return std::make_unique<DesignHasPart>(ValueRef::CloneUnique(m_name),
                                           ValueRef::CloneUnique(m_low),
                                           ValueRef::CloneUnique(m_high));
}

///////////////////////////////////////////////////////////
// DesignHasPartClass                                    //
///////////////////////////////////////////////////////////
DesignHasPartClass::DesignHasPartClass(ShipPartClass part_class,
                                       std::unique_ptr<ValueRef::ValueRef<int>>&& low,
                                       std::unique_ptr<ValueRef::ValueRef<int>>&& high) :
    Condition(),
    m_low(std::move(low)),
    m_high(std::move(high)),
    m_class(std::move(part_class))
{
    std::array<const ValueRef::ValueRefBase*, 2> operands = {{m_low.get(), m_high.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

bool DesignHasPartClass::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const DesignHasPartClass& rhs_ = static_cast<const DesignHasPartClass&>(rhs);

    if (m_class != rhs_.m_class)
        return false;

    CHECK_COND_VREF_MEMBER(m_low)
    CHECK_COND_VREF_MEMBER(m_high)

    return true;
}

namespace {
    struct DesignHasPartClassSimpleMatch {
        DesignHasPartClassSimpleMatch(int low, int high, ShipPartClass part_class, const Universe& universe) :
            m_low(low),
            m_high(high),
            m_part_class(part_class),
            m_universe(universe)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            // is it a ship?
            if (candidate->ObjectType() != UniverseObjectType::OBJ_SHIP)
                return false;
            auto* ship = static_cast<const ::Ship*>(candidate.get());

            // with a valid design?
            const ShipDesign* design = m_universe.GetShipDesign(ship->DesignID());
            if (!design)
                return false;


            int count = 0;
            for (const std::string& name : design->Parts()) {
                if (const ShipPart* ship_part = GetShipPart(name)) {
                    if (ship_part->Class() == m_part_class)
                        ++count;
                }
            }
            return (m_low <= count && count <= m_high);
        }

        int             m_low;
        int             m_high;
        ShipPartClass   m_part_class;
        const Universe& m_universe;
    };
}

void DesignHasPartClass::Eval(const ScriptingContext& parent_context,
                              ObjectSet& matches, ObjectSet& non_matches,
                              SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = (!m_low || m_low->LocalCandidateInvariant()) &&
                            (!m_high || m_high->LocalCandidateInvariant()) &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant());
    if (simple_eval_safe) {
        // evaluate number limits once, use to match all candidates
        int low =          (m_low ? std::max(0, m_low->Eval(parent_context)) : 1);
        int high =         (m_high ? std::min(m_high->Eval(parent_context), INT_MAX) : INT_MAX);

        // need to test each candidate separately using EvalImpl and because the
        // design of the candidate object is tested
        EvalImpl(matches, non_matches, search_domain,
                 DesignHasPartClassSimpleMatch(low, high, m_class, parent_context.ContextUniverse()));
    } else {
        // re-evaluate allowed turn range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string DesignHasPartClass::Description(bool negated/* = false*/) const {
    std::string low_str = "1";
    if (m_low) {
        low_str = m_low->ConstantExpr() ?
                    std::to_string(m_low->Eval()) :
                    m_low->Description();
    }
    std::string high_str = std::to_string(INT_MAX);
    if (m_high) {
        high_str = m_high->ConstantExpr() ?
                    std::to_string(m_high->Eval()) :
                    m_high->Description();
    }
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_DESIGN_HAS_PART_CLASS")
        : UserString("DESC_DESIGN_HAS_PART_CLASS_NOT"))
               % low_str
               % high_str
               % UserString(boost::lexical_cast<std::string>(m_class)));
}

std::string DesignHasPartClass::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "DesignHasPartClass";
    if (m_low)
        retval += " low = " + m_low->Dump(ntabs);
    if (m_high)
        retval += " high = " + m_high->Dump(ntabs);
    retval += " class = " + UserString(boost::lexical_cast<std::string>(m_class));
    retval += "\n";
    return retval;
}

bool DesignHasPartClass::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "DesignHasPartClass::Match passed no candidate object";
        return false;
    }

    int low =  (m_low ? m_low->Eval(local_context) : 0);
    int high = (m_high ? m_high->Eval(local_context) : INT_MAX);

    return DesignHasPartClassSimpleMatch(low, high, m_class, local_context.ContextUniverse())(candidate);
}

void DesignHasPartClass::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                                           ObjectSet& condition_non_targets) const
{ AddShipSet(parent_context.ContextObjects(), condition_non_targets); }

void DesignHasPartClass::SetTopLevelContent(const std::string& content_name) {
    if (m_low)
        m_low->SetTopLevelContent(content_name);
    if (m_high)
        m_high->SetTopLevelContent(content_name);
}

unsigned int DesignHasPartClass::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::DesignHasPartClass");
    CheckSums::CheckSumCombine(retval, m_low);
    CheckSums::CheckSumCombine(retval, m_high);
    CheckSums::CheckSumCombine(retval, m_class);

    TraceLogger() << "GetCheckSum(DesignHasPartClass): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> DesignHasPartClass::Clone() const {
    return std::make_unique<DesignHasPartClass>(m_class,
                                                ValueRef::CloneUnique(m_low),
                                                ValueRef::CloneUnique(m_high));
}

///////////////////////////////////////////////////////////
// PredefinedShipDesign                                  //
///////////////////////////////////////////////////////////
PredefinedShipDesign::PredefinedShipDesign(std::unique_ptr<ValueRef::ValueRef<std::string>>&& name) :
    Condition(),
    m_name(std::move(name))
{
    m_root_candidate_invariant = !m_name || m_name->RootCandidateInvariant();
    m_target_invariant = !m_name || m_name->TargetInvariant();
    m_source_invariant = !m_name || m_name->SourceInvariant();
}

bool PredefinedShipDesign::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const PredefinedShipDesign& rhs_ = static_cast<const PredefinedShipDesign&>(rhs);

    CHECK_COND_VREF_MEMBER(m_name)

    return true;
}

namespace {
    struct PredefinedShipDesignSimpleMatch {
        PredefinedShipDesignSimpleMatch(const Universe& u) :
            m_any_predef_design_ok(true),
            m_name(EMPTY_STRING),
            m_u(u)
        {}

        PredefinedShipDesignSimpleMatch(const std::string& name, const Universe& u) :
            m_any_predef_design_ok(false),
            m_name(name),
            m_u(u)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (candidate->ObjectType() != UniverseObjectType::OBJ_SHIP)
                return false;
            auto* ship = static_cast<const Ship*>(candidate.get());

            const ShipDesign* candidate_design = m_u.GetShipDesign(ship->DesignID());
            if (!candidate_design)
                return false;

            // ship has a valid design.  see if it is / could be a predefined ship design...

            // all predefined named designs are hard-coded in parsing to have a designed on turn 0 (before first turn)
            if (candidate_design->DesignedOnTurn() != 0)
                return false;

            if (m_any_predef_design_ok)
                return true;    // any predefined design is OK; don't need to check name.

            return (m_name == candidate_design->Name(false)); // don't look up in stringtable; predefined designs are stored by stringtable entry key
        }

        bool               m_any_predef_design_ok;
        const std::string& m_name;
        const Universe&    m_u;
    };
}

void PredefinedShipDesign::Eval(const ScriptingContext& parent_context,
                                ObjectSet& matches, ObjectSet& non_matches,
                                SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = (!m_name || m_name->LocalCandidateInvariant()) &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant());
    if (simple_eval_safe) {
        // testing each candidate to see if its design is predefined or is a
        // particular named predefined design
        if (!m_name) {
            EvalImpl(matches, non_matches, search_domain,
                     PredefinedShipDesignSimpleMatch(parent_context.ContextUniverse()));
        } else {
            std::string name = m_name->Eval(parent_context);
            EvalImpl(matches, non_matches, search_domain,
                     PredefinedShipDesignSimpleMatch(name, parent_context.ContextUniverse()));
        }
    } else {
        // re-evaluate allowed turn range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string PredefinedShipDesign::Description(bool negated/* = false*/) const {
    std::string name_str;
    if (m_name) {
        name_str = m_name->Description();
        if (m_name->ConstantExpr() && UserStringExists(name_str))
            name_str = UserString(name_str);
    }
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_PREDEFINED_SHIP_DESIGN")
        : UserString("DESC_PREDEFINED_SHIP_DESIGN_NOT"))
        % name_str);
}

std::string PredefinedShipDesign::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "PredefinedShipDesign";
    if (m_name)
        retval += " name = " + m_name->Dump(ntabs);
    retval += "\n";
    return retval;
}

bool PredefinedShipDesign::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "PredefinedShipDesign::Match passed no candidate object";
        return false;
    }

    if (!m_name)
        return PredefinedShipDesignSimpleMatch(local_context.ContextUniverse())(candidate);

    std::string name = m_name->Eval(local_context);
    return PredefinedShipDesignSimpleMatch(name, local_context.ContextUniverse())(candidate);
}

void PredefinedShipDesign::SetTopLevelContent(const std::string& content_name) {
    if (m_name)
        m_name->SetTopLevelContent(content_name);
}

unsigned int PredefinedShipDesign::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::PredefinedShipDesign");
    CheckSums::CheckSumCombine(retval, m_name);

    TraceLogger() << "GetCheckSum(PredefinedShipDesign): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> PredefinedShipDesign::Clone() const
{ return std::make_unique<PredefinedShipDesign>(ValueRef::CloneUnique(m_name)); }

///////////////////////////////////////////////////////////
// NumberedShipDesign                                    //
///////////////////////////////////////////////////////////
NumberedShipDesign::NumberedShipDesign(std::unique_ptr<ValueRef::ValueRef<int>>&& design_id) :
    Condition(),
    m_design_id(std::move(design_id))
{
    m_root_candidate_invariant = !m_design_id || m_design_id->RootCandidateInvariant();
    m_target_invariant = !m_design_id || m_design_id->TargetInvariant();
    m_source_invariant = !m_design_id || m_design_id->SourceInvariant();
}

bool NumberedShipDesign::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const NumberedShipDesign& rhs_ = static_cast<const NumberedShipDesign&>(rhs);

    CHECK_COND_VREF_MEMBER(m_design_id)

    return true;
}

namespace {
    struct NumberedShipDesignSimpleMatch {
        NumberedShipDesignSimpleMatch(int design_id) :
            m_design_id(design_id)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;
            if (m_design_id == INVALID_DESIGN_ID)
                return false;
            if (auto ship = dynamic_cast<const Ship*>(candidate.get()))
                if (ship->DesignID() == m_design_id)
                    return true;
            return false;
        }

        int m_design_id;
    };
}

void NumberedShipDesign::Eval(const ScriptingContext& parent_context,
                              ObjectSet& matches, ObjectSet& non_matches,
                              SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = m_design_id->ConstantExpr() ||
                            (m_design_id->LocalCandidateInvariant() &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // evaluate design id once, and use to check all candidate objects
        int design_id = m_design_id->Eval(parent_context);

        // design of the candidate objects is tested, so need to check each separately
        EvalImpl(matches, non_matches, search_domain, NumberedShipDesignSimpleMatch(design_id));
    } else {
        // re-evaluate design id for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string NumberedShipDesign::Description(bool negated/* = false*/) const {
    std::string id_str = m_design_id->ConstantExpr() ?
                            std::to_string(m_design_id->Eval()) :
                            m_design_id->Description();

    return str(FlexibleFormat((!negated)
        ? UserString("DESC_NUMBERED_SHIP_DESIGN")
        : UserString("DESC_NUMBERED_SHIP_DESIGN_NOT"))
               % id_str);
}

std::string NumberedShipDesign::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "NumberedShipDesign design_id = " + m_design_id->Dump(ntabs); }

bool NumberedShipDesign::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "NumberedShipDesign::Match passed no candidate object";
        return false;
    }
    return NumberedShipDesignSimpleMatch(m_design_id->Eval(local_context))(candidate);
}

void NumberedShipDesign::SetTopLevelContent(const std::string& content_name) {
    if (m_design_id)
        m_design_id->SetTopLevelContent(content_name);
}

unsigned int NumberedShipDesign::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::NumberedShipDesign");
    CheckSums::CheckSumCombine(retval, m_design_id);

    TraceLogger() << "GetCheckSum(NumberedShipDesign): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> NumberedShipDesign::Clone() const
{ return std::make_unique<NumberedShipDesign>(ValueRef::CloneUnique(m_design_id)); }

///////////////////////////////////////////////////////////
// ProducedByEmpire                                      //
///////////////////////////////////////////////////////////
ProducedByEmpire::ProducedByEmpire(std::unique_ptr<ValueRef::ValueRef<int>>&& empire_id) :
    Condition(),
    m_empire_id(std::move(empire_id))
{
    m_root_candidate_invariant = !m_empire_id || m_empire_id->RootCandidateInvariant();
    m_target_invariant = !m_empire_id || m_empire_id->TargetInvariant();
    m_source_invariant = !m_empire_id || m_empire_id->SourceInvariant();
}

bool ProducedByEmpire::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const ProducedByEmpire& rhs_ = static_cast<const ProducedByEmpire&>(rhs);

    CHECK_COND_VREF_MEMBER(m_empire_id)

    return true;
}

namespace {
    struct ProducedByEmpireSimpleMatch {
        ProducedByEmpireSimpleMatch(int empire_id) :
            m_empire_id(empire_id)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            if (auto ship = dynamic_cast<const ::Ship*>(candidate.get()))
                return ship->ProducedByEmpireID() == m_empire_id;
            else if (auto building = dynamic_cast<const ::Building*>(candidate.get()))
                return building->ProducedByEmpireID() == m_empire_id;
            return false;
        }

        int m_empire_id;
    };
}

void ProducedByEmpire::Eval(const ScriptingContext& parent_context,
                            ObjectSet& matches, ObjectSet& non_matches,
                            SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = m_empire_id->ConstantExpr() ||
                            (m_empire_id->LocalCandidateInvariant() &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // evaluate empire id once, and use to check all candidate objects
        int empire_id = m_empire_id->Eval(parent_context);
        EvalImpl(matches, non_matches, search_domain, ProducedByEmpireSimpleMatch(empire_id));
    } else {
        // re-evaluate empire id for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string ProducedByEmpire::Description(bool negated/* = false*/) const {
    std::string empire_str;
    if (m_empire_id) {
        int empire_id = ALL_EMPIRES;
        if (m_empire_id->ConstantExpr())
            empire_id = m_empire_id->Eval();
        if (const Empire* empire = GetEmpire(empire_id))
            empire_str = empire->Name();
        else
            empire_str = m_empire_id->Description();
    }

    return str(FlexibleFormat((!negated)
        ? UserString("DESC_PRODUCED_BY_EMPIRE")
        : UserString("DESC_PRODUCED_BY_EMPIRE_NOT"))
               % empire_str);
}

std::string ProducedByEmpire::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "ProducedByEmpire empire = " + m_empire_id->Dump(ntabs); }

bool ProducedByEmpire::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "ProducedByEmpire::Match passed no candidate object";
        return false;
    }

    return ProducedByEmpireSimpleMatch(m_empire_id->Eval(local_context))(candidate);
}

void ProducedByEmpire::SetTopLevelContent(const std::string& content_name) {
    if (m_empire_id)
        m_empire_id->SetTopLevelContent(content_name);
}

unsigned int ProducedByEmpire::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::ProducedByEmpire");
    CheckSums::CheckSumCombine(retval, m_empire_id);

    TraceLogger() << "GetCheckSum(ProducedByEmpire): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> ProducedByEmpire::Clone() const
{ return std::make_unique<ProducedByEmpire>(ValueRef::CloneUnique(m_empire_id)); }

///////////////////////////////////////////////////////////
// Chance                                                //
///////////////////////////////////////////////////////////
Chance::Chance(std::unique_ptr<ValueRef::ValueRef<double>>&& chance) :
    Condition(),
    m_chance(std::move(chance))
{
    m_root_candidate_invariant = !m_chance || m_chance->RootCandidateInvariant();
    m_target_invariant = !m_chance || m_chance->TargetInvariant();
    m_source_invariant = !m_chance || m_chance->SourceInvariant();
}

bool Chance::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const Chance& rhs_ = static_cast<const Chance&>(rhs);

    CHECK_COND_VREF_MEMBER(m_chance)

    return true;
}

namespace {
    struct ChanceSimpleMatch {
        ChanceSimpleMatch(float chance) :
            m_chance(chance)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const
        { return RandZeroToOne() <= m_chance; }

        float m_chance;
    };
}

void Chance::Eval(const ScriptingContext& parent_context,
                  ObjectSet& matches, ObjectSet& non_matches,
                  SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = m_chance->ConstantExpr() ||
                            (m_chance->LocalCandidateInvariant() &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // evaluate empire id once, and use to check all candidate objects
        float chance = std::max(0.0, std::min(1.0, m_chance->Eval(parent_context)));
        // chance is tested independently for each candidate object
        EvalImpl(matches, non_matches, search_domain, ChanceSimpleMatch(chance));
    } else {
        // re-evaluate empire id for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string Chance::Description(bool negated/* = false*/) const {
    if (m_chance->ConstantExpr()) {
        return str(FlexibleFormat((!negated)
            ? UserString("DESC_CHANCE_PERCENTAGE")
            : UserString("DESC_CHANCE_PERCENTAGE_NOT"))
                % std::to_string(std::max(0.0, std::min(m_chance->Eval(), 1.0)) * 100));
    } else {
        return str(FlexibleFormat((!negated)
            ? UserString("DESC_CHANCE")
            : UserString("DESC_CHANCE_NOT"))
            % m_chance->Description());
    }
}

std::string Chance::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "Random probability = " + m_chance->Dump(ntabs) + "\n"; }

bool Chance::Match(const ScriptingContext& local_context) const {
    float chance = std::max(0.0, std::min(m_chance->Eval(local_context), 1.0));
    return RandZeroToOne() <= chance;
}

void Chance::SetTopLevelContent(const std::string& content_name) {
    if (m_chance)
        m_chance->SetTopLevelContent(content_name);
}

unsigned int Chance::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Chance");
    CheckSums::CheckSumCombine(retval, m_chance);

    TraceLogger() << "GetCheckSum(Chance): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Chance::Clone() const
{ return std::make_unique<Chance>(ValueRef::CloneUnique(m_chance)); }

///////////////////////////////////////////////////////////
// MeterValue                                            //
///////////////////////////////////////////////////////////
MeterValue::MeterValue(MeterType meter,
                       std::unique_ptr<ValueRef::ValueRef<double>>&& low,
                       std::unique_ptr<ValueRef::ValueRef<double>>&& high) :
    Condition(),
    m_meter(meter),
    m_low(std::move(low)),
    m_high(std::move(high))
{
    std::array<const ValueRef::ValueRefBase*, 2> operands = {{m_low.get(), m_high.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

bool MeterValue::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const MeterValue& rhs_ = static_cast<const MeterValue&>(rhs);

    if (m_meter != rhs_.m_meter)
        return false;

    CHECK_COND_VREF_MEMBER(m_low)
    CHECK_COND_VREF_MEMBER(m_high)

    return true;
}

namespace {
    struct MeterValueSimpleMatch {
        MeterValueSimpleMatch(float low, float high, MeterType meter_type) :
            m_low(low),
            m_high(high),
            m_meter_type(meter_type)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            if (const Meter* meter = candidate->GetMeter(m_meter_type)) {
                float value = meter->Initial();    // match Initial rather than Current to make results reproducible in a given turn, until back propagation happens
                return m_low <= value && value <= m_high;
            }

            return false;
        }

        float m_low;
        float m_high;
        MeterType m_meter_type;
    };

    std::string MeterTypeDumpString(MeterType meter) {
        switch (meter) {
        case MeterType::INVALID_METER_TYPE:        return "INVALID_METER_TYPE"; break;
        case MeterType::METER_TARGET_POPULATION:   return "TargetPopulation";   break;
        case MeterType::METER_TARGET_INDUSTRY:     return "TargetIndustry";     break;
        case MeterType::METER_TARGET_RESEARCH:     return "TargetResearch";     break;
        case MeterType::METER_TARGET_INFLUENCE:    return "TargetInfluence";    break;
        case MeterType::METER_TARGET_CONSTRUCTION: return "TargetConstruction"; break;
        case MeterType::METER_TARGET_HAPPINESS:    return "TargetHappiness";    break;
        case MeterType::METER_MAX_CAPACITY:        return "MaxCapacity";        break;
        case MeterType::METER_MAX_SECONDARY_STAT:  return "MaxSecondaryStat";   break;
        case MeterType::METER_MAX_FUEL:            return "MaxFuel";            break;
        case MeterType::METER_MAX_SHIELD:          return "MaxShield";          break;
        case MeterType::METER_MAX_STRUCTURE:       return "MaxStructure";       break;
        case MeterType::METER_MAX_DEFENSE:         return "MaxDefense";         break;
        case MeterType::METER_MAX_SUPPLY:          return "MaxSupply";          break;
        case MeterType::METER_MAX_STOCKPILE:       return "MaxStockpile";       break;
        case MeterType::METER_MAX_TROOPS:          return "MaxTroops";          break;
        case MeterType::METER_POPULATION:          return "Population";         break;
        case MeterType::METER_INDUSTRY:            return "Industry";           break;
        case MeterType::METER_RESEARCH:            return "Research";           break;
        case MeterType::METER_INFLUENCE:           return "Influence";          break;
        case MeterType::METER_CONSTRUCTION:        return "Construction";       break;
        case MeterType::METER_HAPPINESS:           return "Happiness";          break;
        case MeterType::METER_CAPACITY:            return "Capacity";           break;
        case MeterType::METER_SECONDARY_STAT:      return "SecondaryStat";      break;
        case MeterType::METER_FUEL:                return "Fuel";               break;
        case MeterType::METER_SHIELD:              return "Shield";             break;
        case MeterType::METER_STRUCTURE:           return "Structure";          break;
        case MeterType::METER_DEFENSE:             return "Defense";            break;
        case MeterType::METER_SUPPLY:              return "Supply";             break;
        case MeterType::METER_STOCKPILE:           return "Stockpile";          break;
        case MeterType::METER_TROOPS:              return "Troops";             break;
        case MeterType::METER_REBEL_TROOPS:        return "RebelTroops";        break;
        case MeterType::METER_SIZE:                return "Size";               break;
        case MeterType::METER_STEALTH:             return "Stealth";            break;
        case MeterType::METER_DETECTION:           return "Detection";          break;
        case MeterType::METER_SPEED:               return "Speed";              break;
        default:                                   return "?Meter?";            break;
        }
    }
}

void MeterValue::Eval(const ScriptingContext& parent_context,
                      ObjectSet& matches, ObjectSet& non_matches,
                      SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = ((!m_low || m_low->LocalCandidateInvariant()) &&
                             (!m_high || m_high->LocalCandidateInvariant()) &&
                             (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // evaluate number limits once, use to match all candidates
        float low = (m_low ? m_low->Eval(parent_context) : -Meter::LARGE_VALUE);
        float high = (m_high ? m_high->Eval(parent_context) : Meter::LARGE_VALUE);
        EvalImpl(matches, non_matches, search_domain, MeterValueSimpleMatch(low, high, m_meter));
    } else {
        // re-evaluate allowed turn range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string MeterValue::Description(bool negated/* = false*/) const {
    std::string low_str = (m_low ? (m_low->ConstantExpr() ?
                                    std::to_string(m_low->Eval()) :
                                    m_low->Description())
                                 : std::to_string(-Meter::LARGE_VALUE));
    std::string high_str = (m_high ? (m_high->ConstantExpr() ?
                                      std::to_string(m_high->Eval()) :
                                      m_high->Description())
                                   : std::to_string(Meter::LARGE_VALUE));

    if (m_low && !m_high) {
        return str(FlexibleFormat((!negated) ?
                                    UserString("DESC_METER_VALUE_CURRENT_MIN") :
                                    UserString("DESC_METER_VALUE_CURRENT_MIN_NOT"))
            % UserString(boost::lexical_cast<std::string>(m_meter))
            % low_str);
    } else if (m_high && !m_low) {
        return str(FlexibleFormat((!negated) ?
                                    UserString("DESC_METER_VALUE_CURRENT_MAX") :
                                    UserString("DESC_METER_VALUE_CURRENT_MAX_NOT"))
            % UserString(boost::lexical_cast<std::string>(m_meter))
            % high_str);
    } else {
        return str(FlexibleFormat((!negated) ?
                                    UserString("DESC_METER_VALUE_CURRENT") :
                                    UserString("DESC_METER_VALUE_CURRENT_NOT"))
            % UserString(boost::lexical_cast<std::string>(m_meter))
            % low_str
            % high_str);
    }
}

std::string MeterValue::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs);
    retval += MeterTypeDumpString(m_meter);
    if (m_low)
        retval += " low = " + m_low->Dump(ntabs);
    if (m_high)
        retval += " high = " + m_high->Dump(ntabs);
    retval += "\n";
    return retval;
}

bool MeterValue::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "MeterValue::Match passed no candidate object";
        return false;
    }
    float low = (m_low ? m_low->Eval(local_context) : -Meter::LARGE_VALUE);
    float high = (m_high ? m_high->Eval(local_context) : Meter::LARGE_VALUE);
    return MeterValueSimpleMatch(low, high, m_meter)(candidate);
}

void MeterValue::SetTopLevelContent(const std::string& content_name) {
    if (m_low)
        m_low->SetTopLevelContent(content_name);
    if (m_high)
        m_high->SetTopLevelContent(content_name);
}

unsigned int MeterValue::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::MeterValue");
    CheckSums::CheckSumCombine(retval, m_meter);
    CheckSums::CheckSumCombine(retval, m_low);
    CheckSums::CheckSumCombine(retval, m_high);

    TraceLogger() << "GetCheckSum(MeterValue): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> MeterValue::Clone() const {
    return std::make_unique<MeterValue>(m_meter,
                                        ValueRef::CloneUnique(m_low),
                                        ValueRef::CloneUnique(m_high));
}

///////////////////////////////////////////////////////////
// ShipPartMeterValue                                    //
///////////////////////////////////////////////////////////
ShipPartMeterValue::ShipPartMeterValue(std::unique_ptr<ValueRef::ValueRef<std::string>>&& ship_part_name,
                                       MeterType meter,
                                       std::unique_ptr<ValueRef::ValueRef<double>>&& low,
                                       std::unique_ptr<ValueRef::ValueRef<double>>&& high) :
    Condition(),
    m_part_name(std::move(ship_part_name)),
    m_meter(meter),
    m_low(std::move(low)),
    m_high(std::move(high))
{
    std::array<const ValueRef::ValueRefBase*, 3> operands = {{m_part_name.get(), m_low.get(), m_high.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

bool ShipPartMeterValue::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const ShipPartMeterValue& rhs_ = static_cast<const ShipPartMeterValue&>(rhs);

    if (m_meter != rhs_.m_meter)
        return false;

    CHECK_COND_VREF_MEMBER(m_part_name)
    CHECK_COND_VREF_MEMBER(m_low)
    CHECK_COND_VREF_MEMBER(m_high)

    return true;
}

namespace {
    struct ShipPartMeterValueSimpleMatch {
        ShipPartMeterValueSimpleMatch(const std::string& ship_part_name,
                                      MeterType meter, float low, float high) :
            m_part_name(ship_part_name),
            m_low(low),
            m_high(high),
            m_meter(meter)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;
            auto ship = dynamic_cast<const Ship*>(candidate.get());
            if (!ship)
                return false;
            const Meter* meter = ship->GetPartMeter(m_meter, m_part_name);
            if (!meter)
                return false;
            float meter_current = meter->Current();
            return (m_low <= meter_current && meter_current <= m_high);
        }

        const std::string& m_part_name;
        float              m_low;
        float              m_high;
        MeterType          m_meter;
    };
}

void ShipPartMeterValue::Eval(const ScriptingContext& parent_context,
                              ObjectSet& matches, ObjectSet& non_matches,
                              SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = ((!m_part_name || m_part_name->LocalCandidateInvariant()) &&
                             (!m_low || m_low->LocalCandidateInvariant()) &&
                             (!m_high || m_high->LocalCandidateInvariant()) &&
                             (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // evaluate number limits once, use to match all candidates
        float low = (m_low ? m_low->Eval(parent_context) : -Meter::LARGE_VALUE);
        float high = (m_high ? m_high->Eval(parent_context) : Meter::LARGE_VALUE);
        std::string part_name = (m_part_name ? m_part_name->Eval(parent_context) : "");
        EvalImpl(matches, non_matches, search_domain, ShipPartMeterValueSimpleMatch(part_name, m_meter, low, high));
    } else {
        // re-evaluate allowed turn range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string ShipPartMeterValue::Description(bool negated/* = false*/) const {
    std::string low_str;
    if (m_low)
        low_str = m_low->Description();
    else
        low_str = std::to_string(-Meter::LARGE_VALUE);

    std::string high_str;
    if (m_high)
        high_str = m_high->Description();
    else
        high_str = std::to_string(Meter::LARGE_VALUE);

    std::string part_str;
    if (m_part_name) {
        part_str = m_part_name->Description();
        if (m_part_name->ConstantExpr() && UserStringExists(part_str))
            part_str = UserString(part_str);
    }

    return str(FlexibleFormat((!negated)
        ? UserString("DESC_SHIP_PART_METER_VALUE_CURRENT")
        : UserString("DESC_SHIP_PART_METER_VALUE_CURRENT_NOT"))
               % UserString(boost::lexical_cast<std::string>(m_meter))
               % part_str
               % low_str
               % high_str);
}

std::string ShipPartMeterValue::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs);
    retval += MeterTypeDumpString(m_meter);
    if (m_part_name)
        retval += " part = " + m_part_name->Dump(ntabs);
    if (m_low)
        retval += " low = " + m_low->Dump(ntabs);
    if (m_high)
        retval += " high = " + m_high->Dump(ntabs);
    retval += "\n";
    return retval;
}

bool ShipPartMeterValue::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "ShipPartMeterValue::Match passed no candidate object";
        return false;
    }
    float low = (m_low ? m_low->Eval(local_context) : -Meter::LARGE_VALUE);
    float high = (m_high ? m_high->Eval(local_context) : Meter::LARGE_VALUE);
    std::string part_name = (m_part_name ? m_part_name->Eval(local_context) : "");
    return ShipPartMeterValueSimpleMatch(std::move(part_name), m_meter, low, high)(candidate);
}

void ShipPartMeterValue::SetTopLevelContent(const std::string& content_name) {
    if (m_part_name)
        m_part_name->SetTopLevelContent(content_name);
    if (m_low)
        m_low->SetTopLevelContent(content_name);
    if (m_high)
        m_high->SetTopLevelContent(content_name);
}

unsigned int ShipPartMeterValue::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::ShipPartMeterValue");
    CheckSums::CheckSumCombine(retval, m_part_name);
    CheckSums::CheckSumCombine(retval, m_meter);
    CheckSums::CheckSumCombine(retval, m_low);
    CheckSums::CheckSumCombine(retval, m_high);

    TraceLogger() << "GetCheckSum(ShipPartMeterValue): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> ShipPartMeterValue::Clone() const {
    return std::make_unique<ShipPartMeterValue>(ValueRef::CloneUnique(m_part_name),
                                                m_meter,
                                                ValueRef::CloneUnique(m_low),
                                                ValueRef::CloneUnique(m_high));
}

///////////////////////////////////////////////////////////
// EmpireMeterValue                                      //
///////////////////////////////////////////////////////////
EmpireMeterValue::EmpireMeterValue(std::string meter,
                                   std::unique_ptr<ValueRef::ValueRef<double>>&& low,
                                   std::unique_ptr<ValueRef::ValueRef<double>>&& high) :
    EmpireMeterValue(nullptr, std::move(meter), std::move(low), std::move(high))
{}

EmpireMeterValue::EmpireMeterValue(std::unique_ptr<ValueRef::ValueRef<int>>&& empire_id,
                                   std::string meter,
                                   std::unique_ptr<ValueRef::ValueRef<double>>&& low,
                                   std::unique_ptr<ValueRef::ValueRef<double>>&& high) :
    Condition(),
    m_empire_id(std::move(empire_id)),
    m_meter(std::move(meter)),
    m_low(std::move(low)),
    m_high(std::move(high))
{
    std::array<const ValueRef::ValueRefBase*, 3> operands = {{m_empire_id.get(), m_low.get(), m_high.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

bool EmpireMeterValue::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const EmpireMeterValue& rhs_ = static_cast<const EmpireMeterValue&>(rhs);

    CHECK_COND_VREF_MEMBER(m_empire_id)

    if (m_meter != rhs_.m_meter)
        return false;

    CHECK_COND_VREF_MEMBER(m_low)
    CHECK_COND_VREF_MEMBER(m_high)

    return true;
}

void EmpireMeterValue::Eval(const ScriptingContext& parent_context,
                            ObjectSet& matches, ObjectSet& non_matches,
                            SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = ((m_empire_id && m_empire_id->LocalCandidateInvariant()) &&
                             (!m_low || m_low->LocalCandidateInvariant()) &&
                             (!m_high || m_high->LocalCandidateInvariant()) &&
                             (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // If m_empire_id is specified (not null), and all parameters are
        // local-candidate-invariant, then matching for this condition doesn't
        // need to check each candidate object separately for matching, so
        // don't need to use EvalImpl and can instead do a simpler transfer
        bool match = Match(parent_context);

        // transfer objects to or from candidate set, according to whether the
        // specified empire meter was in the requested range
        if (search_domain == SearchDomain::MATCHES && !match) {
            // move all objects from matches to non_matches
            std::move(matches.begin(), matches.end(), std::back_inserter(non_matches));
            matches.clear();
        } else if (search_domain == SearchDomain::NON_MATCHES && match) {
            // move all objects from non_matches to matches
            std::move(non_matches.begin(), non_matches.end(), std::back_inserter(matches));
            non_matches.clear();
        }

    } else {
        // re-evaluate all parameters for each candidate object.
        // could optimize further by only re-evaluating the local-candidate
        // variants.
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string EmpireMeterValue::Description(bool negated/* = false*/) const {
    std::string empire_str;
    if (m_empire_id) {
        int empire_id = ALL_EMPIRES;
        if (m_empire_id->ConstantExpr())
            empire_id = m_empire_id->Eval();
        if (const Empire* empire = GetEmpire(empire_id))
            empire_str = empire->Name();
        else
            empire_str = m_empire_id->Description();
    }
    std::string low_str = (m_low ? (m_low->ConstantExpr() ?
                                    std::to_string(m_low->Eval()) :
                                    m_low->Description())
                                 : std::to_string(-Meter::LARGE_VALUE));
    std::string high_str = (m_high ? (m_high->ConstantExpr() ?
                                      std::to_string(m_high->Eval()) :
                                      m_high->Description())
                                   : std::to_string(Meter::LARGE_VALUE));
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_EMPIRE_METER_VALUE_CURRENT")
        : UserString("DESC_EMPIRE_METER_VALUE_CURRENT_NOT"))
               % UserString(m_meter)
               % low_str
               % high_str
               % empire_str);
}

std::string EmpireMeterValue::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "EmpireMeterValue";
    if (m_empire_id)
        retval += " empire = " + m_empire_id->Dump(ntabs);
    retval += " meter = " + m_meter;
    if (m_low)
        retval += " low = " + m_low->Dump(ntabs);
    if (m_high)
        retval += " high = " + m_high->Dump(ntabs);
    retval += "\n";
    return retval;
}

bool EmpireMeterValue::Match(const ScriptingContext& local_context) const {
    int empire_id = ALL_EMPIRES;
    auto& candidate = local_context.condition_local_candidate;
    // if m_empire_id not set, default to candidate object's owner
    if (!m_empire_id && !candidate) {
        ErrorLogger() << "EmpireMeterValue::Match passed no candidate object but expects one due to having no empire id valueref specified and thus wanting to use the local candidate's owner as the empire id";
        return false;

    } else if (m_empire_id && !candidate && !m_empire_id->LocalCandidateInvariant()) {
        ErrorLogger() << "EmpireMeterValue::Match passed no candidate object but but empire id valueref references the local candidate";
        return false;

    } else if (!m_empire_id && candidate) {
        // default to candidate's owner if no empire id valueref is specified
        empire_id = candidate->Owner();

    } else if (m_empire_id) {
        // either candidate exists or m_empire_id is local-candidate-invariant (or both)
        empire_id = m_empire_id->Eval(local_context);

    } else {
        ErrorLogger() << "EmpireMeterValue::Match reached unexpected default case for candidate and empire id valueref existance";
        return false;
    }

    auto empire = local_context.GetEmpire(empire_id);
    if (!empire)
        return false;
    const Meter* meter = empire->GetMeter(m_meter);
    if (!meter)
        return false;

    float meter_current = meter->Current();
    float low =  (m_low ? m_low->Eval(local_context) : -Meter::LARGE_VALUE);
    float high = (m_high ? m_high->Eval(local_context) : Meter::LARGE_VALUE);

    return (low <= meter_current && meter_current <= high);
}

void EmpireMeterValue::SetTopLevelContent(const std::string& content_name) {
    if (m_empire_id)
        m_empire_id->SetTopLevelContent(content_name);
    if (m_low)
        m_low->SetTopLevelContent(content_name);
    if (m_high)
        m_high->SetTopLevelContent(content_name);
}

unsigned int EmpireMeterValue::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::EmpireMeterValue");
    CheckSums::CheckSumCombine(retval, m_empire_id);
    CheckSums::CheckSumCombine(retval, m_meter);
    CheckSums::CheckSumCombine(retval, m_low);
    CheckSums::CheckSumCombine(retval, m_high);

    TraceLogger() << "GetCheckSum(EmpireMeterValue): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> EmpireMeterValue::Clone() const {
    return std::make_unique<EmpireMeterValue>(ValueRef::CloneUnique(m_empire_id),
                                              m_meter,
                                              ValueRef::CloneUnique(m_low),
                                              ValueRef::CloneUnique(m_high));
}

///////////////////////////////////////////////////////////
// EmpireStockpileValue                                  //
///////////////////////////////////////////////////////////
EmpireStockpileValue::EmpireStockpileValue(ResourceType stockpile,
                                           std::unique_ptr<ValueRef::ValueRef<double>>&& low,
                                           std::unique_ptr<ValueRef::ValueRef<double>>&& high) :
    EmpireStockpileValue(nullptr, stockpile, std::move(low), std::move(high))
{}

EmpireStockpileValue::EmpireStockpileValue(std::unique_ptr<ValueRef::ValueRef<int>>&& empire_id,
                                           ResourceType stockpile,
                                           std::unique_ptr<ValueRef::ValueRef<double>>&& low,
                                           std::unique_ptr<ValueRef::ValueRef<double>>&& high) :
    Condition(),
    m_empire_id(std::move(empire_id)),
    m_stockpile(stockpile),
    m_low(std::move(low)),
    m_high(std::move(high))
{
    std::array<const ValueRef::ValueRefBase*, 3> operands = {{m_empire_id.get(), m_low.get(), m_high.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

bool EmpireStockpileValue::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const EmpireStockpileValue& rhs_ = static_cast<const EmpireStockpileValue&>(rhs);

    CHECK_COND_VREF_MEMBER(m_empire_id)

    if (m_stockpile != rhs_.m_stockpile)
        return false;

    CHECK_COND_VREF_MEMBER(m_low)
    CHECK_COND_VREF_MEMBER(m_high)

    return true;
}

void EmpireStockpileValue::Eval(const ScriptingContext& parent_context,
                                ObjectSet& matches, ObjectSet& non_matches,
                                SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    // if m_empire_id not set, the local candidate's owner is used, which is not target invariant
    bool simple_eval_safe = ((m_empire_id && m_empire_id->LocalCandidateInvariant()) &&
                             (!m_low || m_low->LocalCandidateInvariant()) &&
                             (!m_high || m_high->LocalCandidateInvariant()) &&
                             (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // If m_empire_id is specified (not null), and all parameters are
        // local-candidate-invariant, then matching for this condition doesn't
        // need to check each candidate object separately for matching, so
        // don't need to use EvalImpl and can instead do a simpler transfer
        bool match = Match(parent_context);

        // transfer objects to or from candidate set, according to whether the
        // specified empire meter was in the requested range
        if (search_domain == SearchDomain::MATCHES && !match) {
            // move all objects from matches to non_matches
            std::move(matches.begin(), matches.end(), std::back_inserter(non_matches));
            matches.clear();
        } else if (search_domain == SearchDomain::NON_MATCHES && match) {
            // move all objects from non_matches to matches
            std::move(non_matches.begin(), non_matches.end(), std::back_inserter(matches));
            non_matches.clear();
        }

    } else {
        // re-evaluate all parameters for each candidate object.
        // could optimize further by only re-evaluating the local-candidate
        // variants.
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string EmpireStockpileValue::Description(bool negated/* = false*/) const {
    std::string low_str = m_low->ConstantExpr() ?
                            std::to_string(m_low->Eval()) :
                            m_low->Description();
    std::string high_str = m_high->ConstantExpr() ?
                            std::to_string(m_high->Eval()) :
                            m_high->Description();
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_EMPIRE_STOCKPILE_VALUE")
        : UserString("DESC_EMPIRE_STOCKPILE_VALUE_NOT"))
               % UserString(boost::lexical_cast<std::string>(m_stockpile))
               % low_str
               % high_str);
}

std::string EmpireStockpileValue::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs);
    switch (m_stockpile) {
    case ResourceType::RE_INFLUENCE: retval += "OwnerInfluenceStockpile";break;
    case ResourceType::RE_RESEARCH:  retval += "OwnerResearchStockpile"; break;
    case ResourceType::RE_INDUSTRY:  retval += "OwnerIndustryStockpile"; break;
    default:                         retval += "?";                      break;
    }
    if (m_empire_id)
        retval += " empire = " + m_empire_id->Dump(ntabs);
    if (m_low)
        retval += " low = " + m_low->Dump(ntabs);
    if (m_high)
        retval += " high = " + m_high->Dump(ntabs);
    retval += "\n";
    return retval;
}

bool EmpireStockpileValue::Match(const ScriptingContext& local_context) const {
    int empire_id = ALL_EMPIRES;
    auto& candidate = local_context.condition_local_candidate;
    // if m_empire_id not set, default to candidate object's owner
    if (!m_empire_id && !candidate) {
        ErrorLogger() << "EmpireStockpileValue::Match passed no candidate object but expects one due to having no empire id valueref specified and thus wanting to use the local candidate's owner as the empire id";
        return false;

    } else if (m_empire_id && !candidate && !m_empire_id->LocalCandidateInvariant()) {
        ErrorLogger() << "EmpireStockpileValue::Match passed no candidate object but but empire id valueref references the local candidate";
        return false;

    } else if (!m_empire_id && candidate) {
        // default to candidate's owner if no empire id valueref is specified
        empire_id = candidate->Owner();

    } else if (m_empire_id) {
        // either candidate exists or m_empire_id is local-candidate-invariant (or both)
        empire_id = m_empire_id->Eval(local_context);

    } else {
        ErrorLogger() << "EmpireStockpileValue::Match reached unexpected default case for candidate and empire id valueref existance";
        return false;
    }

    auto empire = local_context.GetEmpire(empire_id);
    if (!empire)
         return false;

    try {
        float low = (m_low ? m_low->Eval(local_context) : -Meter::LARGE_VALUE);
        float high = (m_high ? m_high->Eval(local_context) : Meter::LARGE_VALUE);
        float amount = empire->ResourceStockpile(m_stockpile);
        return (low <= amount && amount <= high);
    } catch (...) {
        return false;
    }
}

void EmpireStockpileValue::SetTopLevelContent(const std::string& content_name) {
    if (m_empire_id)
        m_empire_id->SetTopLevelContent(content_name);
    if (m_low)
        m_low->SetTopLevelContent(content_name);
    if (m_high)
        m_high->SetTopLevelContent(content_name);
}

unsigned int EmpireStockpileValue::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::EmpireStockpileValue");
    CheckSums::CheckSumCombine(retval, m_empire_id);
    CheckSums::CheckSumCombine(retval, m_stockpile);
    CheckSums::CheckSumCombine(retval, m_low);
    CheckSums::CheckSumCombine(retval, m_high);

    TraceLogger() << "GetCheckSum(EmpireStockpileValue): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> EmpireStockpileValue::Clone() const {
    return std::make_unique<EmpireStockpileValue>(ValueRef::CloneUnique(m_empire_id),
                                                  m_stockpile,
                                                  ValueRef::CloneUnique(m_low),
                                                  ValueRef::CloneUnique(m_high));
}

///////////////////////////////////////////////////////////
// EmpireHasAdoptedPolicy                                //
///////////////////////////////////////////////////////////
EmpireHasAdoptedPolicy::EmpireHasAdoptedPolicy(std::unique_ptr<ValueRef::ValueRef<int>>&& empire_id,
                                               std::unique_ptr<ValueRef::ValueRef<std::string>>&& name) :
    Condition(),
    m_name(std::move(name)),
    m_empire_id(std::move(empire_id))
{
    std::array<const ValueRef::ValueRefBase*, 2> operands = {{m_name.get(), m_empire_id.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

EmpireHasAdoptedPolicy::EmpireHasAdoptedPolicy(std::unique_ptr<ValueRef::ValueRef<std::string>>&& name) :
    EmpireHasAdoptedPolicy(nullptr, std::move(name))
{}

EmpireHasAdoptedPolicy::~EmpireHasAdoptedPolicy() = default;

bool EmpireHasAdoptedPolicy::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const EmpireHasAdoptedPolicy& rhs_ = static_cast<const EmpireHasAdoptedPolicy&>(rhs);

    CHECK_COND_VREF_MEMBER(m_empire_id)
    CHECK_COND_VREF_MEMBER(m_name)

    return true;
}

void EmpireHasAdoptedPolicy::Eval(const ScriptingContext& parent_context,
                                  ObjectSet& matches, ObjectSet& non_matches,
                                  SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = ((m_empire_id && m_empire_id->LocalCandidateInvariant()) &&
                             (!m_name || m_name->LocalCandidateInvariant()) &&
                             (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // If m_empire_id is specified (not null), and all parameters are
        // local-candidate-invariant, then matching for this condition doesn't
        // need to check each candidate object separately for matching, so
        // don't need to use EvalImpl and can instead do a simpler transfer
        bool match = Match(parent_context);

        // transfer objects to or from candidate set, according to whether the
        // specified empire meter was in the requested range
        if (match && search_domain == SearchDomain::NON_MATCHES) {
            // move all objects from non_matches to matches
            matches.insert(matches.end(), non_matches.begin(), non_matches.end());
            non_matches.clear();
        } else if (!match && search_domain == SearchDomain::MATCHES) {
            // move all objects from matches to non_matches
            non_matches.insert(non_matches.end(), matches.begin(), matches.end());
            matches.clear();
        }

    } else {
        // re-evaluate allowed turn range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string EmpireHasAdoptedPolicy::Description(bool negated/* = false*/) const {
    std::string name_str;
    if (m_name) {
        name_str = m_name->Description();
        if (m_name->ConstantExpr() && UserStringExists(name_str))
            name_str = UserString(name_str);
    }
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_EMPIRE_HAS_ADOPTED_POLICY")
        : UserString("DESC_EMPIRE_HAS_ADOPTED_POLICY_NOT"))
        % name_str);
}

std::string EmpireHasAdoptedPolicy::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "EmpireHasAdoptedPolicy";
    if (m_empire_id)
        retval += " empire = " + m_empire_id->Dump(ntabs);
    if (m_name)
        retval += " name = " + m_name->Dump(ntabs);
    retval += "\n";
    return retval;
}

bool EmpireHasAdoptedPolicy::Match(const ScriptingContext& local_context) const {
    int empire_id = ALL_EMPIRES;
    auto& candidate = local_context.condition_local_candidate;
    // if m_empire_id not set, default to candidate object's owner
    if (!m_empire_id && !candidate) {
        ErrorLogger() << "EmpireHasAdoptedPolicy::Match passed no candidate object but expects one due to having no empire id valueref specified and thus wanting to use the local candidate's owner as the empire id";
        return false;

    } else if (m_empire_id && !candidate && !m_empire_id->LocalCandidateInvariant()) {
        ErrorLogger() << "EmpireHasAdoptedPolicy::Match passed no candidate object but but empire id valueref references the local candidate";
        return false;

    } else if (!m_empire_id && candidate) {
        // default to candidate's owner if no empire id valueref is specified
        empire_id = candidate->Owner();

    } else if (m_empire_id) {
        // either candidate exists or m_empire_id is local-candidate-invariant (or both)
        empire_id = m_empire_id->Eval(local_context);

    } else {
        ErrorLogger() << "EmpireHasAdoptedPolicy::Match reached unexpected default case for candidate and empire id valueref existance";
        return false;
    }

    auto empire = local_context.GetEmpire(empire_id);
    if (!empire)
         return false;

    std::string name = m_name ? m_name->Eval(local_context) : "";

    return empire->PolicyAdopted(name);
}

void EmpireHasAdoptedPolicy::SetTopLevelContent(const std::string& content_name) {
    if (m_empire_id)
        m_empire_id->SetTopLevelContent(content_name);
    if (m_name)
        m_name->SetTopLevelContent(content_name);
}

unsigned int EmpireHasAdoptedPolicy::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::EmpireHasAdoptedPolicy");
    CheckSums::CheckSumCombine(retval, m_empire_id);
    CheckSums::CheckSumCombine(retval, m_name);

    TraceLogger() << "GetCheckSum(EmpireHasAdoptedPolicy): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> EmpireHasAdoptedPolicy::Clone() const {
    return std::make_unique<EmpireHasAdoptedPolicy>(ValueRef::CloneUnique(m_empire_id),
                                                    ValueRef::CloneUnique(m_name));
}

///////////////////////////////////////////////////////////
// OwnerHasTech                                          //
///////////////////////////////////////////////////////////
OwnerHasTech::OwnerHasTech(std::unique_ptr<ValueRef::ValueRef<int>>&& empire_id,
                           std::unique_ptr<ValueRef::ValueRef<std::string>>&& name) :
    Condition(),
    m_name(std::move(name)),
    m_empire_id(std::move(empire_id))
{
    std::array<const ValueRef::ValueRefBase*, 2> operands = {{m_name.get(), m_empire_id.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

OwnerHasTech::OwnerHasTech(std::unique_ptr<ValueRef::ValueRef<std::string>>&& name) :
    OwnerHasTech(nullptr, std::move(name))
{}

bool OwnerHasTech::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const OwnerHasTech& rhs_ = static_cast<const OwnerHasTech&>(rhs);

    CHECK_COND_VREF_MEMBER(m_empire_id)
    CHECK_COND_VREF_MEMBER(m_name)

    return true;
}

namespace {
    struct OwnerHasTechSimpleMatch {
        OwnerHasTechSimpleMatch(int empire_id, const std::string& name, const ScriptingContext& context) :
            m_empire_id(empire_id),
            m_name(name),
            m_context(context)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            int actual_empire_id = m_empire_id;
            if (m_empire_id == ALL_EMPIRES) {
                if (candidate->Unowned())
                    return false;
                actual_empire_id = candidate->Owner();
            }

            auto empire = m_context.GetEmpire(actual_empire_id);
            if (!empire)
                return false;

            return empire->TechResearched(m_name);
        }

        int                     m_empire_id = ALL_EMPIRES;
        const std::string&      m_name;
        const ScriptingContext& m_context;
    };
}

void OwnerHasTech::Eval(const ScriptingContext& parent_context,
                        ObjectSet& matches, ObjectSet& non_matches,
                        SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    // if m_empire_id not set, the local candidate's owner is used, which is not target invariant
    bool simple_eval_safe = ((m_empire_id && m_empire_id->LocalCandidateInvariant()) &&
                             (!m_name || m_name->LocalCandidateInvariant()) &&
                             (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // evaluate number limits once, use to match all candidates
        int empire_id = m_empire_id->Eval(parent_context);   // check above should ensure m_empire_id is non-null
        std::string name = m_name ? m_name->Eval(parent_context) : "";
        EvalImpl(matches, non_matches, search_domain, OwnerHasTechSimpleMatch(empire_id, name, parent_context));
    } else {
        // re-evaluate allowed turn range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string OwnerHasTech::Description(bool negated/* = false*/) const {
    std::string name_str;
    if (m_name) {
        name_str = m_name->Description();
        if (m_name->ConstantExpr() && UserStringExists(name_str))
            name_str = UserString(name_str);
    }
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_OWNER_HAS_TECH")
        : UserString("DESC_OWNER_HAS_TECH_NOT"))
        % name_str);
}

std::string OwnerHasTech::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "OwnerHasTech";
    if (m_empire_id)
        retval += " empire = " + m_empire_id->Dump(ntabs);
    if (m_name)
        retval += " name = " + m_name->Dump(ntabs);
    retval += "\n";
    return retval;
}

bool OwnerHasTech::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "OwnerHasTech::Match passed no candidate object";
        return false;
    }

    int empire_id = (m_empire_id ? m_empire_id->Eval(local_context) : candidate->Owner());
    if (empire_id == ALL_EMPIRES)
        return false;
    std::string name = m_name ? m_name->Eval(local_context) : "";

    return OwnerHasTechSimpleMatch(empire_id, name, local_context)(candidate);
}

void OwnerHasTech::SetTopLevelContent(const std::string& content_name) {
    if (m_empire_id)
        m_empire_id->SetTopLevelContent(content_name);
    if (m_name)
        m_name->SetTopLevelContent(content_name);
}

unsigned int OwnerHasTech::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::OwnerHasTech");
    CheckSums::CheckSumCombine(retval, m_empire_id);
    CheckSums::CheckSumCombine(retval, m_name);

    TraceLogger() << "GetCheckSum(OwnerHasTech): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> OwnerHasTech::Clone() const {
    return std::make_unique<OwnerHasTech>(ValueRef::CloneUnique(m_empire_id),
                                          ValueRef::CloneUnique(m_name));
}

///////////////////////////////////////////////////////////
// OwnerHasBuildingTypeAvailable                         //
///////////////////////////////////////////////////////////
OwnerHasBuildingTypeAvailable::OwnerHasBuildingTypeAvailable(
    std::unique_ptr<ValueRef::ValueRef<int>>&& empire_id,
    std::unique_ptr<ValueRef::ValueRef<std::string>>&& name) :
    Condition(),
    m_name(std::move(name)),
    m_empire_id(std::move(empire_id))
{
    std::array<const ValueRef::ValueRefBase*, 2> operands = {{m_name.get(), m_empire_id.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

OwnerHasBuildingTypeAvailable::OwnerHasBuildingTypeAvailable(const std::string& name) :
    OwnerHasBuildingTypeAvailable(nullptr, std::make_unique<ValueRef::Constant<std::string>>(name))
{}

OwnerHasBuildingTypeAvailable::OwnerHasBuildingTypeAvailable(
    std::unique_ptr<ValueRef::ValueRef<std::string>>&& name) :
    OwnerHasBuildingTypeAvailable(nullptr, std::move(name))
{}

bool OwnerHasBuildingTypeAvailable::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const OwnerHasBuildingTypeAvailable& rhs_ = static_cast<const OwnerHasBuildingTypeAvailable&>(rhs);

    CHECK_COND_VREF_MEMBER(m_empire_id)
    CHECK_COND_VREF_MEMBER(m_name)

    return true;
}

namespace {
    struct OwnerHasBuildingTypeAvailableSimpleMatch {
        OwnerHasBuildingTypeAvailableSimpleMatch(int empire_id, const std::string& name, const ScriptingContext& context) :
            m_empire_id(empire_id),
            m_name(name),
            m_context(context)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            int actual_empire_id = m_empire_id;
            if (m_empire_id == ALL_EMPIRES) {
                if (candidate->Unowned())
                    return false;
                actual_empire_id = candidate->Owner();
            }

            auto empire = m_context.GetEmpire(actual_empire_id);
            if (!empire)
                return false;

            return empire->BuildingTypeAvailable(m_name);
        }

        int                     m_empire_id = ALL_EMPIRES;
        const std::string&      m_name;
        const ScriptingContext& m_context;
    };
}

void OwnerHasBuildingTypeAvailable::Eval(const ScriptingContext& parent_context,
                                         ObjectSet& matches, ObjectSet& non_matches,
                                         SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    // if m_empire_id not set, the local candidate's owner is used, which is not target invariant
    bool simple_eval_safe = ((m_empire_id && m_empire_id->LocalCandidateInvariant()) &&
                             (!m_name || m_name->LocalCandidateInvariant()) &&
                             (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // evaluate number limits once, use to match all candidates
        int empire_id = m_empire_id->Eval(parent_context);   // check above should ensure m_empire_id is non-null
        std::string name = m_name ? m_name->Eval(parent_context) : "";
        EvalImpl(matches, non_matches, search_domain, OwnerHasBuildingTypeAvailableSimpleMatch(empire_id, name, parent_context));
    } else {
        // re-evaluate allowed turn range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string OwnerHasBuildingTypeAvailable::Description(bool negated/* = false*/) const {
    // used internally for a tooltip where context is apparent, so don't need
    // to name builing type here
    return (!negated)
        ? UserString("DESC_OWNER_HAS_BUILDING_TYPE")
        : UserString("DESC_OWNER_HAS_BUILDING_TYPE_NOT");
}

std::string OwnerHasBuildingTypeAvailable::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "OwnerHasBuildingTypeAvailable";
    if (m_empire_id)
        retval += " empire = " + m_empire_id->Dump(ntabs);
    if (m_name)
        retval += " name = " + m_name->Dump(ntabs);
    retval += "\n";
    return retval;
}

bool OwnerHasBuildingTypeAvailable::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "OwnerHasTech::Match passed no candidate object";
        return false;
    }

    int empire_id = (m_empire_id ? m_empire_id->Eval(local_context) : candidate->Owner());
    if (empire_id == ALL_EMPIRES)
        return false;
    std::string name = m_name ? m_name->Eval(local_context) : "";

    return OwnerHasBuildingTypeAvailableSimpleMatch(empire_id, name, local_context)(candidate);
}

void OwnerHasBuildingTypeAvailable::SetTopLevelContent(const std::string& content_name) {
    if (m_empire_id)
        m_empire_id->SetTopLevelContent(content_name);
    if (m_name)
        m_name->SetTopLevelContent(content_name);
}

unsigned int OwnerHasBuildingTypeAvailable::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::OwnerHasBuildingTypeAvailable");
    CheckSums::CheckSumCombine(retval, m_empire_id);
    CheckSums::CheckSumCombine(retval, m_name);

    TraceLogger() << "GetCheckSum(OwnerHasBuildingTypeAvailable): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> OwnerHasBuildingTypeAvailable::Clone() const {
    return std::make_unique<OwnerHasBuildingTypeAvailable>(ValueRef::CloneUnique(m_empire_id),
                                                           ValueRef::CloneUnique(m_name));
}

///////////////////////////////////////////////////////////
// OwnerHasShipDesignAvailable                           //
///////////////////////////////////////////////////////////
OwnerHasShipDesignAvailable::OwnerHasShipDesignAvailable(
    std::unique_ptr<ValueRef::ValueRef<int>>&& empire_id,
    std::unique_ptr<ValueRef::ValueRef<int>>&& design_id) :
    Condition(),
    m_id(std::move(design_id)),
    m_empire_id(std::move(empire_id))
{
    std::array<const ValueRef::ValueRefBase*, 2> operands = {{m_id.get(), m_empire_id.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

OwnerHasShipDesignAvailable::OwnerHasShipDesignAvailable(int design_id) :
    OwnerHasShipDesignAvailable(nullptr, std::make_unique<ValueRef::Constant<int>>(design_id))
{}

OwnerHasShipDesignAvailable::OwnerHasShipDesignAvailable(std::unique_ptr<ValueRef::ValueRef<int>>&& design_id) :
    OwnerHasShipDesignAvailable(nullptr, std::move(design_id))
{}

bool OwnerHasShipDesignAvailable::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const OwnerHasShipDesignAvailable& rhs_ = static_cast<const OwnerHasShipDesignAvailable&>(rhs);

    CHECK_COND_VREF_MEMBER(m_empire_id)
    CHECK_COND_VREF_MEMBER(m_id)

    return true;
}

namespace {
    struct OwnerHasShipDesignAvailableSimpleMatch {
        OwnerHasShipDesignAvailableSimpleMatch(int empire_id, int design_id, const ScriptingContext& context) :
            m_empire_id(empire_id),
            m_id(design_id),
            m_context(context)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            int actual_empire_id = m_empire_id;
            if (m_empire_id == ALL_EMPIRES) {
                if (candidate->Unowned())
                    return false;
                actual_empire_id = candidate->Owner();
            }

            auto empire = m_context.GetEmpire(actual_empire_id);
            if (!empire)
                return false;

            return empire->ShipDesignAvailable(m_id, m_context.ContextUniverse());
        }

        int                     m_empire_id = ALL_EMPIRES;
        int                     m_id = INVALID_DESIGN_ID;
        const ScriptingContext& m_context;
    };
}

void OwnerHasShipDesignAvailable::Eval(const ScriptingContext& parent_context,
                                       ObjectSet& matches, ObjectSet& non_matches,
                                       SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    // if m_empire_id not set, the local candidate's owner is used, which is not target invariant
    bool simple_eval_safe = ((m_empire_id && m_empire_id->LocalCandidateInvariant()) &&
                             (!m_id || m_id->LocalCandidateInvariant()) &&
                             (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // evaluate number limits once, use to match all candidates
        int empire_id = m_empire_id->Eval(parent_context);   // check above should ensure m_empire_id is non-null
        int design_id = m_id ? m_id->Eval(parent_context) : INVALID_DESIGN_ID;
        EvalImpl(matches, non_matches, search_domain, OwnerHasShipDesignAvailableSimpleMatch(empire_id, design_id, parent_context));
    } else {
        // re-evaluate allowed turn range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string OwnerHasShipDesignAvailable::Description(bool negated/* = false*/) const {
    // used internally for a tooltip where context is apparent, so don't need
    // to specify design here
    return (!negated)
        ? UserString("DESC_OWNER_HAS_SHIP_DESIGN")
        : UserString("DESC_OWNER_HAS_SHIP_DESIGN_NOT");
}

std::string OwnerHasShipDesignAvailable::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "OwnerHasShipDesignAvailable";
    if (m_empire_id)
        retval += " empire = " + m_empire_id->Dump(ntabs);
    if (m_id)
        retval += " id = " + m_id->Dump(ntabs);
    retval += "\n";
    return retval;
}

bool OwnerHasShipDesignAvailable::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "OwnerHasTech::Match passed no candidate object";
        return false;
    }

    int empire_id = (m_empire_id ? m_empire_id->Eval(local_context) : candidate->Owner());
    if (empire_id == ALL_EMPIRES)
        return false;
    int design_id = m_id ? m_id->Eval(local_context) : INVALID_DESIGN_ID;

    return OwnerHasShipDesignAvailableSimpleMatch(empire_id, design_id, local_context)(candidate);
}

void OwnerHasShipDesignAvailable::SetTopLevelContent(const std::string& content_name) {
    if (m_empire_id)
        m_empire_id->SetTopLevelContent(content_name);
    if (m_id)
        m_id->SetTopLevelContent(content_name);
}

unsigned int OwnerHasShipDesignAvailable::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::OwnerHasShipDesignAvailable");
    CheckSums::CheckSumCombine(retval, m_empire_id);
    CheckSums::CheckSumCombine(retval, m_id);

    TraceLogger() << "GetCheckSum(OwnerHasShipDesignAvailable): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> OwnerHasShipDesignAvailable::Clone() const {
    return std::make_unique<OwnerHasShipDesignAvailable>(ValueRef::CloneUnique(m_empire_id),
                                                         ValueRef::CloneUnique(m_id));
}

///////////////////////////////////////////////////////////
// OwnerHasShipPartAvailable                             //
///////////////////////////////////////////////////////////
OwnerHasShipPartAvailable::OwnerHasShipPartAvailable(
    std::unique_ptr<ValueRef::ValueRef<int>>&& empire_id,
    std::unique_ptr<ValueRef::ValueRef<std::string>>&& name) :
    Condition(),
    m_name(std::move(name)),
    m_empire_id(std::move(empire_id))
{
    std::array<const ValueRef::ValueRefBase*, 2> operands = {{m_empire_id.get(), m_name.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

OwnerHasShipPartAvailable::OwnerHasShipPartAvailable(const std::string& name) :
    OwnerHasShipPartAvailable(nullptr, std::make_unique<ValueRef::Constant<std::string>>(name))
{}

OwnerHasShipPartAvailable::OwnerHasShipPartAvailable(std::unique_ptr<ValueRef::ValueRef<std::string>>&& name) :
    OwnerHasShipPartAvailable(nullptr, std::move(name))
{}

bool OwnerHasShipPartAvailable::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const OwnerHasShipPartAvailable& rhs_ = static_cast<const OwnerHasShipPartAvailable&>(rhs);

    CHECK_COND_VREF_MEMBER(m_empire_id)
    CHECK_COND_VREF_MEMBER(m_name)

    return true;
}

namespace {
    struct OwnerHasShipPartAvailableSimpleMatch {
        OwnerHasShipPartAvailableSimpleMatch(int empire_id, const std::string& name, const ScriptingContext& context) :
            m_empire_id(empire_id),
            m_name(name),
            m_context(context)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            int actual_empire_id = m_empire_id;
            if (m_empire_id == ALL_EMPIRES) {
                if (candidate->Unowned())
                    return false;
                actual_empire_id = candidate->Owner();
            }

            auto empire = m_context.GetEmpire(actual_empire_id);
            if (!empire)
                return false;

            return empire->ShipPartAvailable(m_name);
        }

        int                     m_empire_id = ALL_EMPIRES;
        const std::string&      m_name;
        const ScriptingContext& m_context;
    };
}

void OwnerHasShipPartAvailable::Eval(const ScriptingContext& parent_context,
                                     ObjectSet& matches, ObjectSet& non_matches,
                                     SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    // if m_empire_id not set, the local candidate's owner is used, which is not target invariant
    bool simple_eval_safe = ((m_empire_id && m_empire_id->LocalCandidateInvariant()) &&
                             (!m_name || m_name->LocalCandidateInvariant()) &&
                             (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // evaluate number limits once, use to match all candidates
        int empire_id = m_empire_id->Eval(parent_context);   // check above should ensure m_empire_id is non-null
        std::string name = m_name ? m_name->Eval(parent_context) : "";
        EvalImpl(matches, non_matches, search_domain, OwnerHasShipPartAvailableSimpleMatch(empire_id, name, parent_context));
    } else {
        // re-evaluate allowed turn range for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string OwnerHasShipPartAvailable::Description(bool negated/* = false*/) const {
    return (!negated)
        ? UserString("DESC_OWNER_HAS_SHIP_PART")
        : UserString("DESC_OWNER_HAS_SHIP_PART_NOT");
}

std::string OwnerHasShipPartAvailable::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "OwnerHasShipPartAvailable";
    if (m_empire_id)
        retval += " empire = " + m_empire_id->Dump(ntabs);
    if (m_name)
        retval += " name = " + m_name->Dump(ntabs);
    retval += "\n";
    return retval;
}

bool OwnerHasShipPartAvailable::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "OwnerHasShipPart::Match passed no candidate object";
        return false;
    }

    int empire_id = (m_empire_id ? m_empire_id->Eval(local_context) : candidate->Owner());
    if (empire_id == ALL_EMPIRES)
        return false;
    std::string name = m_name ? m_name->Eval(local_context) : "";

    return OwnerHasShipPartAvailableSimpleMatch(empire_id, name, local_context)(candidate);
}

void OwnerHasShipPartAvailable::SetTopLevelContent(const std::string& content_name) {
    if (m_empire_id)
        m_empire_id->SetTopLevelContent(content_name);
    if (m_name)
        m_name->SetTopLevelContent(content_name);
}

unsigned int OwnerHasShipPartAvailable::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::OwnerHasShipPartAvailable");
    CheckSums::CheckSumCombine(retval, m_empire_id);
    CheckSums::CheckSumCombine(retval, m_name);

    TraceLogger() << "GetCheckSum(OwnerHasShipPartAvailable): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> OwnerHasShipPartAvailable::Clone() const {
    return std::make_unique<OwnerHasShipPartAvailable>(ValueRef::CloneUnique(m_empire_id),
                                                       ValueRef::CloneUnique(m_name));
}

///////////////////////////////////////////////////////////
// VisibleToEmpire                                       //
///////////////////////////////////////////////////////////
VisibleToEmpire::VisibleToEmpire(std::unique_ptr<ValueRef::ValueRef<int>>&& empire_id) :
    VisibleToEmpire(std::move(empire_id), nullptr, nullptr)
{}

VisibleToEmpire::VisibleToEmpire(std::unique_ptr<ValueRef::ValueRef<int>>&& empire_id,
                                 std::unique_ptr<ValueRef::ValueRef<int>>&& since_turn,
                                 std::unique_ptr<ValueRef::ValueRef<Visibility>>&& vis) :
    m_empire_id(std::move(empire_id)),
    m_since_turn(std::move(since_turn)),
    m_vis(std::move(vis))
{
    std::array<const ValueRef::ValueRefBase*, 3> operands = {{m_empire_id.get(), m_since_turn.get(), m_vis.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

bool VisibleToEmpire::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const VisibleToEmpire& rhs_ = static_cast<const VisibleToEmpire&>(rhs);

    CHECK_COND_VREF_MEMBER(m_empire_id)
    CHECK_COND_VREF_MEMBER(m_since_turn)
    CHECK_COND_VREF_MEMBER(m_vis)

    return true;
}

namespace {
    struct VisibleToEmpireSimpleMatch {
        VisibleToEmpireSimpleMatch(int empire_id, int since_turn, Visibility vis,
                                   const ScriptingContext& context) :
            m_empire_id(empire_id),
            m_since_turn(since_turn),
            m_vis(vis == Visibility::INVALID_VISIBILITY ? Visibility::VIS_BASIC_VISIBILITY : vis),
            m_context{context}
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;
            if (m_empire_id == ALL_EMPIRES)
                return true;
            if (m_vis == Visibility::VIS_NO_VISIBILITY)
                return true;

            if (m_since_turn == INVALID_GAME_TURN) {
                // no valid game turn was specified, so use current universe state
                return m_context.ContextVis(candidate->ID(), m_empire_id) >= m_vis;

            } else {
                // if a game turn after which to check is specified, check the
                // history of when empires saw which objects at which visibility
                const auto& vis_turn_map{m_context.empire_object_vis_turns};
                auto empire_it = vis_turn_map.find(m_empire_id);
                if (empire_it == vis_turn_map.end())
                    return false;
                const auto& object_vis_turns_map = empire_it->second;
                auto object_it = object_vis_turns_map.find(candidate->ID());
                if (object_it == object_vis_turns_map.end())
                    return false;
                const auto& vis_turns = object_it->second;
                auto vis_it = vis_turns.find(m_vis);
                if (vis_it == vis_turns.end())
                    return false;
                return vis_it->second >= m_since_turn;
            }
        }

        int                     m_empire_id = ALL_EMPIRES;
        int                     m_since_turn = BEFORE_FIRST_TURN;
        Visibility              m_vis = Visibility::VIS_BASIC_VISIBILITY;
        const ScriptingContext& m_context;

    };
}

void VisibleToEmpire::Eval(const ScriptingContext& parent_context,
                           ObjectSet& matches, ObjectSet& non_matches,
                           SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = (!m_empire_id || m_empire_id->LocalCandidateInvariant()) &&
                            (!m_since_turn || m_since_turn->LocalCandidateInvariant()) &&
                            (!m_vis || m_vis->LocalCandidateInvariant()) &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant());

    if (simple_eval_safe) {
        // evaluate empire id once, and use to check all candidate objects
        int empire_id = m_empire_id ? m_empire_id->Eval(parent_context) : ALL_EMPIRES;
        int since_turn = m_since_turn ? m_since_turn->Eval(parent_context) : INVALID_GAME_TURN;  // indicates current turn
        Visibility vis = m_vis ? m_vis->Eval(parent_context) : Visibility::VIS_BASIC_VISIBILITY;

        // need to check visibility of each candidate object separately
        EvalImpl(matches, non_matches, search_domain,
                 VisibleToEmpireSimpleMatch(empire_id, since_turn, vis, parent_context));
    } else {
        // re-evaluate empire id for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string VisibleToEmpire::Description(bool negated/* = false*/) const {
    std::string empire_str;
    if (m_empire_id) {
        int empire_id = ALL_EMPIRES;
        if (m_empire_id->ConstantExpr())
            empire_id = m_empire_id->Eval();
        if (const Empire* empire = GetEmpire(empire_id))
            empire_str = empire->Name();
        else
            empire_str = m_empire_id->Description();
    } else {
        empire_str = UserString("DESC_ANY_EMPIRE");
    }

    std::string vis_string;
    if (m_vis) {
        if (m_vis->ConstantExpr()) {
            vis_string = UserString(boost::lexical_cast<std::string>(m_vis->Eval()));
        } else {
            vis_string = m_vis->Description();
        }
    } else {
        // default if vis level not specified is any detected visibility level
        vis_string = UserString(boost::lexical_cast<std::string>(Visibility::VIS_BASIC_VISIBILITY));
    }

    std::string turn_string;
    if (m_since_turn) {
        if (m_since_turn->ConstantExpr()) {
            int turn = m_since_turn->Eval();
            if (turn != INVALID_GAME_TURN)
                turn_string = std::to_string(turn);
        } else {
            turn_string = m_since_turn->Description();
        }
    }

    if (turn_string.empty()) {
        return str(FlexibleFormat((!negated)
            ? UserString("DESC_VISIBLE_TO_EMPIRE")
            : UserString("DESC_VISIBLE_TO_EMPIRE_NOT"))
                   % empire_str
                   % vis_string);
    } else {
        return str(FlexibleFormat((!negated)
            ? UserString("DESC_VISIBLE_TO_EMPIRE_SINCE_TURN")
            : UserString("DESC_VISIBLE_TO_EMPIRE_SINCE_TURN_NOT"))
                   % empire_str
                   % turn_string
                   % vis_string);
    }
}

std::string VisibleToEmpire::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "VisibleToEmpire";
    if (m_empire_id)
        retval += " empire = " + m_empire_id->Dump(ntabs);
    if (m_since_turn)
        retval += " turn = " + m_since_turn->Dump(ntabs);
    if (m_vis)
        retval += " visibility = " + m_vis->Dump(ntabs);
    retval += "\n";
    return retval;
}

bool VisibleToEmpire::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "VisibleToEmpire::Match passed no candidate object";
        return false;
    }

    int empire_id = m_empire_id ? m_empire_id->Eval(local_context) : ALL_EMPIRES;
    int since_turn = m_since_turn ? m_since_turn->Eval(local_context) : INVALID_GAME_TURN;  // indicates current turn
    Visibility vis = m_vis ? m_vis->Eval(local_context) : Visibility::VIS_BASIC_VISIBILITY;
    return VisibleToEmpireSimpleMatch(empire_id, since_turn, vis, local_context)(candidate);
}

void VisibleToEmpire::SetTopLevelContent(const std::string& content_name) {
    if (m_empire_id)
        m_empire_id->SetTopLevelContent(content_name);
    if (m_since_turn)
        m_since_turn->SetTopLevelContent(content_name);
    if (m_vis)
        m_vis->SetTopLevelContent(content_name);
}

unsigned int VisibleToEmpire::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::VisibleToEmpire");
    CheckSums::CheckSumCombine(retval, m_empire_id);
    CheckSums::CheckSumCombine(retval, m_since_turn);
    CheckSums::CheckSumCombine(retval, m_vis);

    TraceLogger() << "GetCheckSum(VisibleToEmpire): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> VisibleToEmpire::Clone() const {
    return std::make_unique<VisibleToEmpire>(ValueRef::CloneUnique(m_empire_id),
                                             ValueRef::CloneUnique(m_since_turn),
                                             ValueRef::CloneUnique(m_vis));
}

///////////////////////////////////////////////////////////
// WithinDistance                                        //
///////////////////////////////////////////////////////////
WithinDistance::WithinDistance(std::unique_ptr<ValueRef::ValueRef<double>>&& distance,
                               std::unique_ptr<Condition>&& condition) :
    Condition(),
    m_distance(std::move(distance)),
    m_condition(std::move(condition))
{
    m_root_candidate_invariant =
        (!m_distance || m_distance->RootCandidateInvariant()) &&
        (!m_condition || m_condition->RootCandidateInvariant());
    m_target_invariant =
        (!m_distance || m_distance->TargetInvariant()) &&
        (!m_condition || m_condition->TargetInvariant());
    m_source_invariant =
        (!m_distance || m_distance->SourceInvariant()) &&
        (!m_condition || m_condition->SourceInvariant());
}

bool WithinDistance::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const WithinDistance& rhs_ = static_cast<const WithinDistance&>(rhs);

    CHECK_COND_VREF_MEMBER(m_distance)
    CHECK_COND_VREF_MEMBER(m_condition)

    return true;
}

namespace {
    struct WithinDistanceSimpleMatch {
        WithinDistanceSimpleMatch(const ObjectSet& from_objects, double distance) :
            m_from_objects(from_objects),
            m_distance2(distance*distance)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            // is candidate object close enough to any of the passed-in objects?
            for (auto& obj : m_from_objects) {
                double delta_x = candidate->X() - obj->X();
                double delta_y = candidate->Y() - obj->Y();
                if (delta_x*delta_x + delta_y*delta_y <= m_distance2)
                    return true;
            }

            return false;
        }

        const ObjectSet& m_from_objects;
        double m_distance2;
    };
}

void WithinDistance::Eval(const ScriptingContext& parent_context,
                          ObjectSet& matches, ObjectSet& non_matches,
                          SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = m_distance->LocalCandidateInvariant() &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant());
    if (simple_eval_safe) {
        // evaluate contained objects and distance once and check for all candidates
        TraceLogger(conditions) << "WithinDistance::Eval simple case";

        // get subcondition matches
        ObjectSet subcondition_matches;
        m_condition->Eval(parent_context, subcondition_matches);
        double distance = m_distance->Eval(parent_context);

        // need to check locations (with respect to subcondition matches) of candidates separately
        EvalImpl(matches, non_matches, search_domain, WithinDistanceSimpleMatch(subcondition_matches, distance));
    } else {
        // re-evaluate contained objects for each candidate object
        TraceLogger(conditions) << "WithinDistance::Eval full case";
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string WithinDistance::Description(bool negated/* = false*/) const {
    std::string value_str = m_distance->ConstantExpr() ?
                                std::to_string(m_distance->Eval()) :
                                m_distance->Description();
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_WITHIN_DISTANCE")
        : UserString("DESC_WITHIN_DISTANCE_NOT"))
               % value_str
               % m_condition->Description());
}

std::string WithinDistance::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "WithinDistance distance = " + m_distance->Dump(ntabs) + " condition =\n";
    retval += m_condition->Dump(ntabs+1);
    return retval;
}

bool WithinDistance::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "WithinDistance::Match passed no candidate object";
        return false;
    }

    // get subcondition matches
    ObjectSet subcondition_matches;
    m_condition->Eval(local_context, subcondition_matches);
    if (subcondition_matches.empty())
        return false;

    return WithinDistanceSimpleMatch(subcondition_matches, m_distance->Eval(local_context))(candidate);
}

void WithinDistance::SetTopLevelContent(const std::string& content_name) {
    if (m_distance)
        m_distance->SetTopLevelContent(content_name);
    if (m_condition)
        m_condition->SetTopLevelContent(content_name);
}

unsigned int WithinDistance::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::WithinDistance");
    CheckSums::CheckSumCombine(retval, m_distance);
    CheckSums::CheckSumCombine(retval, m_condition);

    TraceLogger() << "GetCheckSum(WithinDistance): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> WithinDistance::Clone() const {
    return std::make_unique<WithinDistance>(ValueRef::CloneUnique(m_distance),
                                            ValueRef::CloneUnique(m_condition));
}

///////////////////////////////////////////////////////////
// WithinStarlaneJumps                                   //
///////////////////////////////////////////////////////////
WithinStarlaneJumps::WithinStarlaneJumps(std::unique_ptr<ValueRef::ValueRef<int>>&& jumps,
                                         std::unique_ptr<Condition>&& condition) :
    Condition(),
    m_jumps(std::move(jumps)),
    m_condition(std::move(condition))
{
    m_root_candidate_invariant =
        (!m_jumps || m_jumps->RootCandidateInvariant()) &&
        (!m_condition || m_condition->RootCandidateInvariant());
    m_target_invariant =
        (!m_jumps || m_jumps->TargetInvariant()) &&
        (!m_condition || m_condition->TargetInvariant());
    m_source_invariant =
        (!m_jumps || m_jumps->SourceInvariant()) &&
        (!m_condition || m_condition->SourceInvariant());
}

bool WithinStarlaneJumps::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const WithinStarlaneJumps& rhs_ = static_cast<const WithinStarlaneJumps&>(rhs);

    CHECK_COND_VREF_MEMBER(m_jumps)
    CHECK_COND_VREF_MEMBER(m_condition)

    return true;
}

void WithinStarlaneJumps::Eval(const ScriptingContext& parent_context,
                               ObjectSet& matches, ObjectSet& non_matches,
                               SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = m_jumps->LocalCandidateInvariant() &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant());
    if (simple_eval_safe) {
        // evaluate contained objects and jumps limit once and check for all candidates

        // get subcondition matches
        ObjectSet subcondition_matches;
        m_condition->Eval(parent_context, subcondition_matches);
        int jump_limit = m_jumps->Eval(parent_context);
        ObjectSet &from_set(search_domain == SearchDomain::MATCHES ? matches : non_matches);

        std::tie(matches, non_matches) = parent_context.ContextUniverse().GetPathfinder()->WithinJumpsOfOthers(
            jump_limit, parent_context.ContextObjects(), from_set, subcondition_matches);

    } else {
        // re-evaluate contained objects for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string WithinStarlaneJumps::Description(bool negated/* = false*/) const {
    std::string value_str = m_jumps->ConstantExpr() ? std::to_string(m_jumps->Eval()) : m_jumps->Description();
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_WITHIN_STARLANE_JUMPS")
        : UserString("DESC_WITHIN_STARLANE_JUMPS_NOT"))
               % value_str
               % m_condition->Description());
}

std::string WithinStarlaneJumps::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "WithinStarlaneJumps jumps = " + m_jumps->Dump(ntabs) + " condition =\n";
    retval += m_condition->Dump(ntabs+1);
    return retval;
}

bool WithinStarlaneJumps::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "WithinStarlaneJumps::Match passed no candidate object";
        return false;
    }

    // get subcondition matches
    ObjectSet subcondition_matches;
    m_condition->Eval(local_context, subcondition_matches);
    if (subcondition_matches.empty())
        return false;

    int jump_limit = m_jumps->Eval(local_context);
    if (jump_limit < 0)
        return false;

    ObjectSet candidate_set{candidate};

    // candidate objects within jumps of subcondition_matches objects
    ObjectSet near_objs;

    std::tie(near_objs, std::ignore) =
        local_context.ContextUniverse().GetPathfinder()->WithinJumpsOfOthers(
            jump_limit, local_context.ContextObjects(), candidate_set, subcondition_matches);
    return !near_objs.empty();
}

void WithinStarlaneJumps::SetTopLevelContent(const std::string& content_name) {
    if (m_jumps)
        m_jumps->SetTopLevelContent(content_name);
    if (m_condition)
        m_condition->SetTopLevelContent(content_name);
}

unsigned int WithinStarlaneJumps::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::WithinStarlaneJumps");
    CheckSums::CheckSumCombine(retval, m_jumps);
    CheckSums::CheckSumCombine(retval, m_condition);

    TraceLogger() << "GetCheckSum(WithinStarlaneJumps): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> WithinStarlaneJumps::Clone() const {
    return std::make_unique<WithinStarlaneJumps>(ValueRef::CloneUnique(m_jumps),
                                                 ValueRef::CloneUnique(m_condition));
}

///////////////////////////////////////////////////////////
// CanAddStarlaneConnection                              //
///////////////////////////////////////////////////////////
CanAddStarlaneConnection::CanAddStarlaneConnection(std::unique_ptr<Condition>&& condition) :
    Condition(),
    m_condition(std::move(condition))
{
    m_root_candidate_invariant = !m_condition || m_condition->RootCandidateInvariant();
    m_target_invariant = !m_condition || m_condition->TargetInvariant();
    m_source_invariant = !m_condition || m_condition->SourceInvariant();
}

bool CanAddStarlaneConnection::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const CanAddStarlaneConnection& rhs_ = static_cast<const CanAddStarlaneConnection&>(rhs);

    CHECK_COND_VREF_MEMBER(m_condition)

    return true;
}

namespace {
    // check if two destination systems, connected to the same origin system
    // would have starlanes too close angularly to eachother
    bool LanesAngularlyTooClose(const UniverseObject* sys1,
                                const UniverseObject* lane1_sys2,
                                const UniverseObject* lane2_sys2)
    {
        if (!sys1 || !lane1_sys2 || !lane2_sys2)
            return true;
        if (sys1 == lane1_sys2 || sys1 == lane2_sys2 || lane1_sys2 == lane2_sys2)
            return true;

        float dx1 = lane1_sys2->X() - sys1->X();
        float dy1 = lane1_sys2->Y() - sys1->Y();
        float mag = std::sqrt(dx1*dx1 + dy1*dy1);
        if (mag == 0.0f)
            return true;
        dx1 /= mag;
        dy1 /= mag;

        float dx2 = lane2_sys2->X() - sys1->X();
        float dy2 = lane2_sys2->Y() - sys1->Y();
        mag = std::sqrt(dx2*dx2 + dy2*dy2);
        if (mag == 0.0f)
            return true;
        dx2 /= mag;
        dy2 /= mag;


        constexpr float MAX_LANE_DOT_PRODUCT = 0.87f; // magic limit adjusted to allow no more than 12 starlanes from a system
                                                      // arccos(0.87) = 0.515594 rad = 29.5 degrees

        float dp = (dx1 * dx2) + (dy1 * dy2);
        //TraceLogger() << "systems: " << sys1->UniverseObject::Name() << "  " << lane1_sys2->UniverseObject::Name() << "  " << lane2_sys2->UniverseObject::Name() << "  dp: " << dp << "\n";

        return dp >= MAX_LANE_DOT_PRODUCT;   // if dot product too high after normalizing vectors, angles are adequately separated
    }

    // check the distance between a system and a (possibly nonexistant)
    // starlane between two other systems. distance here is how far the third
    // system is from the line passing through the lane endpoint systems, as
    // long as the third system is closer to either end point than the endpoints
    // are to eachother. if the third system is further than the endpoints, than
    // the distance to the line is not considered and the lane is considered
    // acceptable
    bool ObjectTooCloseToLane(const UniverseObject* lane_end_sys1,
                              const UniverseObject* lane_end_sys2,
                              const UniverseObject* obj)
    {
        if (!lane_end_sys1 || !lane_end_sys2 || !obj)
            return true;
        if (lane_end_sys1 == lane_end_sys2 || obj == lane_end_sys1 || obj == lane_end_sys2)
            return true;

        // check distances (squared) between object and lane-end systems
        float v_12_x = lane_end_sys2->X() - lane_end_sys1->X();
        float v_12_y = lane_end_sys2->Y() - lane_end_sys1->Y();
        float v_o1_x = lane_end_sys1->X() - obj->X();
        float v_o1_y = lane_end_sys1->Y() - obj->Y();
        float v_o2_x = lane_end_sys2->X() - obj->X();
        float v_o2_y = lane_end_sys2->Y() - obj->Y();

        float dist2_12 = v_12_x*v_12_x + v_12_y*v_12_y;
        float dist2_o1 = v_o1_x*v_o1_x + v_o1_y*v_o1_y;
        float dist2_o2 = v_o2_x*v_o2_x + v_o2_y*v_o2_y;

        // object to zero-length lanes
        if (dist2_12 == 0.0f || dist2_o1 == 0.0f || dist2_o2 == 0.0f)
            return true;

        // if object is further from either of the lane end systems than they
        // are from eachother, it is fine, regardless of the right-angle
        // distance to the line between the systems
        if (dist2_12 < dist2_o1 || dist2_12 < dist2_o2)
            return false;


        // check right-angle distance between obj and lane

        // normalize vector components of lane vector
        float mag_12 = std::sqrt(dist2_12);
        if (mag_12 == 0.0f)
            return true;
        v_12_x /= mag_12;
        v_12_y /= mag_12;

        // distance to point from line from vector projection / dot products
        //.......O
        //      /|
        //     / |
        //    /  |d
        //   /   |
        //  /a___|___
        // 1         2
        // (1O).(12) = |1O| |12| cos(a)
        // d = |1O| cos(a) = (1O).(12) / |12|
        // d = -(O1).(12 / |12|)

        constexpr float MIN_PERP_DIST = 20; // magic limit, in units of universe units (uu)

        float perp_dist = std::abs(v_o1_x*v_12_x + v_o1_y*v_12_y);

        return perp_dist < MIN_PERP_DIST;
    }

    inline float CrossProduct(float dx1, float dy1, float dx2, float dy2)
    { return dx1*dy2 - dy1*dx2; }

    bool LanesCross(const System* lane1_end_sys1,
                    const System* lane1_end_sys2,
                    const System* lane2_end_sys1,
                    const System* lane2_end_sys2)
    {
        // are all endpoints valid systems?
        if (!lane1_end_sys1 || !lane1_end_sys2 || !lane2_end_sys1 || !lane2_end_sys2)
            return false;

        // is either lane degenerate (same start and endpoints)
        if (lane1_end_sys1 == lane1_end_sys2 || lane2_end_sys1 == lane2_end_sys2)
            return false;

        // do the two lanes share endpoints?
        bool share_endpoint_1 = lane1_end_sys1 == lane2_end_sys1 || lane1_end_sys1 == lane2_end_sys2;
        bool share_endpoint_2 = lane1_end_sys2 == lane2_end_sys1 || lane1_end_sys2 == lane2_end_sys2;
        if (share_endpoint_1 && share_endpoint_2)
            return true;    // two copies of the same lane?
        if (share_endpoint_1 || share_endpoint_2)
            return false;   // one common endpoing, but not both common, so can't cross in middle

        // calculate vector components for lanes
        // lane 1
        float v_11_12_x = lane1_end_sys2->X() - lane1_end_sys1->X();
        float v_11_12_y = lane1_end_sys2->Y() - lane1_end_sys1->Y();
        // lane 2
        float v_21_22_x = lane2_end_sys2->X() - lane2_end_sys1->X();
        float v_21_22_y = lane2_end_sys2->Y() - lane2_end_sys1->Y();

        // calculate vector components from lane 1 system 1 to lane 2 endpoints
        // lane 1 endpoint 1 to lane 2 endpoint 1
        float v_11_21_x = lane2_end_sys1->X() - lane1_end_sys1->X();
        float v_11_21_y = lane2_end_sys1->Y() - lane1_end_sys1->Y();
        // lane 1 endpoint 1 to lane 2 endpoint 2
        float v_11_22_x = lane2_end_sys2->X() - lane1_end_sys1->X();
        float v_11_22_y = lane2_end_sys2->Y() - lane1_end_sys1->Y();

        // find cross products of vectors to check on which sides of lane 1 the
        // endpoints of lane 2 are located...
        float cp_1_21 = CrossProduct(v_11_12_x, v_11_12_y, v_11_21_x, v_11_21_y);
        float cp_1_22 = CrossProduct(v_11_12_x, v_11_12_y, v_11_22_x, v_11_22_y);
        if (cp_1_21*cp_1_22 >= 0) // product of same sign numbers is positive, of different sign numbers is negative
            return false;   // if same sign, points are on same side of line, so can't cross it

        // calculate vector components from lane 2 system 1 to lane 1 endpoints
        // lane 2 endpoint 1 to lane 1 endpoint 1
        float v_21_11_x = -v_11_21_x;
        float v_21_11_y = -v_11_21_y;
        // lane 2 endpoint 1 to lane 1 endpoint 2
        float v_21_12_x = lane1_end_sys2->X() - lane2_end_sys1->X();
        float v_21_12_y = lane1_end_sys2->Y() - lane2_end_sys1->Y();

        // find cross products of vectors to check on which sides of lane 2 the
        // endpoints of lane 1 are located...
        float cp_2_11 = CrossProduct(v_21_22_x, v_21_22_y, v_21_11_x, v_21_11_y);
        float cp_2_12 = CrossProduct(v_21_22_x, v_21_22_y, v_21_12_x, v_21_12_y);
        if (cp_2_11*cp_2_12 >= 0)
            return false;

        // endpoints of both lines are on opposite sides of the other line, so
        // the lines must cross

        return true;
    }

    bool LaneCrossesExistingLane(const System* lane_end_sys1,
                                 const System* lane_end_sys2,
                                 const ObjectMap& objects)
    {
        if (!lane_end_sys1 || !lane_end_sys2 || lane_end_sys1 == lane_end_sys2)
            return true;

        // loop over all existing lanes in all systems, checking if a lane
        // beween the specified systems would cross any of the existing lanes
        for (auto& system : objects.all<System>()) {
            if (system.get() == lane_end_sys1 || system.get() == lane_end_sys2)
                continue;

            const auto& sys_existing_lanes = system->StarlanesWormholes();

            // check all existing lanes of currently-being-checked system
            for (const auto& lane : sys_existing_lanes) {
                auto lane_end_sys3 = objects.get<System>(lane.first).get();
                if (!lane_end_sys3)
                    continue;
                // don't need to check against existing lanes that include one
                // of the endpoints of the lane is one of the specified systems
                if (lane_end_sys3 == lane_end_sys1 || lane_end_sys3 == lane_end_sys2)
                    continue;

                if (LanesCross(lane_end_sys1, lane_end_sys2, system.get(), lane_end_sys3)) {
                    //TraceLogger() << "... ... ... lane from: " << lane_end_sys1->UniverseObject::Name() << " to: " << lane_end_sys2->UniverseObject::Name() << " crosses lane from: " << system->UniverseObject::Name() << " to: " << lane_end_sys3->UniverseObject::Name() << "\n";
                    return true;
                }
            }
        }

        return false;
    }

    bool LaneTooCloseToOtherSystem(const System* lane_end_sys1,
                                   const System* lane_end_sys2,
                                   const ObjectMap& objects)
    {
        if (!lane_end_sys1 || !lane_end_sys2 || lane_end_sys1 == lane_end_sys2)
            return true;

        // loop over all existing systems, checking if each is too close to a
        // lane between the specified lane endpoints
        for (auto& system : objects.all<System>()) {
            if (system.get() == lane_end_sys1 || system.get() == lane_end_sys2)
                continue;

            if (ObjectTooCloseToLane(lane_end_sys1, lane_end_sys2, system.get()))
                return true;
        }

        return false;
    }

    struct CanAddStarlaneConnectionSimpleMatch {
        CanAddStarlaneConnectionSimpleMatch(const ObjectSet& destination_objects,
                                            const ObjectMap& objects) :
            m_objects(objects)
        {
            // get set of (unique) systems that are or that contain any
            // destination objects
            std::map<int, std::shared_ptr<const System>> dest_systems;
            for (auto& obj : destination_objects) {
                if (auto sys = m_objects.get<System>(obj->SystemID()))
                    dest_systems.emplace(obj->SystemID(), std::move(sys));
            }

            // move into member storage
            m_destination_systems.reserve(dest_systems.size());
            for (auto& id_obj : dest_systems)
                m_destination_systems.push_back(std::move(id_obj.second));
        }

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            // get system from candidate
            auto candidate_sys = dynamic_cast<const System*>(candidate.get());
            if (!candidate_sys)
                candidate_sys = m_objects.get<System>(candidate->SystemID()).get();
            if (!candidate_sys)
                return false;


            // check if candidate is one of the destination systems
            for (auto& destination : m_destination_systems) {
                if (candidate_sys->ID() == destination->ID())
                    return false;
            }


            // check if candidate already has a lane to any of the destination systems
            for (auto& destination : m_destination_systems) {
                if (candidate_sys->HasStarlaneTo(destination->ID()))
                    return false;
            }

            // check if any of the proposed lanes are too close to any already-
            // present lanes of the candidate system
            //TraceLogger() << "... Checking lanes of candidate system: " << candidate->UniverseObject::Name() << "\n";
            for (const auto& lane : candidate_sys->StarlanesWormholes()) {
                auto candidate_existing_lane_end_sys = m_objects.get<System>(lane.first).get();
                if (!candidate_existing_lane_end_sys)
                    continue;

                // check this existing lane against potential lanes to all destination systems
                for (auto& dest_sys : m_destination_systems) {
                    if (LanesAngularlyTooClose(candidate_sys, candidate_existing_lane_end_sys,
                                               dest_sys.get()))
                    {
                        //TraceLogger() << " ... ... can't add lane from candidate: " << candidate_sys->UniverseObject::Name() << " to " << dest_sys->UniverseObject::Name() << " due to existing lane to " << candidate_existing_lane_end_sys->UniverseObject::Name() << "\n";
                        return false;
                    }
                }
            }


            // check if any of the proposed lanes are too close to any already-
            // present lanes of any of the destination systems
            //TraceLogger() << "... Checking lanes of destination systems:" << "\n";
            for (auto& dest_sys : m_destination_systems) {
                // check this destination system's existing lanes against a lane
                // to the candidate system
                for (const auto& dest_lane : dest_sys->StarlanesWormholes()) {
                    auto dest_lane_end_sys = m_objects.get<System>(dest_lane.first).get();
                    if (!dest_lane_end_sys)
                        continue;

                    if (LanesAngularlyTooClose(dest_sys.get(), candidate_sys, dest_lane_end_sys)) {
                        //TraceLogger() << " ... ... can't add lane from candidate: " << candidate_sys->UniverseObject::Name() << " to " << dest_sys->UniverseObject::Name() << " due to existing lane from dest to " << dest_lane_end_sys->UniverseObject::Name() << "\n";
                        return false;
                    }
                }
            }


            // check if any of the proposed lanes are too close to eachother
            //TraceLogger() << "... Checking proposed lanes against eachother" << "\n";
            for (auto it1 = m_destination_systems.begin();
                 it1 != m_destination_systems.end(); ++it1)
            {
                auto dest_sys1 = it1->get();

                // don't need to check a lane in both directions, so start at one past it1
                auto it2 = it1;
                ++it2;
                for (; it2 != m_destination_systems.end(); ++it2) {
                    auto dest_sys2 = it2->get();
                    if (LanesAngularlyTooClose(candidate_sys, dest_sys1, dest_sys2)) {
                        //TraceLogger() << " ... ... can't add lane from candidate: " << candidate_sys->UniverseObject::Name() << " to " << dest_sys1->UniverseObject::Name() << " and also to " << dest_sys2->UniverseObject::Name() << "\n";
                        return false;
                    }
                }
            }


            // check that the proposed lanes are not too close to any existing
            // system they are not connected to
            //TraceLogger() << "... Checking proposed lanes for proximity to other systems" << "\n";
            for (auto& dest_sys : m_destination_systems) {
                if (LaneTooCloseToOtherSystem(candidate_sys, dest_sys.get(), m_objects)) {
                    //TraceLogger() << " ... ... can't add lane from candidate: " << candidate_sys->Name() << " to " << dest_sys->Name() << " due to proximity to another system." << "\n";
                    return false;
                }
            }


            // check that there are no lanes already existing that cross the proposed lanes
            //TraceLogger() << "... Checking for potential lanes crossing existing lanes" << "\n";
            for (auto& dest_sys : m_destination_systems) {
                if (LaneCrossesExistingLane(candidate_sys, dest_sys.get(), m_objects)) {
                    //TraceLogger() << " ... ... can't add lane from candidate: " << candidate_sys->Name() << " to " << dest_sys->Name() << " due to crossing an existing lane." << "\n";
                    return false;
                }
            }

            return true;
        }

        std::vector<std::shared_ptr<const System>> m_destination_systems;
        const ObjectMap& m_objects;
    };
}

void CanAddStarlaneConnection::Eval(const ScriptingContext& parent_context,
                                    ObjectSet& matches, ObjectSet& non_matches,
                                    SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = parent_context.condition_root_candidate || RootCandidateInvariant();
    if (simple_eval_safe) {
        // evaluate contained objects once and check for all candidates

        // get subcondition matches
        ObjectSet subcondition_matches;
        m_condition->Eval(parent_context, subcondition_matches);

        EvalImpl(matches, non_matches, search_domain,
                 CanAddStarlaneConnectionSimpleMatch(subcondition_matches,
                                                     parent_context.ContextObjects()));
    } else {
        // re-evaluate contained objects for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string CanAddStarlaneConnection::Description(bool negated/* = false*/) const {
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_CAN_ADD_STARLANE_CONNECTION") : UserString("DESC_CAN_ADD_STARLANE_CONNECTION_NOT"))
        % m_condition->Description());
}

std::string CanAddStarlaneConnection::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "CanAddStarlanesTo condition =\n";
    retval += m_condition->Dump(ntabs+1);
    return retval;
}

bool CanAddStarlaneConnection::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "CanAddStarlaneConnection::Match passed no candidate object";
        return false;
    }

    // get subcondition matches
    ObjectSet subcondition_matches;
    m_condition->Eval(local_context, subcondition_matches);

    return CanAddStarlaneConnectionSimpleMatch(subcondition_matches,
                                               local_context.ContextObjects())(candidate);
}

void CanAddStarlaneConnection::SetTopLevelContent(const std::string& content_name) {
    if (m_condition)
        m_condition->SetTopLevelContent(content_name);
}

unsigned int CanAddStarlaneConnection::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::CanAddStarlaneConnection");
    CheckSums::CheckSumCombine(retval, m_condition);

    TraceLogger() << "GetCheckSum(CanAddStarlaneConnection): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> CanAddStarlaneConnection::Clone() const {
    return std::make_unique<CanAddStarlaneConnection>(ValueRef::CloneUnique(m_condition));
}

///////////////////////////////////////////////////////////
// ExploredByEmpire                                      //
///////////////////////////////////////////////////////////
ExploredByEmpire::ExploredByEmpire(std::unique_ptr<ValueRef::ValueRef<int>>&& empire_id) :
    Condition(),
    m_empire_id(std::move(empire_id))
{
    m_root_candidate_invariant = !m_empire_id || m_empire_id->RootCandidateInvariant();
    m_target_invariant = !m_empire_id || m_empire_id->TargetInvariant();
    m_source_invariant = !m_empire_id || m_empire_id->SourceInvariant();
}

bool ExploredByEmpire::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const ExploredByEmpire& rhs_ = static_cast<const ExploredByEmpire&>(rhs);

    CHECK_COND_VREF_MEMBER(m_empire_id)

    return true;
}

namespace {
    struct ExploredByEmpireSimpleMatch {
        ExploredByEmpireSimpleMatch(int empire_id, const ScriptingContext& context) :
            m_empire_id(empire_id),
            m_context(context)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            auto empire = m_context.GetEmpire(m_empire_id);
            if (!empire)
                return false;
            return empire->HasExploredSystem(candidate->ID());
        }

        int                     m_empire_id = ALL_EMPIRES;
        const ScriptingContext& m_context;
    };
}

void ExploredByEmpire::Eval(const ScriptingContext& parent_context,
                            ObjectSet& matches, ObjectSet& non_matches,
                            SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = m_empire_id->ConstantExpr() ||
                            (m_empire_id->LocalCandidateInvariant() &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // evaluate empire id once, and use to check all candidate objects
        int empire_id = m_empire_id->Eval(parent_context);

        // need to check each candidate separately to test if it has been explored
        EvalImpl(matches, non_matches, search_domain, ExploredByEmpireSimpleMatch(empire_id, parent_context));
    } else {
        // re-evaluate empire id for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string ExploredByEmpire::Description(bool negated/* = false*/) const {
    std::string empire_str;
    if (m_empire_id) {
        int empire_id = ALL_EMPIRES;
        if (m_empire_id->ConstantExpr())
            empire_id = m_empire_id->Eval();
        if (const Empire* empire = GetEmpire(empire_id))
            empire_str = empire->Name();
        else
            empire_str = m_empire_id->Description();
    }

    return str(FlexibleFormat((!negated)
               ? UserString("DESC_EXPLORED_BY_EMPIRE")
               : UserString("DESC_EXPLORED_BY_EMPIRE_NOT"))
               % empire_str);
}

std::string ExploredByEmpire::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "ExploredByEmpire empire_id = " + m_empire_id->Dump(ntabs); }

bool ExploredByEmpire::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "ExploredByEmpire::Match passed no candidate object";
        return false;
    }

    return ExploredByEmpireSimpleMatch(m_empire_id->Eval(local_context), local_context)(candidate);
}

void ExploredByEmpire::SetTopLevelContent(const std::string& content_name) {
    if (m_empire_id)
        m_empire_id->SetTopLevelContent(content_name);
}

unsigned int ExploredByEmpire::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::ExploredByEmpire");
    CheckSums::CheckSumCombine(retval, m_empire_id);

    TraceLogger() << "GetCheckSum(ExploredByEmpire): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> ExploredByEmpire::Clone() const
{ return std::make_unique<ExploredByEmpire>(ValueRef::CloneUnique(m_empire_id)); }

///////////////////////////////////////////////////////////
// Stationary                                            //
///////////////////////////////////////////////////////////
Stationary::Stationary() :
    Condition()
{
    m_root_candidate_invariant = true;
    m_target_invariant = true;
    m_source_invariant = true;
}

bool Stationary::operator==(const Condition& rhs) const
{ return Condition::operator==(rhs); }

std::string Stationary::Description(bool negated/* = false*/) const {
    return (!negated)
        ? UserString("DESC_STATIONARY")
        : UserString("DESC_STATIONARY_NOT");
}

std::string Stationary::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "Stationary\n"; }

bool Stationary::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "Stationary::Match passed no candidate object";
        return false;
    }

    // the only objects that can move are fleets and the ships in them.  so,
    // attempt to cast the candidate object to a fleet or ship, and if it's a ship
    // get the fleet of that ship
    auto fleet = dynamic_cast<const Fleet*>(candidate.get());
    if (!fleet)
        if (auto ship = dynamic_cast<const Ship*>(candidate.get()))
            fleet = local_context.ContextObjects().get<Fleet>(ship->FleetID()).get();

    if (fleet) {
        // if a fleet is available, it is "moving", or not stationary, if it's
        // next system is a system and isn't the current system.  This will
        // mean fleets that have arrived at a system on the current turn will
        // be stationary, but fleets departing won't be stationary.
        int next_id = fleet->NextSystemID();
        int cur_id = fleet->SystemID();
        if (next_id != INVALID_OBJECT_ID && next_id != cur_id)
            return false;
    }

    return true;
}

unsigned int Stationary::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Stationary");

    TraceLogger() << "GetCheckSum(Stationary): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Stationary::Clone() const
{ return std::make_unique<Stationary>(); }

///////////////////////////////////////////////////////////
// Aggressive                                            //
///////////////////////////////////////////////////////////
Aggressive::Aggressive() :
    Aggressive(true)
{}

Aggressive::Aggressive(bool aggressive) :
    Condition(),
    m_aggressive(aggressive)
{
    m_root_candidate_invariant = true;
    m_target_invariant = true;
    m_source_invariant = true;
}

bool Aggressive::operator==(const Condition& rhs) const
{ return Condition::operator==(rhs); }

std::string Aggressive::Description(bool negated/* = false*/) const {
    if (m_aggressive)
        return (!negated)
            ? UserString("DESC_AGGRESSIVE")
            : UserString("DESC_AGGRESSIVE_NOT");
    else
        return (!negated)
            ? UserString("DESC_PASSIVE")
            : UserString("DESC_PASSIVE_NOT");
}

std::string Aggressive::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + (m_aggressive ? "Aggressive\n" : "Passive\n"); }

bool Aggressive::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "Aggressive::Match passed no candidate object";
        return false;
    }

    // the only objects that can be aggressive are fleets and the ships in them.
    // so, attempt to cast the candidate object to a fleet or ship, and if it's
    // a ship get the fleet of that ship
    const Fleet* fleet = nullptr;
    if (candidate->ObjectType() == UniverseObjectType::OBJ_FLEET) {
        fleet = static_cast<const Fleet*>(candidate.get());
    } else if (candidate->ObjectType() == UniverseObjectType::OBJ_SHIP) {
        auto* ship = static_cast<const Ship*>(candidate.get());
        fleet = local_context.ContextObjects().get<Fleet>(ship->FleetID()).get();
    }

    if (!fleet)
        return false;

    return m_aggressive == fleet->Aggressive();
}

unsigned int Aggressive::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Aggressive");
    CheckSums::CheckSumCombine(retval, m_aggressive);

    TraceLogger() << "GetCheckSum(Aggressive): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Aggressive::Clone() const
{ return std::make_unique<Aggressive>(m_aggressive); }

///////////////////////////////////////////////////////////
// FleetSupplyableByEmpire                               //
///////////////////////////////////////////////////////////
FleetSupplyableByEmpire::FleetSupplyableByEmpire(std::unique_ptr<ValueRef::ValueRef<int>>&& empire_id) :
    Condition(),
    m_empire_id(std::move(empire_id))
{
    m_root_candidate_invariant = !m_empire_id || m_empire_id->RootCandidateInvariant();
    m_target_invariant = !m_empire_id || m_empire_id->TargetInvariant();
    m_source_invariant = !m_empire_id || m_empire_id->SourceInvariant();
}

bool FleetSupplyableByEmpire::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const FleetSupplyableByEmpire& rhs_ = static_cast<const FleetSupplyableByEmpire&>(rhs);

    CHECK_COND_VREF_MEMBER(m_empire_id)

    return true;
}

namespace {
    struct FleetSupplyableSimpleMatch {
        FleetSupplyableSimpleMatch(int empire_id, const SupplyManager& supply) :
            m_empire_id(empire_id),
            m_supply(supply)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;

            const auto& empire_supplyable_systems = m_supply.FleetSupplyableSystemIDs();
            auto it = empire_supplyable_systems.find(m_empire_id);
            if (it == empire_supplyable_systems.end())
                return false;
            return it->second.count(candidate->SystemID());
        }

        int m_empire_id;
        const SupplyManager& m_supply;
    };
}

void FleetSupplyableByEmpire::Eval(const ScriptingContext& parent_context,
                                   ObjectSet& matches, ObjectSet& non_matches,
                                   SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = m_empire_id->ConstantExpr() ||
                            (m_empire_id->LocalCandidateInvariant() &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // evaluate empire id once, and use to check all candidate objects
        int empire_id = m_empire_id->Eval(parent_context);
        EvalImpl(matches, non_matches, search_domain, FleetSupplyableSimpleMatch(empire_id, parent_context.supply));
    } else {
        // re-evaluate empire id for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string FleetSupplyableByEmpire::Description(bool negated/* = false*/) const {
    std::string empire_str;
    if (m_empire_id) {
        int empire_id = ALL_EMPIRES;
        if (m_empire_id->ConstantExpr())
            empire_id = m_empire_id->Eval();
        if (const Empire* empire = GetEmpire(empire_id))
            empire_str = empire->Name();
        else
            empire_str = m_empire_id->Description();
    }

    return str(FlexibleFormat((!negated)
               ? UserString("DESC_SUPPLY_CONNECTED_FLEET")
               : UserString("DESC_SUPPLY_CONNECTED_FLEET_NOT"))
               % empire_str);
}

std::string FleetSupplyableByEmpire::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "ResupplyableBy empire_id = " + m_empire_id->Dump(ntabs); }

bool FleetSupplyableByEmpire::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "FleetSupplyableByEmpire::Match passed no candidate object";
        return false;
    }

    int empire_id = m_empire_id->Eval(local_context);

    return FleetSupplyableSimpleMatch(empire_id, local_context.supply)(candidate);
}

void FleetSupplyableByEmpire::SetTopLevelContent(const std::string& content_name) {
    if (m_empire_id)
        m_empire_id->SetTopLevelContent(content_name);
}

unsigned int FleetSupplyableByEmpire::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::FleetSupplyableByEmpire");
    CheckSums::CheckSumCombine(retval, m_empire_id);

    TraceLogger() << "GetCheckSum(FleetSupplyableByEmpire): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> FleetSupplyableByEmpire::Clone() const
{ return std::make_unique<FleetSupplyableByEmpire>(ValueRef::CloneUnique(m_empire_id)); }

///////////////////////////////////////////////////////////
// ResourceSupplyConnectedByEmpire                       //
///////////////////////////////////////////////////////////
ResourceSupplyConnectedByEmpire::ResourceSupplyConnectedByEmpire(
    std::unique_ptr<ValueRef::ValueRef<int>>&& empire_id,
    std::unique_ptr<Condition>&& condition) :
    Condition(),
    m_empire_id(std::move(empire_id)),
    m_condition(std::move(condition))
{
    m_root_candidate_invariant =
        (!m_empire_id || m_empire_id->RootCandidateInvariant()) &&
        (!m_condition || m_condition->RootCandidateInvariant());
    m_target_invariant =
        (!m_empire_id || m_empire_id->TargetInvariant()) &&
        (!m_condition || m_condition->TargetInvariant());
    m_source_invariant =
        (!m_empire_id || m_empire_id->SourceInvariant()) &&
        (!m_condition || m_condition->SourceInvariant());
}

bool ResourceSupplyConnectedByEmpire::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const ResourceSupplyConnectedByEmpire& rhs_ = static_cast<const ResourceSupplyConnectedByEmpire&>(rhs);

    CHECK_COND_VREF_MEMBER(m_empire_id)

    return true;
}

namespace {
    struct ResourceSupplySimpleMatch {
        ResourceSupplySimpleMatch(int empire_id, const ObjectSet& from_objects,
                                  const ObjectMap& objects, const SupplyManager& supply) :
            m_empire_id(empire_id),
            m_from_objects(from_objects),
            m_objects(objects),
            m_supply(supply)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;
            if (m_from_objects.empty())
                return false;
            const auto& groups = m_supply.ResourceSupplyGroups(m_empire_id);
            if (groups.empty())
                return false;

            // is candidate object connected to a subcondition matching object by resource supply?
            // first check if candidate object is (or is a building on) a blockaded planet
            // "isolated" objects are anything not in a non-blockaded system
            bool is_isolated = true;
            for (const auto& group : groups) {
                if (group.count(candidate->SystemID())) {
                    is_isolated = false;
                    break;
                }
            }
            if (is_isolated) {
                // planets are still supply-connected to themselves even if blockaded
                auto candidate_planet = std::dynamic_pointer_cast<const Planet>(candidate);
                std::shared_ptr<const ::Building> building;
                if (!candidate_planet && (building = std::dynamic_pointer_cast<const ::Building>(candidate)))
                    candidate_planet = m_objects.get<Planet>(building->PlanetID());
                if (candidate_planet) {
                    int candidate_planet_id = candidate_planet->ID();
                    // can only match if the from_object is (or is on) the same planet
                    for (auto& from_object : m_from_objects) {
                        auto from_obj_planet = std::dynamic_pointer_cast<const Planet>(from_object);
                        std::shared_ptr<const ::Building> from_building;
                        if (!from_obj_planet && (from_building = std::dynamic_pointer_cast<const ::Building>(from_object)))
                            from_obj_planet = m_objects.get<Planet>(from_building->PlanetID());
                        if (from_obj_planet && from_obj_planet->ID() == candidate_planet_id)
                            return true;
                    }
                }
                // candidate is isolated, but did not match planet for any test object
                return false;
            }
            // candidate is not blockaded, so check for system group matches
            for (auto& from_object : m_from_objects) {
                for (const auto& group : groups) {
                    if (group.count(from_object->SystemID())) {
                        // found resource sharing group containing test object system.  Does it also contain candidate?
                        if (group.count(candidate->SystemID()))
                            return true;    // test object and candidate object are in same resourse sharing group
                        else
                            // test object is not in resource sharing group with candidate
                            // as each object can be in only one group, no need to check the remaining groups
                            break;
                    }
                    // current subcondition-matching object is not in this resource sharing group
                }
                // current subcondition-matching object is not in any resource sharing group for this empire
            }

            return false;
        }

        int m_empire_id;
        const ObjectSet& m_from_objects;
        const ObjectMap& m_objects;
        const SupplyManager& m_supply;
    };
}

void ResourceSupplyConnectedByEmpire::Eval(const ScriptingContext& parent_context,
                                           ObjectSet& matches, ObjectSet& non_matches,
                                           SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = m_empire_id->ConstantExpr() ||
                            (m_empire_id->LocalCandidateInvariant() &&
                            (parent_context.condition_root_candidate || RootCandidateInvariant()));
    if (simple_eval_safe) {
        // evaluate contained objects once and check for all candidates

        // get objects to be considering for matching against subcondition
        ObjectSet subcondition_matches;
        m_condition->Eval(parent_context, subcondition_matches);
        int empire_id = m_empire_id->Eval(parent_context);

        EvalImpl(matches, non_matches, search_domain,
                 ResourceSupplySimpleMatch(empire_id, subcondition_matches, parent_context.ContextObjects(),
                                           parent_context.supply));
    } else {
        // re-evaluate empire id for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

bool ResourceSupplyConnectedByEmpire::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "ResourceSupplyConnectedByEmpire::Match passed no candidate object";
        return false;
    }

    // get subcondition matches
    ObjectSet subcondition_matches;
    m_condition->Eval(local_context, subcondition_matches);
    int empire_id = m_empire_id->Eval(local_context);

    return ResourceSupplySimpleMatch(empire_id, subcondition_matches, local_context.ContextObjects(),
                                     local_context.supply)(candidate);
}

std::string ResourceSupplyConnectedByEmpire::Description(bool negated/* = false*/) const {
    std::string empire_str;
    if (m_empire_id) {
        int empire_id = ALL_EMPIRES;
        if (m_empire_id->ConstantExpr())
            empire_id = m_empire_id->Eval();
        if (const Empire* empire = GetEmpire(empire_id))
            empire_str = empire->Name();
        else
            empire_str = m_empire_id->Description();
    }

    return str(FlexibleFormat((!negated)
               ? UserString("DESC_SUPPLY_CONNECTED_RESOURCE")
               : UserString("DESC_SUPPLY_CONNECTED_RESOURCE_NOT"))
               % empire_str
               % m_condition->Description());
}

std::string ResourceSupplyConnectedByEmpire::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "ResourceSupplyConnectedBy empire_id = "
        + m_empire_id->Dump(ntabs) + " condition = \n";
    retval += m_condition->Dump(ntabs+1);
    return retval;
}

void ResourceSupplyConnectedByEmpire::SetTopLevelContent(const std::string& content_name) {
    if (m_empire_id)
        m_empire_id->SetTopLevelContent(content_name);
    if (m_condition)
        m_condition->SetTopLevelContent(content_name);
}

unsigned int ResourceSupplyConnectedByEmpire::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::ResourceSupplyConnectedByEmpire");
    CheckSums::CheckSumCombine(retval, m_empire_id);
    CheckSums::CheckSumCombine(retval, m_condition);

    TraceLogger() << "GetCheckSum(ResourceSupplyConnectedByEmpire): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> ResourceSupplyConnectedByEmpire::Clone() const {
    return std::make_unique<ResourceSupplyConnectedByEmpire>(ValueRef::CloneUnique(m_empire_id),
                                                             ValueRef::CloneUnique(m_condition));
}

///////////////////////////////////////////////////////////
// CanColonize                                           //
///////////////////////////////////////////////////////////
CanColonize::CanColonize() :
    Condition()
{
    m_root_candidate_invariant = true;
    m_target_invariant = true;
    m_source_invariant = true;
}

bool CanColonize::operator==(const Condition& rhs) const
{ return Condition::operator==(rhs); }

std::string CanColonize::Description(bool negated/* = false*/) const {
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_CAN_COLONIZE")
        : UserString("DESC_CAN_COLONIZE_NOT")));
}

std::string CanColonize::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "CanColonize\n"; }

bool CanColonize::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "CanColonize::Match passed no candidate object";
        return false;
    }

    // is it a ship, a planet, or a building on a planet?
    std::string species_name;
    if (candidate->ObjectType() == UniverseObjectType::OBJ_PLANET) {
        auto planet = std::dynamic_pointer_cast<const Planet>(candidate);
        if (!planet) {
            ErrorLogger() << "CanColonize couldn't cast supposedly planet candidate";
            return false;
        }
        species_name = planet->SpeciesName();

    } else if (candidate->ObjectType() == UniverseObjectType::OBJ_BUILDING) {
        auto building = std::dynamic_pointer_cast<const ::Building>(candidate);
        if (!building) {
            ErrorLogger() << "CanColonize couldn't cast supposedly building candidate";
            return false;
        }
        auto planet = local_context.ContextObjects().get<Planet>(building->PlanetID());
        if (!planet) {
            ErrorLogger() << "CanColonize couldn't get building's planet";
            return false;
        }
        species_name = planet->SpeciesName();

    } else if (candidate->ObjectType() == UniverseObjectType::OBJ_SHIP) {
        auto ship = std::dynamic_pointer_cast<const Ship>(candidate);
        if (!ship) {
            ErrorLogger() << "CanColonize couldn't cast supposedly ship candidate";
            return false;
        }
        species_name = ship->SpeciesName();
    }

    if (species_name.empty())
        return false;
    auto species = GetSpecies(species_name);
    if (!species) {
        ErrorLogger() << "CanColonize couldn't get species: " << species_name;
        return false;
    }
    return species->CanColonize();
}

unsigned int CanColonize::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::CanColonize");

    TraceLogger() << "GetCheckSum(CanColonize): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> CanColonize::Clone() const
{ return std::make_unique<CanColonize>(); }

///////////////////////////////////////////////////////////
// CanProduceShips                                       //
///////////////////////////////////////////////////////////
CanProduceShips::CanProduceShips() :
    Condition()
{
    m_root_candidate_invariant = true;
    m_target_invariant = true;
    m_source_invariant = true;
}

bool CanProduceShips::operator==(const Condition& rhs) const
{ return Condition::operator==(rhs); }

std::string CanProduceShips::Description(bool negated/* = false*/) const {
    return str(FlexibleFormat((!negated)
        ? UserString("DESC_CAN_PRODUCE_SHIPS")
        : UserString("DESC_CAN_PRODUCE_SHIPS_NOT")));
}

std::string CanProduceShips::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "CanColonize\n"; }

bool CanProduceShips::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "CanProduceShips::Match passed no candidate object";
        return false;
    }

    // is it a ship, a planet, or a building on a planet?
    std::string species_name;
    if (candidate->ObjectType() == UniverseObjectType::OBJ_PLANET) {
        auto planet = std::dynamic_pointer_cast<const Planet>(candidate);
        if (!planet) {
            ErrorLogger() << "CanProduceShips couldn't cast supposedly planet candidate";
            return false;
        }
        species_name = planet->SpeciesName();

    } else if (candidate->ObjectType() == UniverseObjectType::OBJ_BUILDING) {
        auto building = std::dynamic_pointer_cast<const ::Building>(candidate);
        if (!building) {
            ErrorLogger() << "CanProduceShips couldn't cast supposedly building candidate";
            return false;
        }
        auto planet = local_context.ContextObjects().get<Planet>(building->PlanetID());
        if (!planet) {
            ErrorLogger() << "CanProduceShips couldn't get building's planet";
            return false;
        }
        species_name = planet->SpeciesName();

    } else if (candidate->ObjectType() == UniverseObjectType::OBJ_SHIP) {
        auto ship = std::dynamic_pointer_cast<const Ship>(candidate);
        if (!ship) {
            ErrorLogger() << "CanProduceShips couldn't cast supposedly ship candidate";
            return false;
        }
        species_name = ship->SpeciesName();
    }

    if (species_name.empty())
        return false;
    auto species = GetSpecies(species_name);
    if (!species) {
        ErrorLogger() << "CanProduceShips couldn't get species: " << species_name;
        return false;
    }
    return species->CanProduceShips();
}

unsigned int CanProduceShips::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::CanProduceShips");

    TraceLogger() << "GetCheckSum(CanProduceShips): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> CanProduceShips::Clone() const
{ return std::make_unique<CanProduceShips>(); }

///////////////////////////////////////////////////////////
// OrderedBombarded                                      //
///////////////////////////////////////////////////////////
OrderedBombarded::OrderedBombarded(std::unique_ptr<Condition>&& by_object_condition) :
    Condition(),
    m_by_object_condition(std::move(by_object_condition))
{
    m_root_candidate_invariant = !m_by_object_condition || m_by_object_condition->RootCandidateInvariant();
    m_target_invariant = !m_by_object_condition || m_by_object_condition->TargetInvariant();
    m_source_invariant = !m_by_object_condition || m_by_object_condition->SourceInvariant();
}

bool OrderedBombarded::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const OrderedBombarded& rhs_ = static_cast<const OrderedBombarded&>(rhs);

    CHECK_COND_VREF_MEMBER(m_by_object_condition)

    return true;
}

namespace {
    struct OrderedBombardedSimpleMatch {
        OrderedBombardedSimpleMatch(const ObjectSet& by_objects) :
            m_by_objects(by_objects)
        {}

        bool operator()(const std::shared_ptr<const UniverseObject>& candidate) const {
            if (!candidate)
                return false;
            if (m_by_objects.empty())
                return false;
            auto planet = std::dynamic_pointer_cast<const Planet>(candidate);
            if (!planet)
                return false;
            int planet_id = planet->ID();
            if (planet_id == INVALID_OBJECT_ID)
                return false;

            // check if any of the by_objects is ordered to bombard the candidate planet
            for (auto& obj : m_by_objects) {
                auto ship = std::dynamic_pointer_cast<const Ship>(obj);
                if (!ship)
                    continue;
                if (ship->OrderedBombardPlanet() == planet_id)
                    return true;
            }
            return false;
        }

        const ObjectSet& m_by_objects;
    };
}

void OrderedBombarded::Eval(const ScriptingContext& parent_context,
                            ObjectSet& matches, ObjectSet& non_matches,
                            SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = parent_context.condition_root_candidate || RootCandidateInvariant();
    if (simple_eval_safe) {
        // evaluate contained objects once and check for all candidates

        // get subcondition matches
        ObjectSet subcondition_matches;
        m_by_object_condition->Eval(parent_context, subcondition_matches);

        EvalImpl(matches, non_matches, search_domain, OrderedBombardedSimpleMatch(subcondition_matches));
    } else {
        // re-evaluate contained objects for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string OrderedBombarded::Description(bool negated/* = false*/) const {
    std::string by_str;
    if (m_by_object_condition)
        by_str = m_by_object_condition->Description();

    return str(FlexibleFormat((!negated)
               ? UserString("DESC_ORDERED_BOMBARDED")
               : UserString("DESC_ORDERED_BOMBARDED_NOT"))
               % by_str);
}

std::string OrderedBombarded::Dump(unsigned short ntabs) const
{ return DumpIndent(ntabs) + "OrderedBombarded object = " + m_by_object_condition->Dump(ntabs); }

bool OrderedBombarded::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "OrderedBombarded::Match passed no candidate object";
        return false;
    }

    // get subcondition matches
    ObjectSet subcondition_matches;
    m_by_object_condition->Eval(local_context, subcondition_matches);

    return OrderedBombardedSimpleMatch(subcondition_matches)(candidate);
}

void OrderedBombarded::SetTopLevelContent(const std::string& content_name) {
    if (m_by_object_condition)
        m_by_object_condition->SetTopLevelContent(content_name);
}

unsigned int OrderedBombarded::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::OrderedBombarded");
    CheckSums::CheckSumCombine(retval, m_by_object_condition);

    TraceLogger() << "GetCheckSum(OrderedBombarded): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> OrderedBombarded::Clone() const
{ return std::make_unique<OrderedBombarded>(ValueRef::CloneUnique(m_by_object_condition)); }

///////////////////////////////////////////////////////////
// ValueTest                                             //
///////////////////////////////////////////////////////////
namespace {
    bool Comparison(float val1, ComparisonType comp, float val2) {
        switch (comp) {
            case ComparisonType::EQUAL:                 return val1 == val2;
            case ComparisonType::GREATER_THAN:          return val1 > val2;
            case ComparisonType::GREATER_THAN_OR_EQUAL: return val1 >= val2;
            case ComparisonType::LESS_THAN:             return val1 < val2;
            case ComparisonType::LESS_THAN_OR_EQUAL:    return val1 <= val2;
            case ComparisonType::NOT_EQUAL:             return val1 != val2;
            case ComparisonType::INVALID_COMPARISON:
            default:                                    return false;
        }
    }
    bool Comparison(const std::string& val1, ComparisonType comp,
                    const std::string& val2)
    {
        switch (comp) {
            case ComparisonType::EQUAL:              return val1 == val2;
            case ComparisonType::NOT_EQUAL:          return val1 != val2;
            case ComparisonType::INVALID_COMPARISON:
            default:                                 return false;
        }
    }

    std::string CompareTypeString(ComparisonType comp) {
        switch (comp) {
        case ComparisonType::EQUAL:                 return "=";
        case ComparisonType::GREATER_THAN:          return ">";
        case ComparisonType::GREATER_THAN_OR_EQUAL: return ">=";
        case ComparisonType::LESS_THAN:             return "<";
        case ComparisonType::LESS_THAN_OR_EQUAL:    return "<=";
        case ComparisonType::NOT_EQUAL:             return "!=";
        case ComparisonType::INVALID_COMPARISON:
        default:                                    return "";
        }
    }
}

ValueTest::ValueTest(std::unique_ptr<ValueRef::ValueRef<double>>&& value_ref1,
                     ComparisonType comp1,
                     std::unique_ptr<ValueRef::ValueRef<double>>&& value_ref2,
                     ComparisonType comp2,
                     std::unique_ptr<ValueRef::ValueRef<double>>&& value_ref3) :
    Condition(),
    m_value_ref1(std::move(value_ref1)),
    m_value_ref2(std::move(value_ref2)),
    m_value_ref3(std::move(value_ref3)),
    m_compare_type1(comp1),
    m_compare_type2(comp2)
{
    auto operands = {m_value_ref1.get(), m_value_ref2.get(), m_value_ref3.get()};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

ValueTest::ValueTest(std::unique_ptr<ValueRef::ValueRef<std::string>>&& value_ref1,
                     ComparisonType comp1,
                     std::unique_ptr<ValueRef::ValueRef<std::string>>&& value_ref2,
                     ComparisonType comp2,
                     std::unique_ptr<ValueRef::ValueRef<std::string>>&& value_ref3) :
    Condition(),
    m_string_value_ref1(std::move(value_ref1)),
    m_string_value_ref2(std::move(value_ref2)),
    m_string_value_ref3(std::move(value_ref3)),
    m_compare_type1(comp1),
    m_compare_type2(comp2)
{
    auto operands = {m_string_value_ref1.get(), m_string_value_ref2.get(), m_string_value_ref3.get()};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

ValueTest::ValueTest(std::unique_ptr<ValueRef::ValueRef<int>>&& value_ref1,
                     ComparisonType comp1,
                     std::unique_ptr<ValueRef::ValueRef<int>>&& value_ref2,
                     ComparisonType comp2,
                     std::unique_ptr<ValueRef::ValueRef<int>>&& value_ref3) :
    Condition(),
    m_int_value_ref1(std::move(value_ref1)),
    m_int_value_ref2(std::move(value_ref2)),
    m_int_value_ref3(std::move(value_ref3)),
    m_compare_type1(comp1),
    m_compare_type2(comp2)
{
    auto operands = {m_int_value_ref1.get(), m_int_value_ref2.get(), m_int_value_ref3.get()};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

ValueTest::ValueTest(const ValueTest& rhs) :
    Condition(rhs),
    m_value_ref1(ValueRef::CloneUnique(rhs.m_value_ref1)),
    m_value_ref2(ValueRef::CloneUnique(rhs.m_value_ref2)),
    m_value_ref3(ValueRef::CloneUnique(rhs.m_value_ref3)),
    m_string_value_ref1(ValueRef::CloneUnique(rhs.m_string_value_ref1)),
    m_string_value_ref2(ValueRef::CloneUnique(rhs.m_string_value_ref2)),
    m_string_value_ref3(ValueRef::CloneUnique(rhs.m_string_value_ref3)),
    m_int_value_ref1(ValueRef::CloneUnique(rhs.m_int_value_ref1)),
    m_int_value_ref2(ValueRef::CloneUnique(rhs.m_int_value_ref2)),
    m_int_value_ref3(ValueRef::CloneUnique(rhs.m_int_value_ref3)),
    m_compare_type1(rhs.m_compare_type1),
    m_compare_type2(rhs.m_compare_type2)
{
}

bool ValueTest::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const ValueTest& rhs_ = static_cast<const ValueTest&>(rhs);

    CHECK_COND_VREF_MEMBER(m_value_ref1)
    CHECK_COND_VREF_MEMBER(m_value_ref2)
    CHECK_COND_VREF_MEMBER(m_value_ref3)
    CHECK_COND_VREF_MEMBER(m_string_value_ref1)
    CHECK_COND_VREF_MEMBER(m_string_value_ref2)
    CHECK_COND_VREF_MEMBER(m_string_value_ref3)
    CHECK_COND_VREF_MEMBER(m_int_value_ref1)
    CHECK_COND_VREF_MEMBER(m_int_value_ref2)
    CHECK_COND_VREF_MEMBER(m_int_value_ref3)

    if (m_compare_type1 != rhs_.m_compare_type1)
        return false;
    if (m_compare_type2 != rhs_.m_compare_type2)
        return false;

    return true;
}

void ValueTest::Eval(const ScriptingContext& parent_context,
                     ObjectSet& matches, ObjectSet& non_matches,
                     SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = ((!m_value_ref1         || m_value_ref1->LocalCandidateInvariant()) &&
                             (!m_value_ref2         || m_value_ref2->LocalCandidateInvariant()) &&
                             (!m_value_ref3         || m_value_ref3->LocalCandidateInvariant()) &&
                             (!m_string_value_ref1  || m_string_value_ref1->LocalCandidateInvariant()) &&
                             (!m_string_value_ref2  || m_string_value_ref2->LocalCandidateInvariant()) &&
                             (!m_string_value_ref3  || m_string_value_ref3->LocalCandidateInvariant()) &&
                             (!m_int_value_ref1     || m_int_value_ref1->LocalCandidateInvariant()) &&
                             (!m_int_value_ref2     || m_int_value_ref2->LocalCandidateInvariant()) &&
                             (!m_int_value_ref3     || m_int_value_ref3->LocalCandidateInvariant()) &&
                             (parent_context.condition_root_candidate || RootCandidateInvariant()));

    if (simple_eval_safe) {
        // evaluate value and range limits once, use to match all candidates
        bool match = Match(parent_context);

        // transfer objects to or from candidate set, according to whether the value comparisons were true
        if (search_domain == SearchDomain::MATCHES && !match) {
            // move all objects from matches to non_matches
            std::move(matches.begin(), matches.end(), std::back_inserter(non_matches));
            matches.clear();
        } else if (search_domain == SearchDomain::NON_MATCHES && match) {
            // move all objects from non_matches to matches
            std::move(non_matches.begin(), non_matches.end(), std::back_inserter(matches));
            non_matches.clear();
        }

    } else {
        // re-evaluate value and ranges for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string ValueTest::Description(bool negated/* = false*/) const {
    std::string value_str1, value_str2, value_str3;
    if (m_value_ref1)
        value_str1 = m_value_ref1->Description();
    else if (m_string_value_ref1)
        value_str1 = m_string_value_ref1->Description();
    else if (m_int_value_ref1)
        value_str1 = m_int_value_ref1->Description();

    if (m_value_ref2)
        value_str2 = m_value_ref2->Description();
    else if (m_string_value_ref2)
        value_str2 = m_string_value_ref2->Description();
    else if (m_int_value_ref2)
        value_str2 = m_int_value_ref2->Description();

    if (m_value_ref3)
        value_str3 = m_value_ref3->Description();
    else if (m_string_value_ref3)
        value_str3 = m_string_value_ref3->Description();
    else if (m_int_value_ref3)
        value_str3 = m_int_value_ref3->Description();

    std::string comp_str1 = CompareTypeString(m_compare_type1);
    std::string comp_str2 = CompareTypeString(m_compare_type2);

    std::string composed_comparison = value_str1 + " " + comp_str1 + " " + value_str2;
    if (!comp_str2.empty())
        composed_comparison += " " + comp_str2;
    if (!value_str3.empty())
        composed_comparison += +" " + value_str3;

    return str(FlexibleFormat((!negated)
               ? UserString("DESC_VALUE_TEST")
               : UserString("DESC_VALUE_TEST_NOT"))
               % composed_comparison);
}

std::string ValueTest::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "(";
    if (m_value_ref1)
        retval += m_value_ref1->Dump(ntabs);
    else if (m_string_value_ref1)
        retval += m_string_value_ref1->Dump(ntabs);
    else if (m_int_value_ref1)
        retval += m_int_value_ref1->Dump(ntabs);

    if (m_compare_type1 != ComparisonType::INVALID_COMPARISON)
        retval += " " + CompareTypeString(m_compare_type1);

    if (m_value_ref2)
        retval += " " + m_value_ref2->Dump(ntabs);
    else if (m_string_value_ref2)
        retval += " " + m_string_value_ref2->Dump(ntabs);
    else if (m_int_value_ref2)
        retval += " " + m_int_value_ref2->Dump(ntabs);

    if (m_compare_type2 != ComparisonType::INVALID_COMPARISON)
        retval += " " + CompareTypeString(m_compare_type2);

    if (m_value_ref3)
        retval += " " + m_value_ref3->Dump(ntabs);
    else if (m_string_value_ref3)
        retval += " " + m_string_value_ref3->Dump(ntabs);
    else if (m_int_value_ref3)
        retval += " " + m_int_value_ref3->Dump(ntabs);

    retval += ")\n";
    return retval;
}

bool ValueTest::Match(const ScriptingContext& local_context) const {
    if (m_compare_type1 == ComparisonType::INVALID_COMPARISON)
        return false;

    // simple evaluation should have only local-candidate-invariation sub-value-refs
    // base class evaulation should have defined local candidate

    if (m_value_ref1) {
        if (!m_value_ref2)
            return false;

        float val1 = m_value_ref1->Eval(local_context);
        float val2 = m_value_ref2->Eval(local_context);
        if (!Comparison(val1, m_compare_type1, val2))
            return false;

        if (m_compare_type2 == ComparisonType::INVALID_COMPARISON || !m_value_ref3)
            return true;

        float val3 = m_value_ref3->Eval(local_context);
        return Comparison(val2, m_compare_type1, val3);

    } else if (m_string_value_ref1) {
        if (!m_string_value_ref2)
            return false;

        std::string val1 = m_string_value_ref1->Eval(local_context);
        std::string val2 = m_string_value_ref2->Eval(local_context);
        if (!Comparison(val1, m_compare_type1, val2))
            return false;

        if (m_compare_type2 == ComparisonType::INVALID_COMPARISON || !m_value_ref3)
            return true;

        std::string val3 = m_string_value_ref3->Eval(local_context);
        return Comparison(val2, m_compare_type1, val3);

    } else if (m_int_value_ref1) {
        if (!m_int_value_ref2)
            return false;

        int val1 = m_int_value_ref1->Eval(local_context);
        int val2 = m_int_value_ref2->Eval(local_context);
        if (!Comparison(val1, m_compare_type1, val2))
            return false;

        if (m_compare_type2 == ComparisonType::INVALID_COMPARISON || !m_value_ref3)
            return true;

        int val3 = m_int_value_ref3->Eval(local_context);
        return Comparison(val2, m_compare_type1, val3);
    }

    return false;
}

void ValueTest::SetTopLevelContent(const std::string& content_name) {
    if (m_value_ref1)
        m_value_ref1->SetTopLevelContent(content_name);
    if (m_value_ref2)
        m_value_ref2->SetTopLevelContent(content_name);
    if (m_value_ref3)
        m_value_ref3->SetTopLevelContent(content_name);
    if (m_string_value_ref1)
        m_string_value_ref1->SetTopLevelContent(content_name);
    if (m_string_value_ref2)
        m_string_value_ref2->SetTopLevelContent(content_name);
    if (m_string_value_ref3)
        m_string_value_ref3->SetTopLevelContent(content_name);
    if (m_int_value_ref1)
        m_int_value_ref1->SetTopLevelContent(content_name);
    if (m_int_value_ref2)
        m_int_value_ref2->SetTopLevelContent(content_name);
    if (m_int_value_ref3)
        m_int_value_ref3->SetTopLevelContent(content_name);
}

unsigned int ValueTest::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::ValueTest");
    CheckSums::CheckSumCombine(retval, m_value_ref1);
    CheckSums::CheckSumCombine(retval, m_value_ref2);
    CheckSums::CheckSumCombine(retval, m_value_ref3);
    CheckSums::CheckSumCombine(retval, m_string_value_ref1);
    CheckSums::CheckSumCombine(retval, m_string_value_ref2);
    CheckSums::CheckSumCombine(retval, m_string_value_ref3);
    CheckSums::CheckSumCombine(retval, m_int_value_ref1);
    CheckSums::CheckSumCombine(retval, m_int_value_ref2);
    CheckSums::CheckSumCombine(retval, m_int_value_ref3);
    CheckSums::CheckSumCombine(retval, m_compare_type1);
    CheckSums::CheckSumCombine(retval, m_compare_type2);

    TraceLogger() << "GetCheckSum(ValueTest): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> ValueTest::Clone() const
{ return std::make_unique<ValueTest>(*this); }

///////////////////////////////////////////////////////////
// Location                                              //
///////////////////////////////////////////////////////////
namespace {
    [[nodiscard]] const Condition* GetLocationCondition(ContentType content_type,
                                                        const std::string& name1,
                                                        const std::string& name2)
    {
        if (name1.empty())
            return nullptr;
        switch (content_type) {
        case ContentType::CONTENT_BUILDING: {
            if (auto bt = GetBuildingType(name1))
                return bt->Location();
            break;
        }
        case ContentType::CONTENT_SPECIES: {
            if (auto s = GetSpecies(name1))
                return s->Location();
            break;
        }
        case ContentType::CONTENT_SHIP_HULL: {
            if (auto h = GetShipHull(name1))
                return h->Location();
            break;
        }
        case ContentType::CONTENT_SHIP_PART: {
            if (auto p = GetShipPart(name1))
                return p->Location();
            break;
        }
        case ContentType::CONTENT_SPECIAL: {
            if (auto s = GetSpecial(name1))
                return s->Location();
            break;
        }
        case ContentType::CONTENT_FOCUS: {
            if (name2.empty())
                return nullptr;
            // get species, then focus from that species
            if (auto s = GetSpecies(name1)) {
                for (auto& f : s->Foci()) {
                    if (f.Name() == name2)
                        return f.Location();
                }
            }
            break;
        }
        default:
            return nullptr;
        }
        return nullptr;
    }

    [[nodiscard]] const std::string& GetContentTypeName(ContentType content_type) {
        switch (content_type) {
        case ContentType::CONTENT_BUILDING:  return UserString("UIT_BUILDING");          break;
        case ContentType::CONTENT_SPECIES:   return UserString("ENC_SPECIES");           break;
        case ContentType::CONTENT_SHIP_HULL: return UserString("UIT_SHIP_HULL");         break;
        case ContentType::CONTENT_SHIP_PART: return UserString("UIT_SHIP_PART");         break;
        case ContentType::CONTENT_SPECIAL:   return UserString("ENC_SPECIAL");           break;
        case ContentType::CONTENT_FOCUS:     return UserString("PLANETARY_FOCUS_TITLE"); break;
        default:                             return EMPTY_STRING;                        break;
        }
    }
}

Location::Location(ContentType content_type,
                   std::unique_ptr<ValueRef::ValueRef<std::string>>&& name1,
                   std::unique_ptr<ValueRef::ValueRef<std::string>>&& name2) :
    Condition(),
    m_name1(std::move(name1)),
    m_name2(std::move(name2)),
    m_content_type(content_type)
{
    std::array<const ValueRef::ValueRefBase*, 2> operands = {{m_name1.get(), m_name2.get()}};
    m_root_candidate_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

bool Location::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const Location& rhs_ = static_cast<const Location&>(rhs);

    if (m_content_type != rhs_.m_content_type)
        return false;

    CHECK_COND_VREF_MEMBER(m_name1)
    CHECK_COND_VREF_MEMBER(m_name2)

    return true;
}

void Location::Eval(const ScriptingContext& parent_context,
                    ObjectSet& matches, ObjectSet& non_matches,
                    SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = ((!m_name1 || m_name1->LocalCandidateInvariant()) &&
                             (!m_name2 || m_name2->LocalCandidateInvariant()) &&
                             (parent_context.condition_root_candidate || RootCandidateInvariant()));

    if (simple_eval_safe) {
        // evaluate value and range limits once, use to match all candidates

        std::string name1 = (m_name1 ? m_name1->Eval(parent_context) : "");
        std::string name2 = (m_name2 ? m_name2->Eval(parent_context) : "");

        // get condition from content, apply to matches / non_matches
        const auto condition = GetLocationCondition(m_content_type, name1, name2);
        if (condition && condition != this) {
            condition->Eval(parent_context, matches, non_matches, search_domain);
        } else {
            // if somehow in a cyclical loop because some content's location
            // was defined as Location or if there is no location
            // condition, match nothing
            if (search_domain == SearchDomain::MATCHES) {
                // move all objects from matches to non_matches
                std::move(matches.begin(), matches.end(), std::back_inserter(non_matches));
                matches.clear();
            }
        }

    } else {
        // re-evaluate value and ranges for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string Location::Description(bool negated/* = false*/) const {
    std::string name1_str;
    if (m_name1)
        name1_str = m_name1->Description();

    std::string name2_str;
    if (m_name2)
        name2_str = m_name2->Description();

    std::string content_type_str = GetContentTypeName(m_content_type);
    std::string name_str = (m_content_type == ContentType::CONTENT_FOCUS ? name2_str : name1_str);

    return str(FlexibleFormat((!negated)
               ? UserString("DESC_LOCATION")
               : UserString("DESC_LOCATION_NOT"))
               % content_type_str
               % name_str);
}

std::string Location::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "Location content_type = ";

    switch (m_content_type) {
    case ContentType::CONTENT_BUILDING:  retval += "Building";   break;
    case ContentType::CONTENT_FOCUS:     retval += "Focus";      break;
    case ContentType::CONTENT_SHIP_HULL: retval += "Hull";       break;
    case ContentType::CONTENT_SHIP_PART: retval += "Part";       break;
    case ContentType::CONTENT_SPECIAL:   retval += "Special";    break;
    case ContentType::CONTENT_SPECIES:   retval += "Species";    break;
    default:                             retval += "???";
    }

    if (m_name1)
        retval += " name1 = " + m_name1->Dump(ntabs);
    if (m_name2)
        retval += " name2 = " + m_name2->Dump(ntabs);
    return retval;
}

bool Location::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "Location::Match passed no candidate object";
        return false;
    }

    std::string name1 = (m_name1 ? m_name1->Eval(local_context) : "");
    std::string name2 = (m_name2 ? m_name2->Eval(local_context) : "");

    const auto condition = GetLocationCondition(m_content_type, name1, name2);
    if (!condition || condition == this)
        return false;

    // other Conditions' Match functions not directly callable, so can't do any
    // better than just calling Eval for each candidate...
    return condition->Eval(local_context, candidate);
}

void Location::SetTopLevelContent(const std::string& content_name) {
    if (m_name1)
        m_name1->SetTopLevelContent(content_name);
    if (m_name2)
        m_name2->SetTopLevelContent(content_name);
}

unsigned int Location::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Location");
    CheckSums::CheckSumCombine(retval, m_name1);
    CheckSums::CheckSumCombine(retval, m_name2);
    CheckSums::CheckSumCombine(retval, m_content_type);

    TraceLogger() << "GetCheckSum(Location): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Location::Clone() const {
    return std::make_unique<Location>(m_content_type,
                                      CloneUnique(m_name1),
                                      CloneUnique(m_name2));
}

///////////////////////////////////////////////////////////
// CombatTarget                                          //
///////////////////////////////////////////////////////////
namespace {
    const Condition* GetCombatTargetCondition(
        ContentType content_type, const std::string& name)
    {
        if (name.empty())
            return nullptr;
        switch (content_type) {
        case ContentType::CONTENT_SPECIES: {
            if (auto s = GetSpecies(name))
                return s->CombatTargets();
            break;
        }
        case ContentType::CONTENT_SHIP_PART: {
            if (auto p = GetShipPart(name))
                return p->CombatTargets();
            break;
        }
        default:
            return nullptr;
        }
        return nullptr;
    }
}

CombatTarget::CombatTarget(ContentType content_type,
                           std::unique_ptr<ValueRef::ValueRef<std::string>>&& name) :
    Condition(),
    m_name(std::move(name)),
    m_content_type(content_type)
{
    m_root_candidate_invariant = !m_name || m_name->RootCandidateInvariant();
    m_target_invariant = !m_name|| m_name->TargetInvariant();
    m_source_invariant = !m_name || m_name->SourceInvariant();
}

bool CombatTarget::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const CombatTarget& rhs_ = static_cast<const CombatTarget&>(rhs);

    if (m_content_type != rhs_.m_content_type)
        return false;

    CHECK_COND_VREF_MEMBER(m_name)

    return true;
}

void CombatTarget::Eval(const ScriptingContext& parent_context,
                        ObjectSet& matches, ObjectSet& non_matches,
                        SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    bool simple_eval_safe = ((!m_name || m_name->LocalCandidateInvariant()) &&
                             (parent_context.condition_root_candidate || RootCandidateInvariant()));

    if (simple_eval_safe) {
        // evaluate value and range limits once, use to match all candidates

        std::string name = (m_name ? m_name->Eval(parent_context) : "");

        // get condition from content, apply to matches / non_matches
        const auto condition = GetCombatTargetCondition(m_content_type, name);
        if (condition && condition != this) {
            condition->Eval(parent_context, matches, non_matches, search_domain);
        } else {
            // if somehow in a cyclical loop because some content's location
            // was defined as CombatTarget or if there is no available combat
            // targetting condition (eg. in valid content type, or name of
            // a bit of content that doesn't exist), match nothing
            if (search_domain == SearchDomain::MATCHES) {
                non_matches.insert(non_matches.end(), matches.begin(), matches.end());
                matches.clear();
            }
        }

    } else {
        // re-evaluate value and ranges for each candidate object
        Condition::Eval(parent_context, matches, non_matches, search_domain);
    }
}

std::string CombatTarget::Description(bool negated/* = false*/) const {
    std::string name_str;
    if (m_name)
        name_str = m_name->Description();

    std::string content_type_str = GetContentTypeName(m_content_type);

    return str(FlexibleFormat((!negated)
               ? UserString("DESC_COMBAT_TARGET")
               : UserString("DESC_COMBAT_TARGET_NOT"))
               % content_type_str
               % name_str);
}

std::string CombatTarget::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "CombatTarget content_type = ";

    switch (m_content_type) {
    case ContentType::CONTENT_BUILDING:  retval += "Building";   break;
    case ContentType::CONTENT_FOCUS:     retval += "Focus";      break;
    case ContentType::CONTENT_SHIP_HULL: retval += "Hull";       break;
    case ContentType::CONTENT_SHIP_PART: retval += "Part";       break;
    case ContentType::CONTENT_SPECIAL:   retval += "Special";    break;
    case ContentType::CONTENT_SPECIES:   retval += "Species";    break;
    default:                             retval += "???";
    }

    if (m_name)
        retval += " name = " + m_name->Dump(ntabs);
    return retval;
}

bool CombatTarget::Match(const ScriptingContext& local_context) const {
    auto& candidate = local_context.condition_local_candidate;
    if (!candidate) {
        ErrorLogger() << "CombatTarget::Match passed no candidate object";
        return false;
    }

    std::string name = (m_name ? m_name->Eval(local_context) : "");

    const auto condition = GetCombatTargetCondition(m_content_type, name);
    if (!condition || condition == this)
        return false;

    // other Conditions' Match functions not directly callable, so can't do any
    // better than just calling Eval for each candidate...
    return condition->Eval(local_context, candidate);
}

void CombatTarget::SetTopLevelContent(const std::string& content_name) {
    if (m_name)
        m_name->SetTopLevelContent(content_name);
}

unsigned int CombatTarget::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::CombatTarget");
    CheckSums::CheckSumCombine(retval, m_name);
    CheckSums::CheckSumCombine(retval, m_content_type);

    TraceLogger() << "GetCheckSum(CombatTarget): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> CombatTarget::Clone() const {
    return std::make_unique<CombatTarget>(m_content_type,
                                          CloneUnique(m_name));
}

///////////////////////////////////////////////////////////
// And                                                   //
///////////////////////////////////////////////////////////
And::And(std::vector<std::unique_ptr<Condition>>&& operands) :
    Condition(),
    m_operands(std::move(operands))
{
    m_root_candidate_invariant = boost::algorithm::all_of(m_operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(m_operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(m_operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

And::And(std::unique_ptr<Condition>&& operand1, std::unique_ptr<Condition>&& operand2,
         std::unique_ptr<Condition>&& operand3, std::unique_ptr<Condition>&& operand4) :
    Condition()
{
    // would prefer to initialize the vector m_operands in the initializer list, but this is difficult with non-copyable unique_ptr parameters
    if (operand1)
        m_operands.push_back(std::move(operand1));
    if (operand2)
        m_operands.push_back(std::move(operand2));
    if (operand3)
        m_operands.push_back(std::move(operand3));
    if (operand4)
        m_operands.push_back(std::move(operand4));

    m_root_candidate_invariant = boost::algorithm::all_of(m_operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(m_operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(m_operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

bool And::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const And& rhs_ = static_cast<const And&>(rhs);

    if (m_operands.size() != rhs_.m_operands.size())
        return false;
    for (unsigned int i = 0; i < m_operands.size(); ++i) {
        CHECK_COND_VREF_MEMBER(m_operands.at(i))
    }

    return true;
}

void And::Eval(const ScriptingContext& parent_context, ObjectSet& matches,
               ObjectSet& non_matches, SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    if (m_operands.empty()) {
        ErrorLogger() << "And::Eval given no operands!";
        return;
    }
    for (auto& operand : m_operands) {
        if (!operand) {
            ErrorLogger() << "And::Eval given null operand!";
            return;
        }
    }

    auto ObjList = [](const ObjectSet& objs) -> std::string {
        std::stringstream ss;
        for (const auto& obj : objs)
            ss << obj->Name() << " (" << std::to_string(obj->ID()) << ")  ";
        return ss.str();
    };

    TraceLogger(conditions) << [&]() {
        std::stringstream ss;
        ss << "And::Eval searching " << (search_domain == SearchDomain::MATCHES ? "matches" : "non_matches")
           << " with input matches (" << matches.size() << "): " << ObjList(matches)
           << " and input non_matches(" << non_matches.size() << "): " << ObjList(non_matches);
        return ss.str();
    }();

    if (search_domain == SearchDomain::NON_MATCHES) {
        ObjectSet partly_checked_non_matches;
        partly_checked_non_matches.reserve(non_matches.size());

        // move items in non_matches set that pass first operand condition into
        // partly_checked_non_matches set
        m_operands[0]->Eval(parent_context, partly_checked_non_matches, non_matches, SearchDomain::NON_MATCHES);
        TraceLogger(conditions) << [&]() {
            std::stringstream ss;
            ss << "Subcondition: " << m_operands[0]->Dump()
               <<"\npartly_checked_non_matches (" << partly_checked_non_matches.size() << "): "
               << ObjList(partly_checked_non_matches);
            return ss.str();
        }();

        // move items that don't pass one of the other conditions back to non_matches
        for (unsigned int i = 1; i < m_operands.size(); ++i) {
            if (partly_checked_non_matches.empty()) break;
            m_operands[i]->Eval(parent_context, partly_checked_non_matches, non_matches, SearchDomain::MATCHES);
            TraceLogger(conditions) << [&]() {
                std::stringstream ss;
                ss << "Subcondition: " << m_operands[i]->Dump()
                   <<"\npartly_checked_non_matches (" << partly_checked_non_matches.size() << "): "
                   << ObjList(partly_checked_non_matches);
                return ss.str();
            }();
        }

        // merge items that passed all operand conditions into matches
        matches.insert(matches.end(), std::make_move_iterator(partly_checked_non_matches.begin()),
                       std::make_move_iterator(partly_checked_non_matches.end()));

        // items already in matches set are not checked, and remain in matches set even if
        // they don't match one of the operand conditions

    } else /*(search_domain == SearchDomain::MATCHES)*/ {
        // check all operand conditions on all objects in the matches set, moving those
        // that don't pass a condition to the non-matches set

        for (auto& operand : m_operands) {
            if (matches.empty()) break;
            operand->Eval(parent_context, matches, non_matches, SearchDomain::MATCHES);
            TraceLogger(conditions) << "Subcondition: " << operand->Dump()
                                    <<"\nremaining matches (" << matches.size() << "): " << ObjList(matches);
        }

        // items already in non_matches set are not checked, and remain in non_matches set
        // even if they pass all operand conditions
    }
    TraceLogger(conditions) << "And::Eval final matches (" << matches.size() << "): " << ObjList(matches)
                            << " and non_matches (" << non_matches.size() << "): " << ObjList(non_matches);
}

std::string And::Description(bool negated/* = false*/) const {
    std::string values_str;
    if (m_operands.size() == 1) {
        values_str += (!negated)
            ? UserString("DESC_AND_BEFORE_SINGLE_OPERAND")
            : UserString("DESC_NOT_AND_BEFORE_SINGLE_OPERAND");
        // Pushing the negation to the enclosed conditions
        values_str += m_operands[0]->Description(negated);
        values_str += (!negated)
            ? UserString("DESC_AND_AFTER_SINGLE_OPERAND")
            : UserString("DESC_NOT_AND_AFTER_SINGLE_OPERAND");
    } else {
        values_str += (!negated)
            ? UserString("DESC_AND_BEFORE_OPERANDS")
            : UserString("DESC_NOT_AND_BEFORE_OPERANDS");
        for (unsigned int i = 0; i < m_operands.size(); ++i) {
            // Pushing the negation to the enclosed conditions
            values_str += m_operands[i]->Description(negated);
            if (i != m_operands.size() - 1) {
                values_str += (!negated)
                    ? UserString("DESC_AND_BETWEEN_OPERANDS")
                    : UserString("DESC_NOT_AND_BETWEEN_OPERANDS");
            }
        }
        values_str += (!negated)
            ? UserString("DESC_AND_AFTER_OPERANDS")
            : UserString("DESC_NOT_AND_AFTER_OPERANDS");
    }
    return values_str;
}

std::string And::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "And [\n";
    for (auto& operand : m_operands)
        retval += operand->Dump(ntabs+1);
    retval += DumpIndent(ntabs) + "]\n";
    return retval;
}

void And::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context, ObjectSet& condition_non_targets) const {
    if (!m_operands.empty()) {
        m_operands[0]->GetDefaultInitialCandidateObjects(parent_context, condition_non_targets); // gets condition_non_targets from first operand condition
    } else {
        Condition::GetDefaultInitialCandidateObjects(parent_context, condition_non_targets);
    }
}

void And::SetTopLevelContent(const std::string& content_name) {
    for (auto& operand : m_operands) {
        operand->SetTopLevelContent(content_name);
    }
}

unsigned int And::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::And");
    CheckSums::CheckSumCombine(retval, m_operands);

    TraceLogger() << "GetCheckSum(And): retval: " << retval;
    return retval;
}

std::vector<const Condition*> And::Operands() const {
    std::vector<const Condition*> retval;
    retval.reserve(m_operands.size());
    std::transform(m_operands.begin(), m_operands.end(), std::back_inserter(retval),
                   [](auto& xx) {return xx.get();});
    return retval;
}

std::unique_ptr<Condition> And::Clone() const
{ return std::make_unique<And>(ValueRef::CloneUnique(m_operands)); }

///////////////////////////////////////////////////////////
// Or                                                    //
///////////////////////////////////////////////////////////
Or::Or(std::vector<std::unique_ptr<Condition>>&& operands) :
    Condition(),
    m_operands(std::move(operands))
{
    m_root_candidate_invariant = boost::algorithm::all_of(m_operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(m_operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(m_operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

Or::Or(std::unique_ptr<Condition>&& operand1,
       std::unique_ptr<Condition>&& operand2,
       std::unique_ptr<Condition>&& operand3,
       std::unique_ptr<Condition>&& operand4) :
    Condition()
{
    // would prefer to initialize the vector m_operands in the initializer list, but this is difficult with non-copyable unique_ptr parameters
    if (operand1)
        m_operands.push_back(std::move(operand1));
    if (operand2)
        m_operands.push_back(std::move(operand2));
    if (operand3)
        m_operands.push_back(std::move(operand3));
    if (operand4)
        m_operands.push_back(std::move(operand4));

    m_root_candidate_invariant = boost::algorithm::all_of(m_operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(m_operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(m_operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

bool Or::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const Or& rhs_ = static_cast<const Or&>(rhs);

    if (m_operands.size() != rhs_.m_operands.size())
        return false;
    for (unsigned int i = 0; i < m_operands.size(); ++i) {
        CHECK_COND_VREF_MEMBER(m_operands.at(i))
    }

    return true;
}

void Or::Eval(const ScriptingContext& parent_context, ObjectSet& matches,
              ObjectSet& non_matches, SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    if (m_operands.empty()) {
        ErrorLogger() << "Or::Eval given no operands!";
        return;
    }
    for (auto& operand : m_operands) {
        if (!operand) {
            ErrorLogger() << "Or::Eval given null operand!";
            return;
        }
    }

    if (search_domain == SearchDomain::NON_MATCHES) {
        // check each item in the non-matches set against each of the operand conditions
        // if a non-candidate item matches an operand condition, move the item to the
        // matches set.

        for (auto& operand : m_operands) {
            if (non_matches.empty()) break;
            operand->Eval(parent_context, matches, non_matches, SearchDomain::NON_MATCHES);
        }

        // items already in matches set are not checked and remain in the
        // matches set even if they fail all the operand conditions

    } else {
        ObjectSet partly_checked_matches;
        partly_checked_matches.reserve(matches.size());

        // move items in matches set the fail the first operand condition into
        // partly_checked_matches set
        m_operands[0]->Eval(parent_context, matches, partly_checked_matches, SearchDomain::MATCHES);

        // move items that pass any of the other conditions back into matches
        for (auto& operand : m_operands) {
            if (partly_checked_matches.empty()) break;
            operand->Eval(parent_context, matches, partly_checked_matches, SearchDomain::NON_MATCHES);
        }

        // merge items that failed all operand conditions into non_matches
        non_matches.insert(non_matches.end(), partly_checked_matches.begin(), partly_checked_matches.end());

        // items already in non_matches set are not checked and remain in
        // non_matches set even if they pass one or more of the operand
        // conditions
    }
}

std::string Or::Description(bool negated/* = false*/) const {
    std::string values_str;
    if (m_operands.size() == 1) {
        values_str += (!negated)
            ? UserString("DESC_OR_BEFORE_SINGLE_OPERAND")
            : UserString("DESC_NOT_OR_BEFORE_SINGLE_OPERAND");
        // Pushing the negation to the enclosed conditions
        values_str += m_operands[0]->Description(negated);
        values_str += (!negated)
            ? UserString("DESC_OR_AFTER_SINGLE_OPERAND")
            : UserString("DESC_NOT_OR_AFTER_SINGLE_OPERAND");
    } else {
        // TODO: use per-operand-type connecting language
        values_str += (!negated)
            ? UserString("DESC_OR_BEFORE_OPERANDS")
            : UserString("DESC_NOT_OR_BEFORE_OPERANDS");
        for (unsigned int i = 0; i < m_operands.size(); ++i) {
            // Pushing the negation to the enclosed conditions
            values_str += m_operands[i]->Description(negated);
            if (i != m_operands.size() - 1) {
                values_str += (!negated)
                    ? UserString("DESC_OR_BETWEEN_OPERANDS")
                    : UserString("DESC_NOT_OR_BETWEEN_OPERANDS");
            }
        }
        values_str += (!negated)
            ? UserString("DESC_OR_AFTER_OPERANDS")
            : UserString("DESC_NOT_OR_AFTER_OPERANDS");
    }
    return values_str;
}

void Or::GetDefaultInitialCandidateObjects(const ScriptingContext& parent_context,
                                           ObjectSet& condition_non_targets) const
{
    if (m_operands.empty())
        return;

    if (m_operands.size() == 1) {
        // get condition_non_targets from the single / only operand condition
        m_operands[0]->GetDefaultInitialCandidateObjects(parent_context, condition_non_targets);
        return;
    }

    if (parent_context.source && m_operands.size() == 2) {
        if (dynamic_cast<const Source*>(m_operands[0].get())) {
            // special case when first of two subconditions is just Source:
            // get the default candidates of the second and add the source if
            // it is not already present.
            // TODO: extend to other single-match conditions: RootCandidate, Target
            // TODO: predetermine this situation to avoid repeat runtime dynamic-casts
            // TODO: fancier deep inspection of m_operands to determine optimal
            //       way to combine the default candidates...

            m_operands[1]->GetDefaultInitialCandidateObjects(parent_context, condition_non_targets);
            if (std::find(condition_non_targets.begin(), condition_non_targets.end(), parent_context.source) ==
                condition_non_targets.end())
            { condition_non_targets.push_back(parent_context.source); }
            return;
        }
    }

    // default / fallback
    Condition::GetDefaultInitialCandidateObjects(parent_context, condition_non_targets);

    // Also tried looping over all subconditions in m_operands and putting all
    // of their initial candidates into an unordered_set (to remove duplicates)
    // and then copying the result back into condition_non_targets but this was
    // substantially slower for many Or conditions
}

std::string Or::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "Or [\n";
    for (auto& operand : m_operands)
        retval += operand->Dump(ntabs+1);
    retval += "\n" + DumpIndent(ntabs) + "]\n";
    return retval;
}

void Or::SetTopLevelContent(const std::string& content_name) {
    for (auto& operand : m_operands) {
        operand->SetTopLevelContent(content_name);
    }
}

unsigned int Or::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Or");
    CheckSums::CheckSumCombine(retval, m_operands);

    TraceLogger() << "GetCheckSum(Or): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Or::Clone() const
{ return std::make_unique<Or>(ValueRef::CloneUnique(m_operands)); }

///////////////////////////////////////////////////////////
// Not                                                   //
///////////////////////////////////////////////////////////
Not::Not(std::unique_ptr<Condition>&& operand) :
    Condition(),
    m_operand(std::move(operand))
{
    m_root_candidate_invariant = !m_operand || m_operand->RootCandidateInvariant();
    m_target_invariant = !m_operand || m_operand->TargetInvariant();
    m_source_invariant = !m_operand || m_operand->SourceInvariant();
}

bool Not::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const Not& rhs_ = static_cast<const Not&>(rhs);

    CHECK_COND_VREF_MEMBER(m_operand)

    return true;
}

void Not::Eval(const ScriptingContext& parent_context, ObjectSet& matches, ObjectSet& non_matches,
               SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    if (!m_operand) {
        ErrorLogger() << "Not::Eval found no subcondition to evaluate!";
        return;
    }

    if (search_domain == SearchDomain::NON_MATCHES) {
        // search non_matches set for items that don't meet the operand
        // condition, and move those to the matches set
        m_operand->Eval(parent_context, non_matches, matches, SearchDomain::MATCHES); // swapping order of matches and non_matches set parameters and MATCHES / NON_MATCHES search domain effects NOT on requested search domain
    } else {
        // search matches set for items that meet the operand condition
        // condition, and move those to the non_matches set
        m_operand->Eval(parent_context, non_matches, matches, SearchDomain::NON_MATCHES);
    }
}

std::string Not::Description(bool negated/* = false*/) const
{ return m_operand->Description(!negated); }

std::string Not::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "Not\n";
    retval += m_operand->Dump(ntabs+1);
    return retval;
}

void Not::SetTopLevelContent(const std::string& content_name) {
    if (m_operand)
        m_operand->SetTopLevelContent(content_name);
}

unsigned int Not::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Not");
    CheckSums::CheckSumCombine(retval, m_operand);

    TraceLogger() << "GetCheckSum(Not): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Not::Clone() const
{ return std::make_unique<Not>(ValueRef::CloneUnique(m_operand)); }

///////////////////////////////////////////////////////////
// OrderedAlternativesOf
///////////////////////////////////////////////////////////
namespace {
    void FCMoveContent(ObjectSet& from_set, ObjectSet& to_set) {
        to_set.insert(to_set.end(),
                      std::make_move_iterator(from_set.begin()),
                      std::make_move_iterator(from_set.end()));
        from_set.clear();
    }
}

OrderedAlternativesOf::OrderedAlternativesOf(
    std::vector<std::unique_ptr<Condition>>&& operands) :
    Condition(),
    m_operands(std::move(operands))
{
    m_root_candidate_invariant = boost::algorithm::all_of(m_operands, [](auto& e){ return !e || e->RootCandidateInvariant(); });
    m_target_invariant = boost::algorithm::all_of(m_operands, [](auto& e){ return !e || e->TargetInvariant(); });
    m_source_invariant = boost::algorithm::all_of(m_operands, [](auto& e){ return !e || e->SourceInvariant(); });
}

bool OrderedAlternativesOf::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const OrderedAlternativesOf& rhs_ = static_cast<const OrderedAlternativesOf&>(rhs);

    if (m_operands.size() != rhs_.m_operands.size())
        return false;
    for (unsigned int i = 0; i < m_operands.size(); ++i) {
        CHECK_COND_VREF_MEMBER(m_operands.at(i))
    }

    return true;
}

void OrderedAlternativesOf::Eval(const ScriptingContext& parent_context,
                                 ObjectSet& matches, ObjectSet& non_matches,
                                 SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    if (m_operands.empty()) {
        ErrorLogger() << "OrderedAlternativesOf::Eval given no operands!";
        return;
    }
    for (auto& operand : m_operands) {
        if (!operand) {
            ErrorLogger() << "OrderedAlternativesOf::Eval given null operand!";
            return;
        }
    }

    // OrderedAlternativesOf [ A B C ] matches all candidates which match the topmost condition.
    // If any candidate matches A, then all candidates that match A are matched,
    // or if no candidate matches A but any candidate matches B, then all candidates that match B are matched,
    // or if no candidate matches A or B but any candidate matches C, then all candidates that match C are matched.
    // If no candidate matches A, B, or C, then nothing is matched.
    //
    // Not OrderedAlternativesOf [ A B C ] finds the topmost condition which has matches and then matches its non-matches.
    // If any candidate matches A, then all candidates that do not match A are matched,
    // or if no candidates match A but any candidate matches B, then all candidates that do not match B are matched,
    // or if no candidate matches A or B but any candidate matches C, then all candidates that do not match C are matched.
    // If no candidate matches A, B, or C, then all candidates are matched.
    if (search_domain == SearchDomain::NON_MATCHES) {
        // Check each operand condition on objects in the input matches and non_matches sets, until an operand condition matches an object.
        // If an operand condition is selected, apply it to the input non_matches set, moving matching candidates to matches.
        // If no operand condition is selected, because no candidate is matched by any operand condition, then do nothing.
        ObjectSet temp_objects;
        temp_objects.reserve(std::max(matches.size(),non_matches.size()));

        for (auto& operand : m_operands) {
            operand->Eval(parent_context, temp_objects, non_matches, SearchDomain::NON_MATCHES);
            if (!temp_objects.empty()) {
                // Select the current operand condition. Apply it to the NON_MATCHES candidate set.
                // We alread did the application, so we use the results
                matches.reserve(temp_objects.size() + matches.size());
                FCMoveContent(temp_objects, matches);
                return;
            }
            // Check if the operand condition matches an object in the other input set
            operand->Eval(parent_context, matches, temp_objects, SearchDomain::MATCHES);
            if (!matches.empty()) {
                // Select the current operand condition. Apply it to the NON_MATCHES candidate set.
                // We already did the application before, but there were no matches.
                // restore state before checking the operand
                FCMoveContent(temp_objects, matches);
                return;
            }

            // restore state before checking the operand
            FCMoveContent(temp_objects, matches);
            // try the next operand
        }

        // No operand condition was selected. State is restored. Nothing should be moved to matches input set
    } else /*(search_domain == SearchDomain::MATCHES)*/ {
        // Check each operand condition on objects in the input matches and non_matches sets, until an operand condition matches an object.
        // If an operand condition is selected, apply it to the input matches set, moving non-matching candidates to non_matches.
        // If no operand condition is selected, because no candidate is matched by any operand condition, then move all of the input matches into non_matches.
        ObjectSet temp_objects;
        temp_objects.reserve(std::max(matches.size(),non_matches.size()));

        for (auto& operand : m_operands) {
            // Apply the current operand optimistically. Select it if there are any matching objects in the input sets
            operand->Eval(parent_context, temp_objects, matches, SearchDomain::NON_MATCHES);
            // temp_objects are objects from input matches set which also match the operand
            // matches are objects from input matches set which do not match the operand
            if (!temp_objects.empty()) {
                // Select and apply this operand. Objects in matches do not match this condition.
                non_matches.reserve(matches.size() + non_matches.size());
                FCMoveContent(matches, non_matches);
                FCMoveContent(temp_objects, matches);
                return;
            }
            // Select this operand if there are matching objects in the non_matches input set.
            operand->Eval(parent_context, temp_objects, non_matches, SearchDomain::NON_MATCHES);
            if (!temp_objects.empty()) {
                // Select and apply this operand. But no matching objects exist in the matches input set,
                // so all objects need to be moved into the non_matches set
                non_matches.reserve(matches.size() + non_matches.size() + temp_objects.size());
                FCMoveContent(matches, non_matches);
                FCMoveContent(temp_objects, non_matches);
                return;
            }

            // Operand was not selected. Restore state before. Try next operand.
            FCMoveContent(temp_objects, matches);
        }

        // No operand condition was selected. Objects in matches input set do not match, so move those to non_matches input set.
        non_matches.reserve(matches.size() + non_matches.size());
        FCMoveContent(matches, non_matches);
    }
}

std::string OrderedAlternativesOf::Description(bool negated/* = false*/) const {
    std::string values_str;
    if (m_operands.size() == 1) {
        values_str += (!negated)
            ? UserString("DESC_ORDERED_ALTERNATIVES_BEFORE_SINGLE_OPERAND")
            : UserString("DESC_NOT_ORDERED_ALTERNATIVES_BEFORE_SINGLE_OPERAND");
        // Pushing the negation of matches to the enclosed conditions
        values_str += m_operands[0]->Description(negated);
        values_str += (!negated)
            ? UserString("DESC_ORDERED_ALTERNATIVES_AFTER_SINGLE_OPERAND")
            : UserString("DESC_NOT_ORDERED_ALTERNATIVES_AFTER_SINGLE_OPERAND");
    } else {
        // TODO: use per-operand-type connecting language
        values_str += (!negated)
            ? UserString("DESC_ORDERED_ALTERNATIVES_BEFORE_OPERANDS")
            : UserString("DESC_NOT_ORDERED_ALTERNATIVES_BEFORE_OPERANDS");
        for (unsigned int i = 0; i < m_operands.size(); ++i) {
            // Pushing the negation of matches to the enclosed conditions
            values_str += m_operands[i]->Description(negated);
            if (i != m_operands.size() - 1) {
                values_str += (!negated)
                    ? UserString("DESC_ORDERED_ALTERNATIVES_BETWEEN_OPERANDS")
                    : UserString("DESC_NOT_ORDERED_ALTERNATIVES_BETWEEN_OPERANDS");
            }
        }
        values_str += (!negated)
            ? UserString("DESC_ORDERED_ALTERNATIVES_AFTER_OPERANDS")
            : UserString("DESC_NOT_ORDERED_ALTERNATIVES_AFTER_OPERANDS");
    }
    return values_str;
}

std::string OrderedAlternativesOf::Dump(unsigned short ntabs) const {
    std::string retval = DumpIndent(ntabs) + "OrderedAlternativesOf [\n";
    for (auto& operand : m_operands)
        retval += operand->Dump(ntabs+1);
    retval += DumpIndent(ntabs) + "]\n";
    return retval;
}

void OrderedAlternativesOf::SetTopLevelContent(const std::string& content_name) {
    for (auto& operand : m_operands) {
        operand->SetTopLevelContent(content_name);
    }
}

unsigned int OrderedAlternativesOf::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::OrderedAlternativesOf");
    CheckSums::CheckSumCombine(retval, m_operands);

    TraceLogger() << "GetCheckSum(OrderedAlternativesOf): retval: " << retval;
    return retval;
}

std::vector<const Condition*> OrderedAlternativesOf::Operands() const {
    std::vector<const Condition*> retval;
    retval.reserve(m_operands.size());
    std::transform(m_operands.begin(), m_operands.end(), std::back_inserter(retval),
                   [](auto& xx) {return xx.get();});
    return retval;
}

std::unique_ptr<Condition> OrderedAlternativesOf::Clone() const
{ return std::make_unique<OrderedAlternativesOf>(ValueRef::CloneUnique(m_operands)); }

///////////////////////////////////////////////////////////
// Described                                             //
///////////////////////////////////////////////////////////
Described::Described(std::unique_ptr<Condition>&& condition, const std::string& desc_stringtable_key) :
    Condition(),
    m_condition(std::move(condition)),
    m_desc_stringtable_key(desc_stringtable_key)
{
    m_root_candidate_invariant = !m_condition || m_condition->RootCandidateInvariant();
    m_target_invariant = !m_condition || m_condition->TargetInvariant();
    m_source_invariant = !m_condition || m_condition->SourceInvariant();
}

bool Described::operator==(const Condition& rhs) const {
    if (this == &rhs)
        return true;
    if (typeid(*this) != typeid(rhs))
        return false;

    const Described& rhs_ = static_cast<const Described&>(rhs);

   if (m_desc_stringtable_key != rhs_.m_desc_stringtable_key)
        return false;

    CHECK_COND_VREF_MEMBER(m_condition)

    return true;
}

void Described::Eval(const ScriptingContext& parent_context, ObjectSet& matches, ObjectSet& non_matches,
                     SearchDomain search_domain/* = SearchDomain::NON_MATCHES*/) const
{
    if (!m_condition) {
        ErrorLogger() << "Described::Eval found no subcondition to evaluate!";
        return;
    }
    return m_condition->Eval(parent_context, matches, non_matches, search_domain);
}

std::string Described::Description(bool negated/* = false*/) const {
    if (!m_desc_stringtable_key.empty() && UserStringExists(m_desc_stringtable_key))
        return UserString(m_desc_stringtable_key);
    if (m_condition)
        return m_condition->Description(negated);
    return "";
}

void Described::SetTopLevelContent(const std::string& content_name) {
    if (m_condition)
        m_condition->SetTopLevelContent(content_name);
}

unsigned int Described::GetCheckSum() const {
    unsigned int retval{0};

    CheckSums::CheckSumCombine(retval, "Condition::Described");
    CheckSums::CheckSumCombine(retval, m_condition);
    CheckSums::CheckSumCombine(retval, m_desc_stringtable_key);

    TraceLogger() << "GetCheckSum(Described): retval: " << retval;
    return retval;
}

std::unique_ptr<Condition> Described::Clone() const {
    return std::make_unique<Described>(ValueRef::CloneUnique(m_condition),
                                       m_desc_stringtable_key);
}

}
