// The radio calculation on arbitrary terrain (rf-core link budget and Bullington diffraction,
// rf-ext clutter): profiles come from whatever DEM and land cover the operator loads, so any
// shape of ground has to give a usable answer.
//
// Input: radio parameters, then one record per profile sample (step, height, land cover).
// Properties: the documented std::invalid_argument / std::runtime_error are the only failures,
// and a computed margin is a finite number.

#include "../support.hpp"

#include <rfcore/link_budget.hpp>
#include <rfext/clutter.hpp>

#include <cmath>
#include <stdexcept>

using namespace rfcore;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    rfmap::fuzz::ByteReader in(data, size);
    const double k = in.range(0.25, 20.0);
    const Hertz frequency(in.range(30e6, 100e9));
    TransmitterEnd tx;
    tx.antennaAgl = Metres(in.range(0.0, 3000.0));
    tx.antennaGain = DecibelIsotropic(in.range(-10.0, 30.0));
    tx.feederLoss = Decibel(in.range(0.0, 10.0));
    tx.power = DecibelMilliwatt(in.range(-10.0, 40.0));
    ReceiverEnd rx;
    rx.antennaAgl = Metres(in.range(0.0, 3000.0));
    rx.antennaGain = DecibelIsotropic(in.range(-10.0, 30.0));
    rx.feederLoss = Decibel(in.range(0.0, 10.0));
    rx.sensitivity = DecibelMilliwatt(in.range(-130.0, -50.0));
    rx.requiredMargin = Decibel(in.range(0.0, 30.0));
    const double latitude = in.range(-89.0, 89.0);
    const double azimuth = in.range(0.0, 360.0);
    const bool clutter = in.flag();
    const double treeHeight = in.range(0.0, 60.0);

    std::vector<ProfileSample> samples;
    double distance = 0.0;
    while (!in.empty() && samples.size() < 512) {
        // Steps from a centimetre to 5 km; heights from the Dead Sea to Everest.
        distance += samples.empty() ? 0.0 : in.range(0.01, 5000.0);
        const Metres ground(in.range(-430.0, 8850.0));
        const auto cover = static_cast<int>(in.below(11) + 1) * 10;
        samples.emplace_back(Metres(distance), ground, Metres(0.0), Metres(4.0), GeoPoint{latitude, 30.0}, cover);
    }

    try {
        TerrainProfile profile(std::move(samples), "fuzz", azimuth, latitude);
        if (clutter) {
            std::map<rfext::LandCover, rfext::ClassSetting> settings;
            settings[rfext::LandCover::Tree] = {true, Metres(treeHeight), "fuzz", nullptr};
            settings[rfext::LandCover::BuiltUp] = {true, std::nullopt, "", nullptr};
            (void)rfext::ClutterConditioner(settings).apply(profile);
        }
        const LinkEvaluator eval{EarthModel(k), BullingtonDiffraction()};
        const LinkResult r = eval.evaluate(profile, frequency, tx, rx);
        rfmap::fuzz::check(std::isfinite(r.margin.value()));
        rfmap::fuzz::check(r.diffraction.dominantSample < profile.samples().size());
    } catch (const std::invalid_argument& e) {
        // A profile the radio maths documents as unusable: refusing it is the expected answer.
        rfmap::fuzz::refused(e);
    } catch (const std::runtime_error& e) {
        // A calculation the maths documents as impossible on this ground: also expected.
        rfmap::fuzz::refused(e);
    }
    return 0;
}
