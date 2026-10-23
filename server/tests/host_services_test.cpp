#include "host_services.hpp"
#include "logger.hpp"
#include "module_runner.hpp"

#include <rfcore/link_budget.hpp>

#include <gtest/gtest.h>

#include <functional>

using namespace rfmap;

namespace {

class FakeModule final : public rfmod::IModuleInstance {
public:
    FakeModule(const std::string& id, std::function<json(const std::string&, const json&)> handler)
        : handler_(std::move(handler)) {
        manifest_.id = id;
        manifest_.version = rfmod::Version(1, 0, 0);
        manifest_.provides = {"loss.contributor"};
    }
    [[nodiscard]] const rfmod::Manifest& manifest() const override { return manifest_; }
    [[nodiscard]] bool isolated() const override { return false; }
    json invoke(const std::string& method, const json& request) override {
        ++calls;
        return handler_(method, request);
    }
    void shutdown() override {}
    int calls = 0;

private:
    rfmod::Manifest manifest_;
    std::function<json(const std::string&, const json&)> handler_;
};

rfcore::TerrainProfile flatProfile() {
    std::vector<rfcore::ProfileSample> s;
    for (int i = 0; i <= 10; ++i) {
        s.push_back({rfcore::Metres(100.0 * i), rfcore::Metres(150.0), rfcore::Metres(0.0), rfcore::Metres(2.0),
                     {48.5 + 0.0009 * i, 35.2}});
    }
    return rfcore::TerrainProfile(std::move(s), "test", 0.0, 48.5045);
}

rfcore::LinkResult evaluateWith(std::shared_ptr<const rfcore::ILossContributor> c) {
    rfcore::LinkEvaluator ev(rfcore::EarthModel(4.0 / 3.0), rfcore::BullingtonDiffraction());
    if (c) ev.addContributor(std::move(c));
    rfcore::TransmitterEnd tx{};
    tx.antennaAgl = rfcore::Metres(2.0);
    tx.power = rfcore::DecibelMilliwatt(20.0);
    rfcore::ReceiverEnd rx{};
    rx.antennaAgl = rfcore::Metres(100.0);
    rx.sensitivity = rfcore::DecibelMilliwatt(-100.0);
    return ev.evaluate(flatProfile(), rfcore::Hertz(2.44e9), tx, rx);
}

PathEnds ends() { return {{{48.5, 35.2}, 2.0}, {{48.509, 35.2}, 100.0}}; }

double total(const rfcore::LinkResult& r) {
    double t = 0.0;
    for (const auto& term : r.terms) t += term.value.value();
    return t;
}

}  // namespace

TEST(TermPrefixRule, OnlyTermsInTheModulesNamespaceAreKept) {
    std::vector<std::string> dropped;
    const json terms = json::parse(R"([
        {"id": "acme.rain/path", "label": "Rain", "valueDb": 0.42, "source": "P.838"},
        {"id": "free_space", "valueDb": -60},
        {"id": "acme.rainbow/x", "valueDb": 1},
        {"id": "acme.rain/", "valueDb": 1},
        {"id": "rf.weather/rain", "valueDb": -3},
        {"id": "acme.rain/nan", "valueDb": "lots"},
        {"label": "no id", "valueDb": 1}
    ])");
    const auto kept = ModuleLossContributor::acceptTerms("acme.rain", terms, dropped);
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_EQ(kept[0].id, "acme.rain/path");
    EXPECT_DOUBLE_EQ(kept[0].value.value(), 0.42);
    EXPECT_EQ(dropped.size(), 6u);
}

