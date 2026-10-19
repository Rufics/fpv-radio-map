#pragma once

#include "link_service.hpp"

#include <rfmod/host.hpp>

#include <functional>
#include <stdexcept>

namespace rfmap {

class Logger;

/// A host call the server cannot answer; what() is the message the module receives.
class HostCallError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Makes the CalcContext a mission is evaluated with (time, module choices, forecast).
using ContextFactory = std::function<CalcContext(const json& mission)>;

/// The calculation context of the request running on this thread, so a module's host call
/// sees the same time, forecast and module choices as the calculation that called it.
class CalcContextScope {
public:
    explicit CalcContextScope(const CalcContext& ctx);
    ~CalcContextScope();
    CalcContextScope(const CalcContextScope&) = delete;
    CalcContextScope& operator=(const CalcContextScope&) = delete;
    static const CalcContext* current();

private:
    const CalcContext* previous_;
};

/// Host calls of docs/modules/developer-guide.md section 6, answered by the server.
class ServerHostServices final : public rfmod::IHostServices {
public:
    ServerHostServices(LinkService* links, const Library* library, WeatherService* weather, ContextFactory contexts,
                       Logger& log);

    json call(const std::string& moduleId, const std::string& method, const json& request) override;
    void log(const std::string& moduleId, int level, const std::string& message) override;

    void bind(LinkService* links, const Library* library, WeatherService* weather, ContextFactory contexts,
              const class SchemaRegistry* schemas);

    static constexpr int kMaxDepth = 4;

private:
    json linkEvaluate(const json& req) const;
    json profileGet(const json& req) const;
    json elevationPoint(const json& req) const;
    json weatherPoint(const json& req);
    json libraryGet(const json& req) const;

    LinkService* links_;
    const Library* library_;
    WeatherService* weather_;
    ContextFactory contexts_;
    const class SchemaRegistry* schemas_ = nullptr;
    Logger& log_;
};

}  // namespace rfmap
