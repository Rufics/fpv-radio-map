// Flight log uploads: the fpv.blackbox decoder (header, frame encodings, predictors, resync after
// corrupt frames) and then rf-flightlog, which turns its answer into the replay the map draws.
// Any user can upload a log, and a log cut off mid-frame or with garbage between flights is
// normal for Blackbox files, so every input must decode without a crash.
//
// Input: log file bytes. Properties: probe and decode never throw and agree on the number of
// flights, and the processor accepts every document the decoder answers.

#include "../support.hpp"

#include "decoder.hpp"

#include <rfflightlog/processor.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const fpv::blackbox::BlackboxDecoder decoder("fpv.blackbox", "0.0.0-fuzz");
    static const rfflightlog::FlightLogProcessor processor;
    const std::span<const std::uint8_t> bytes(data, size);

    const nlohmann::json probe = decoder.probe(bytes);
    // A small sample rate and cap keep each run short; the decoding paths are the same.
    const nlohmann::json document = decoder.decode(bytes, "fuzz.bbl", {.sampleHz = 25.0, .maxSamples = 4000});
    rfmap::fuzz::check(probe.at("logs").get<std::size_t>() == document.at("logs").size());
    rfmap::fuzz::check(probe.at("match").get<bool>() == !document.at("logs").empty());

    const auto flights = processor.process(document, {.timelineHz = 5.0, .story = {}});
    rfmap::fuzz::check(flights.size() == document.at("logs").size());
    for (const auto& flight : flights) static_cast<void>(flight.toJson());
    return 0;
}
