#include "rfmod/loader.hpp"

namespace rfmod {

ModuleLoader::ModuleLoader(LoaderConfig config, const ConfinedRoot& root, IHostServices& services)
    : config_(std::move(config)), root_(root), services_(services) {}

bool ModuleLoader::isolates(const TrustedKey& signer) const { return config_.isolateAll || !signer.firstParty(); }

std::shared_ptr<IModuleInstance> ModuleLoader::load(const Manifest& manifest, const TrustedKey& signer,
                                                    const std::filesystem::path& dir, const json& options,
                                                    bool forceIsolated) const {
    const std::filesystem::path moduleDir = root_.confine(dir);
    const std::filesystem::path library = root_.confine(moduleDir / manifest.entry.at(currentPlatform()));
    if (!forceIsolated && !isolates(signer)) {
        return std::make_shared<InProcessModule>(manifest, root_, library, services_, options);
    }
    // The owner's own modules are trusted with the host process; a worker for them is a
    // convenience (isolateAll, self-test) and must not fail where the kernel lacks a sandbox.
    ModulePolicy policy = config_.policy;
    if (signer.firstParty()) policy.requireSandbox = false;
    return std::make_shared<IsolatedModule>(manifest, moduleDir, library, config_.workerExecutable, services_, options,
                                            policy);
}

}  // namespace rfmod