TEST(TermPrefixRule, ForeignTermsNeverReachTheBudget) {
    auto m = std::make_shared<FakeModule>("acme.rain", [](const std::string& method, const json& req) -> json {
        EXPECT_EQ(method, "loss.contribute");
        EXPECT_DOUBLE_EQ(req.at("frequencyHz").get<double>(), 2.44e9);
        EXPECT_EQ(req.at("profile").at("samples").size(), 11u);
        EXPECT_EQ(req.at("options").at("rate"), 5);
        // A gain smuggled in under someone else's id must not count.
        return {{"terms", {{{"id", "acme.rain/path"}, {"valueDb", 1.25}},
                           {{"id", "free_space"}, {"label", "fake"}, {"valueDb", -40.0}}}},
                {"notes", {{{"what", "Rain"}, {"detail", "forecast"}}}}};
    });
    std::vector<ModuleFailure> failures;
    const auto base = evaluateWith(nullptr);
    const auto withModule = evaluateWith(
        std::make_shared<ModuleLossContributor>(m, json{{"rate", 5}}, ends(), Clock::now(), nullptr, failures));
    EXPECT_EQ(m->calls, 1);
    EXPECT_TRUE(failures.empty());
    EXPECT_NEAR(total(withModule) - total(base), 1.25, 1e-12);
    EXPECT_NEAR(base.margin.value() - withModule.margin.value(), 1.25, 1e-12);
    const bool hasForeign = std::any_of(withModule.terms.begin(), withModule.terms.end(),
                                        [](const rfcore::BudgetTerm& t) { return t.label == "fake"; });
    EXPECT_FALSE(hasForeign);
    const bool noted = std::any_of(withModule.uncertainty.begin(), withModule.uncertainty.end(),
                                   [](const rfcore::UncertaintyNote& n) { return n.detail.find("free_space") != std::string::npos; });
    EXPECT_TRUE(noted) << "the dropped term is reported";
}

TEST(TermPrefixRule, AFailingModuleContributesNothingAndIsReported) {
    auto m = std::make_shared<FakeModule>("acme.broken", [](const std::string&, const json&) -> json {
        throw rfmod::ModuleError("internal", "division by zero");
    });
    std::vector<ModuleFailure> failures;
    const auto base = evaluateWith(nullptr);
    const auto r = evaluateWith(std::make_shared<ModuleLossContributor>(m, json::object(), ends(), Clock::now(),
                                                                        nullptr, failures));
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_EQ(failures[0].moduleId, "acme.broken");
    EXPECT_NE(failures[0].error.find("division by zero"), std::string::npos);
    EXPECT_DOUBLE_EQ(r.margin.value(), base.margin.value());
}

TEST(HostCalls, OutsideARequestEveryCallIsRefused) {
    Logger log;
    log.setQuiet(true);
    ServerHostServices host(log);
    EXPECT_THROW((void)host.call("m.x", "geodesy.inverse", json::object()), HostCallError);
    EXPECT_THROW((void)host.call("m.x", "fs.read", json::object()), HostCallError);
    EXPECT_FALSE(host.methods().find("fs.read"));
    EXPECT_TRUE(host.methods().find("link.evaluate"));
}

TEST(HostRequest, ReadingsNameTheFieldAndItsRange) {
    const json r = {{"lat", 48.5}, {"lon", 35.2}, {"n", "nan"}, {"s", "text"}, {"p", {{"lat", 91.0}, {"lon", 0.0}}}};
    const HostRequest req(r);
    EXPECT_DOUBLE_EQ(req.number("lat", -90, 90), 48.5);
    EXPECT_THROW((void)req.number("lat", 0, 10), HostCallError);
    EXPECT_THROW((void)req.number("s", 0, 10), HostCallError);
    EXPECT_THROW((void)req.number("missing", 0, 10), HostCallError);
    EXPECT_FALSE(req.optionalNumber("missing", 0, 10).has_value());
    EXPECT_EQ(req.string("s"), "text");
    EXPECT_THROW((void)req.string("s", 2), HostCallError);
    EXPECT_THROW((void)req.point("p"), HostCallError);
    EXPECT_THROW((void)req.object("s"), HostCallError);
    const TimePoint fallback = Clock::now();
    EXPECT_EQ(req.time("missing", fallback), fallback);
    EXPECT_THROW((void)req.time("s", fallback), HostCallError);
}
