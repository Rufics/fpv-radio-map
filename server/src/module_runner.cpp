#include "module_runner.hpp"

#include <rfext/clutter.hpp>

#include <cmath>

namespace rfmap {

namespace {
thread_local std::string activeContributor;
}

LossContributionScope::LossContributionScope(std::string moduleId) : previous_(activeContributor) {
    activeContributor = std::move(moduleId);
}

// swap, not assignment: it cannot throw, and this runs in a destructor.
LossContributionScope::~LossContributionScope() { activeContributor.swap(previous_); }

const std::string& LossContributionScope::active() { return activeContributor; }

ModuleLossContributor::ModuleLossContributor(rfmod::IModuleInstance& instance, json options, const PathEnds& ends,
                                             TimePoint time, const LandCoverSource* landcover,
                                             std::vector<ModuleFailure>& failures)
    : module_(instance),
      id_(instance.manifest().id),
      options_(std::move(options)),
      ends_(ends),
      time_(time),
      landcover_(landcover),
      failures_(failures) {}

std::vector<rfcore::BudgetTerm> ModuleLossContributor::acceptTerms(const std::string& moduleId, const json& terms,
                                                                   std::vector<std::string>& dropped) {
    std::vector<rfcore::BudgetTerm> out;
    if (!terms.is_array()) {
        if (!terms.is_null()) dropped.emplace_back("terms is not an array");
        return out;
    }
    const std::string prefix = moduleId + "/";
    for (const json& t : terms) {
        const std::string id = t.is_object() ? t.value("id", "") : "";
        if (!id.starts_with(prefix) || id.size() == prefix.size()) {
            dropped.push_back("term '" + id + "' does not start with " + prefix);
            continue;
        }
        if (!t.contains("valueDb") || !t.at("valueDb").is_number() || !std::isfinite(t.at("valueDb").get<double>())) {
            dropped.push_back("term '" + id + "' has no finite valueDb");
            continue;
        }
        out.push_back({id, t.value("label", id), rfcore::Decibel(t.at("valueDb").get<double>()),
                       t.value("source", moduleId)});
    }
    return out;
}

json ModuleLossContributor::request(const rfcore::PathContext& ctx) const {
    const auto& s = ctx.profile.samples();
    json samples = json::array();
    for (const auto& p : s) {
        json j = {{"distanceM", p.distance.value()},
                  {"lat", p.position.latitudeDeg},
                  {"lon", p.position.longitudeDeg},
                  {"groundMslM", p.groundMsl.value()},
                  {"obstacleM", p.obstacleHeight.value()},
                  {"uncertaintyM", p.uncertainty.value()}};
        if (landcover_ && p.landCover.has_value()) {
            if (const auto c = rfext::landCoverFromCode(*p.landCover)) {
                j["landCover"] = rfext::landCoverName(*c);
                j["landCoverCode"] = *p.landCover;
            }
        }
        samples.push_back(std::move(j));
    }
    auto end = [](const Terminal& t, const rfcore::ProfileSample& p) {
        return json{{"lat", t.position.latitudeDeg},
                    {"lon", t.position.longitudeDeg},
                    {"groundMslM", p.groundMsl.value()},
                    {"antennaAglM", t.aglM}};
    };
    const auto& d = ctx.diffraction;
    return {{"frequencyHz", ctx.frequency.value()},
            {"distanceM", ctx.profile.length().value()},
            {"rayLengthM", ctx.geometry.rayLength().value()},
            {"time", isoUtc(time_)},
            {"tx", end(ends_.tx, s.front())},
            {"rx", end(ends_.rx, s.back())},
            {"diffraction",
             {{"lineOfSight", d.lineOfSight}, {"nu", d.nu}, {"lossDb", d.loss.value()},
              {"minFresnelClearance", d.minFresnelClearance}}},
            {"profile", {{"source", ctx.profile.sourceName()}, {"samples", samples}}},
            {"options", options_}};
}

std::vector<rfcore::BudgetTerm> ModuleLossContributor::contribute(const rfcore::PathContext& ctx) const {
    std::vector<rfcore::UncertaintyNote> notes;
    std::vector<rfcore::BudgetTerm> terms;
    try {
        json response;
        {
            const LossContributionScope scope(id_);
            response = module_.invoke("loss.contribute", request(ctx));
        }
        std::vector<std::string> dropped;
        terms = acceptTerms(id_, response.value("terms", json()), dropped);
        for (const auto& d : dropped) notes.emplace_back(id_, "dropped: " + d);
        if (response.contains("notes") && response.at("notes").is_array()) {
            for (const json& n : response.at("notes")) {
                if (n.is_object()) notes.emplace_back(n.value("what", id_), n.value("detail", ""));
            }
        }
    } catch (const rfmod::ModuleError& e) {
        failures_.push_back({id_, e.code() + ": " + e.what()});
    } catch (const std::exception& e) {
        failures_.push_back({id_, e.what()});
    }
    const std::scoped_lock lock(lastMutex_);
    lastProfile_ = &ctx.profile;
    lastNotes_ = std::move(notes);
    return terms;
}

std::vector<rfcore::UncertaintyNote> ModuleLossContributor::notes(const rfcore::PathContext& ctx) const {
    const std::scoped_lock lock(lastMutex_);
    return &ctx.profile == lastProfile_ ? lastNotes_ : std::vector<rfcore::UncertaintyNote>{};
}

std::vector<ModuleRunner::Active> ModuleRunner::optedIn(const std::string& capability, const CalcContext& ctx) const {
    std::vector<Active> out;
    if (!registry_) return out;
    for (rfmod::IModuleInstance* m : registry_->withCapability(capability)) {
        const std::string& id = m->manifest().id;
        if (!ctx.optedIn(id)) continue;
        out.push_back({m, id, ctx.optionsFor(id)});
    }
    return out;
}

std::vector<std::shared_ptr<const rfcore::ILossContributor>> ModuleRunner::lossContributors(
    const CalcContext& ctx, const PathEnds& ends, const LandCoverSource* landcover,
    std::vector<ModuleFailure>& failures) const {
    std::vector<std::shared_ptr<const rfcore::ILossContributor>> out;
    for (const Active& a : optedIn("loss.contributor", ctx)) {
        out.push_back(std::make_shared<ModuleLossContributor>(*a.instance, a.options, ends, ctx.time, landcover, failures));
    }
    return out;
}

json ModuleRunner::versions(const CalcContext& ctx) const {
    json out = json::object();
    if (!registry_) return out;
    for (const char* cap : {"loss.contributor", "route.analysis", "coverage.analysis"}) {
        for (const Active& a : optedIn(cap, ctx)) out[a.id] = a.instance->manifest().version.str();
    }
    return out;
}

}  // namespace rfmap
