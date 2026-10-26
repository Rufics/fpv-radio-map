#include "result_key.hpp"

#include "result_cache.hpp"

namespace rfmap {

void LibraryRefList::add(const json& ref) {
    for (const auto& r : refs_) {
        if (r.at("kind") == ref.at("kind") && r.at("id") == ref.at("id") && r.at("version") == ref.at("version")) return;
    }
    refs_.push_back(ref);
}

void LibraryRefList::addAll(const std::vector<LibraryRef>& refs) {
    for (const auto& r : refs) add(r.toJson());
}

ResultKeyBuilder::ResultKeyBuilder(const LinkService& links, const BuiltinModules& builtins, const ModuleRunner& modules)
    : links_(links), builtins_(builtins), modules_(modules) {}

json ResultKeyBuilder::environment(const CalcContext& ctx) const {
    const std::optional<double> demError = links_.dem() ? links_.dem()->statedErrorM() : std::nullopt;
    json key = {{"server", links_.version()},
                {"dem", links_.dem() ? links_.dem()->raster().identity() : ""},
                {"demError", demError ? json(*demError) : json()},
                {"landcover", links_.landcover() ? links_.landcover()->raster().identity() : ""},
                {"builtins", builtins_.list()},
                {"modules", modules_.versions(ctx)}};
    // A forecast is an input like any other: a new run gives a new key.
    if (ctx.forecast) key["forecast"] = ctx.forecast->point;
    return key;
}

std::string ResultKeyBuilder::hash(json inputs, const LibraryRefList& refs, const CalcContext& ctx) const {
    inputs["library"] = refs.toJson();
    inputs["time"] = UtcTime::format(ctx.time);
    inputs["env"] = environment(ctx);
    return inputHash(inputs);
}

}  // namespace rfmap
