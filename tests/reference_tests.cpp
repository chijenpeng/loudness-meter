// Independent offline numerical anchors, not a production meter or streaming core.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr long double offset = -0.691L;
constexpr long double pi = 3.141592653589793238462643383279502884L;
long double powerFor(long double lufs) { return std::pow(10.0L, (lufs - offset) / 10); }
long double loudness(long double energy) {
    return energy > 0 ? offset + 10 * std::log10(energy)
                      : -std::numeric_limits<long double>::infinity();
}
void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void near(long double actual, long double expected, long double tolerance, const char* message) {
    check(std::abs(actual - expected) <= tolerance, message);
}
long double integrated(const std::vector<long double>& energies) {
    std::vector<long double> absolute;
    for (auto energy : energies)
        if (std::isfinite(energy) && energy > powerFor(-70)) absolute.push_back(energy);
    if (absolute.empty()) return loudness(0);
    long double sum = 0;
    for (auto energy : absolute) sum += energy;
    const auto relative = sum / static_cast<long double>(absolute.size()) / 10;
    sum = 0;
    std::size_t count = 0;
    for (auto energy : absolute)
        if (energy > relative) { sum += energy; ++count; }
    return loudness(sum / static_cast<long double>(count));
}
long double quantile(std::vector<long double> values, long double probability) {
    if (values.empty()) return std::numeric_limits<long double>::quiet_NaN();
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(
        std::floor(static_cast<long double>(values.size() - 1) * probability + 0.5L));
    return values[index];
}
long double lra(const std::vector<long double>& energies) {
    std::vector<long double> absolute;
    for (auto energy : energies)
        if (std::isfinite(energy) && energy >= powerFor(-70)) absolute.push_back(energy);
    if (absolute.empty()) return std::numeric_limits<long double>::quiet_NaN();
    long double sum = 0;
    for (auto energy : absolute) sum += energy;
    const auto relative = sum / static_cast<long double>(absolute.size()) / 100;
    std::vector<long double> selected;
    for (auto energy : absolute)
        if (energy >= relative) selected.push_back(loudness(energy));
    return quantile(selected, 0.95L) - quantile(selected, 0.10L);
}
std::vector<long double> sectionEnergies(const std::vector<long double>& levels) {
    std::vector<long double> energies;
    for (int end = 30; end <= static_cast<int>(levels.size()) * 200; ++end) {
        long double energy = 0;
        for (int section = 0; section < static_cast<int>(levels.size()); ++section) {
            const auto overlap = std::max(0, std::min(end, (section + 1) * 200)
                                             - std::max(end - 30, section * 200));
            energy += static_cast<long double>(overlap) * powerFor(levels[static_cast<std::size_t>(section)]);
        }
        energies.push_back(energy / 30);
    }
    return energies;
}
long double kGainAt1k() {
    const std::complex<long double> z = std::exp(std::complex<long double>(0, -2 * pi * 1000 / 48000));
    const auto shelf = (1.53512485958697L - 2.69169618940638L * z + 1.19839281085285L * z * z)
                     / (1.0L - 1.69065929318241L * z + 0.73248077421585L * z * z);
    const auto rlb = (1.0L - 2.0L * z + z * z)
                   / (1.0L - 1.99004745483398L * z + 0.99007225036621L * z * z);
    return 20 * std::log10(std::abs(shelf * rlb));
}
void runTests() {
    const auto gain = kGainAt1k();
    near(gain, 0.6977043961L, 1e-9L, "48k K-weight 1k spectral anchor");
    near(offset + gain - 10 * std::log10(2.0L), -3.0035955606L, 1e-9L, "mono fullpeak sine anchor");
    near(offset + gain - 20, -19.993295604L, 1e-9L, "stereo .1 peak sine anchor");
    near(integrated({1, 0.01L}), offset, 1e-12L, "two-stage gate excludes low block");
    check(std::isinf(integrated({0, 0})), "gate-empty distinct from -70");
    check(std::isinf(integrated({powerFor(-70)})), "I strict absolute boundary");
    near(lra({powerFor(-70), powerFor(-70)}), 0, 1e-12L, "LRA inclusive absolute boundary");
    near(quantile({0, 10, 20, 30}, 0.1L), 0, 1e-12L, "nearest order lower quantile");
    near(quantile({0, 10, 20, 30}, 0.95L), 30, 1e-12L, "nearest order upper quantile");
    near(lra(sectionEnergies({-20, -30})), 10, 1, "nominal 10 LU sections");
    near(lra(sectionEnergies({-20, -15})), 5, 1, "nominal 5 LU sections");
    near(lra(sectionEnergies({-40, -20})), 20, 1, "nominal 20 LU sections");
    near(lra(sectionEnergies({-50, -35, -20, -35, -50})), 15, 1, "nominal 15 LU sections");
    near(20 * std::log10(1.41L), 2.9843822531L, 1e-9L, "TP19 analytic amplitude");
    std::vector<long double> boundary(10, 1.0001L);
    boundary[0] = 90.9991L;
    near(integrated(boundary), 9.309L, 1e-10L, "histogram cut-bin adversarial exact anchor");
    for (auto rate : std::array<int, 6>{44100, 48000, 88200, 96000, 176400, 192000}) {
        const auto hop = rate / 10;
        check(hop * 10 == rate && hop * 4 == rate * 4 / 10 && hop * 30 == rate * 3,
              "supported sample-rate grid has exact integer boundaries");
    }
}
void printAnchors() {
    const auto gain = kGainAt1k();
    std::cout << std::setprecision(17)
              << "{\"k_gain_1k_db\":" << gain
              << ",\"mono_peak1_1k_lufs\":" << offset + gain - 10 * std::log10(2.0L)
              << ",\"stereo_peak01_1k_lufs\":" << offset + gain - 20
              << ",\"integrated_1_001\":" << integrated({1, 0.01L})
              << ",\"lra_20_30\":" << lra(sectionEnergies({-20, -30}))
              << ",\"tp19_analytical_dbtp\":" << 20 * std::log10(1.41L) << "}\n";
}
}
int main(int argc, char** argv) {
    try {
        runTests();
        if (argc == 2 && std::string(argv[1]) == "--anchors-json") printAnchors();
        else if (argc == 1) std::cout << "Independent numerical reference anchors passed; no product was tested.\n";
        else throw std::runtime_error("usage: loudness_reference_tests [--anchors-json]");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
