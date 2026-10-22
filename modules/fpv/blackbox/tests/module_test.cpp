// fpv.blackbox entry: flightlog.probe and flightlog.decode through the host ABI, and their errors.

#include "sample_log.hpp"

#include <fake_host.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace fpv;
using namespace fpv::test;

namespace {

class BlackboxModule : public ModuleEntryTest {
protected:
    static std::string base64(const std::vector<std::uint8_t>& bytes) {
        static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (std::size_t i = 0; i < bytes.size(); i += 3) {
            const std::uint32_t n = static_cast<std::uint32_t>(bytes[i]) << 16 |
                                    (i + 1 < bytes.size() ? static_cast<std::uint32_t>(bytes[i + 1]) << 8 : 0u) |
                                    (i + 2 < bytes.size() ? bytes[i + 2] : 0u);
            out += kAlphabet[(n >> 18) & 63];
            out += kAlphabet[(n >> 12) & 63];
            out += i + 1 < bytes.size() ? kAlphabet[(n >> 6) & 63] : '=';
            out += i + 2 < bytes.size() ? kAlphabet[n & 63] : '=';
        }
        return out;
    }
};

}  // namespace

TEST_F(BlackboxModule, ManifestSelfTestAndErrors) {
    EXPECT_EQ(module_.manifest().at("id"), "fpv.blackbox");
    const auto self = module_.selftest();
    ASSERT_EQ(self.status, RFMOD_OK);
    EXPECT_TRUE(self.body.at("ok").get<bool>()) << self.body.dump();

    EXPECT_EQ(module_.invoke("route.analyze", json::object()).status, RFMOD_ERR_UNKNOWN_METHOD);
    EXPECT_EQ(module_.invoke("flightlog.decode", std::string("{not json")).status, RFMOD_ERR_BAD_REQUEST);
    EXPECT_EQ(module_.invoke("flightlog.decode", json::object()).status, RFMOD_ERR_BAD_REQUEST);
    EXPECT_EQ(module_.invoke("flightlog.decode", json{{"file", "@@@"}}).status, RFMOD_ERR_BAD_REQUEST);
    const json badRate = {{"file", base64(blackbox::SampleLog::build())}, {"options", {{"sampleHz", 0}}}};
    EXPECT_EQ(module_.invoke("flightlog.decode", badRate).status, RFMOD_ERR_BAD_REQUEST);
}

TEST_F(BlackboxModule, ProbeRecognisesBlackboxOnly) {
    const auto yes = module_.invoke("flightlog.probe", json{{"file", base64(blackbox::SampleLog::build())}});
    ASSERT_EQ(yes.status, RFMOD_OK) << yes.body.dump();
    EXPECT_TRUE(yes.body.at("match").get<bool>());
    EXPECT_EQ(yes.body.at("firmware"), "Betaflight 4.5.1");
    const auto no = module_.invoke("flightlog.probe", json{{"file", base64({'P', 'K', 3, 4})}});
    ASSERT_EQ(no.status, RFMOD_OK);
    EXPECT_FALSE(no.body.at("match").get<bool>());
}

TEST_F(BlackboxModule, DecodeAnswersTheNormalizedLog) {
    const json req = {{"file", base64(blackbox::SampleLog::build())}, {"fileName", "LOG00001.BFL"}, {"options", {{"sampleHz", 1000}}}};
    const auto r = module_.invoke("flightlog.decode", req);
    ASSERT_EQ(r.status, RFMOD_OK) << r.body.dump();
    EXPECT_EQ(r.body.at("schema"), "flightlog/v1");
    EXPECT_EQ(r.body.at("source").at("decoder"), "fpv.blackbox");
    EXPECT_EQ(r.body.at("source").at("fileName"), "LOG00001.BFL");
    ASSERT_EQ(r.body.at("logs").size(), 1u);
    EXPECT_EQ(r.body.at("logs").at(0).at("series").at("t").size(), 2u);
}
