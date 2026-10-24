#include "module_runner.hpp"

#include <optional>

#include "invocation_scope.hpp"

#include <rfext/clutter.hpp>

#include <cmath>
#include <stdexcept>

namespace rfmap {

ModuleLossContributor::ModuleLossContributor(std::shared_ptr<rfmod::IModuleInstance> instance, json options,
                                             const PathEnds& ends, TimePoint time, const LandCoverSource* landcover,
                                             std::vector<ModuleFailure>& failures)
    : module_(std::move(instance)),
      id_(module_->manifest().id),
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
        out.emplace_back(id, t.value("label", id), rfcore::Decibel(t.at("valueDb").get<double>()),
                         t.value("source", moduleId));
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
            {"time", UtcTime::format(time_)},
            {"tx", end(ends_.tx, s.front())},
            {"rx", end(ends_.rx, s.back())},
            {"diffraction",
             {{"lineOfSight", d.lineOfSight}, {"nu", d.nu}, {"lossDb", d.loss.value()},
              {"minFresnelClearance", d.minFresnelClearance}}},
            {"profile", {{"source", ctx.profile.sourceName()}, {"samples", samples}}},
            {"options", options_}};
}

rfcore::LossContribution ModuleLossContributor::accept(const json& response) const {
    rfcore::LossContribution out;
    std::vector<std::string> dropped;
    out.terms = acceptTerms(id_, response.value("terms", json()), dropped);
    for (const auto& d : dropped) out.notes.emplace_back(id_, "dropped: " + d);
    if (response.contains("notes") && response.at("notes").is_array()) {
        for (const json& n : response.at("notes")) {
            if (n.is_object()) out.notes.emplace_back(n.value("what", id_), n.value("detail", ""));
        }
    }
    return out;
}

rfcore::LossContribution ModuleLossContributor::contribute(const rfcore::PathContext& ctx) const {
    try {
        // The module may call the host back while it answers; the scope names it, so the host can
        // refuse link.evaluate (which would run this very contributor again). It nests in the
        // request's scope; without one (a direct unit test) host calls are refused anyway.
        std::optional<InvocationScope> scope;
        if (InvocationScope::current()) scope.emplace(id_);
        return accept(module_->invoke("loss.contribute", request(ctx)));
    } catch (const rfmod::ModuleError& e) {
        failures_.emplace_back(id_, e.code() + ": " + e.what());
    } catch (const std::exception& e) {  // NOSONAR(cpp:S1181): a module failure is reported, never fails the calculation
        failures_.emplace_back(id_, e.what());
    }
    return {};
}

std::vector<ModuleRunner::Active> ModuleRunner::optedIn(const std::string& capability, const CalcContext& ctx) const {
    std::vector<Active> out;
    for (std::shared_ptr<rfmod::IModuleInstance>& m : registry_.withCapability(capability)) {
        const std::string id = m->manifest().id;
        if (!ctx.optedIn(id)) continue;
        out.emplace_back(std::move(m), id, ctx.optionsFor(id));
    }
    return out;
}

std::vector<std::shared_ptr<const rfcore::ILossContributor>> ModuleRunner::lossContributors(
    const CalcContext& ctx, const PathEnds& ends, const LandCoverSource* landcover,
    std::vector<ModuleFailure>& failures) const {
    std::vector<std::shared_ptr<const rfcore::ILossContributor>> out;
    for (Active& a : optedIn("loss.contributor", ctx)) {
        out.push_back(std::make_shared<ModuleLossContributor>(std::move(a.instance), std::move(a.options), ends, ctx.time,
                                                              landcover, failures));
    }
    return out;
}

json ModuleRunner::versions(const CalcContext& ctx) const {
    json out = json::object();
    for (const char* cap : {"loss.contributor", "route.preparation", "route.analysis", "coverage.analysis"}) {
        for (const Active& a : optedIn(cap, ctx)) out[a.id] = a.instance->manifest().version.str();
    }
    return out;
}

std::vector<std::string> ModuleRunner::checkOptions(const json& missionModules) const {
    std::vector<std::string> problems;
    if (!missionModules.is_object()) return problems;
    for (const auto& [id, options] : missionModules.items()) {
        if (!registry_.instance(id)) {
            // rf.* without a package is the server's own (rf.core, rf.weather, ...): those read
            // their options with defaults and need no schema here.
            if (!id.starts_with("rf.")) problems.push_back("modules." + id + ": no such module is active on this server");
            continue;
        }
        if (const auto why = registry_.checkOptions(id, options)) problems.push_back("modules." + *why);
    }
    return problems;
}

}  // namespace rfmap
