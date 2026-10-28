#include "LoudnessCore.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using loudness::Core;
namespace {
constexpr double pi = 3.141592653589793238462643383279502884;
void check(bool pass, const char* what) { if (!pass) throw std::runtime_error(what); }
void near(double actual, double expected, double tol, const char* what) {
    if (!(std::abs(actual - expected) <= tol)) {
        std::cerr << what << ": got " << actual << ", expected " << expected << '\n';
        throw std::runtime_error(what);
    }
}
double power(double lufs) { return std::pow(10.0, (lufs + 0.691) / 10); }
double lufs(double energy) { return -0.691 + 10 * std::log10(energy); }
// Independent raw-list oracle: never uses production bins or energy sums.
double integrated(const std::vector<double>& values) {
    std::vector<double> absolute;
    for (double value : values) if (value > power(-70)) absolute.push_back(value);
    if (absolute.empty()) return -std::numeric_limits<double>::infinity();
    long double sum = 0;
    for (double value : absolute) sum += value;
    const long double relative = sum / absolute.size() / 10;
    sum = 0; std::size_t count = 0;
    for (double value : absolute) if (value > relative) { sum += value; ++count; }
    return lufs(static_cast<double>(sum / count));
}
double range(const std::vector<double>& values) {
    std::vector<double> absolute;
    for (double value : values) if (value >= power(-70)) absolute.push_back(value);
    if (absolute.empty()) return std::numeric_limits<double>::quiet_NaN();
    long double sum = 0;
    for (double value : absolute) sum += value;
    const long double relative = sum / absolute.size() / 100;
    std::vector<double> selected;
    for (double value : absolute) if (value >= relative) selected.push_back(value);
    std::sort(selected.begin(), selected.end());
    auto pick = [&](double p) { return selected[static_cast<std::size_t>(std::floor((selected.size() - 1) * p + 0.5))]; };
    return 10 * std::log10(pick(.95) / pick(.1));
}
// Published 48 kHz BS.1770 coefficients, kept separate from production implementation.
constexpr std::array<double, 5> shelf48{1.53512485958697, -2.69169618940638,
    1.19839281085285, -1.69065929318241, 0.73248077421585};
constexpr std::array<double, 5> highpass48{1, -2, 1, -1.99004745483398, 0.99007225036621};
struct RefFilter {
    std::array<double, 5> c;
    double x1=0,x2=0,y1=0,y2=0;
    double step(double x) {
        const double y = c[0]*x+c[1]*x1+c[2]*x2-c[3]*y1-c[4]*y2;
        x2=x1; x1=x; y2=y1; y1=y; return y;
    }
};
double spectralGain(int rate, double frequency) {
    // Evaluates the analog De Man prefilter response by a separate complex transfer function.
    const double ks = std::tan(pi * 1681.974450955533 / rate);
    const double qs = .7071752369554196;
    const double vh = std::pow(10., 3.99984385397/20.);
    const double vb = std::pow(vh, .4996667741545416);
    const double as = 1+ks/qs+ks*ks;
    const std::array<double,5> s{(vh+vb*ks/qs+ks*ks)/as,2*(ks*ks-vh)/as,
       (vh-vb*ks/qs+ks*ks)/as,2*(ks*ks-1)/as,(1-ks/qs+ks*ks)/as};
    const double kh=std::tan(pi*38.13547087602444/rate), qh=.5003270373238773;
    const double ah=1+kh/qh+kh*kh;
    const std::array<double,5> h{1,-2,1,2*(kh*kh-1)/ah,(1-kh/qh+kh*kh)/ah};
    const auto z=std::exp(std::complex<double>(0,-2*pi*frequency/rate));
    auto response=[&](const std::array<double,5>& c) {
        return (c[0]+c[1]*z+c[2]*z*z)/(1.+c[3]*z+c[4]*z*z);
    };
    return 20*std::log10(std::abs(response(s)*response(h)));
}
void filterAndSchedule() {
    const auto actualShelf=Core::shelfCoefficients(48000);
    const auto actualHigh=Core::highPassCoefficients(48000);
    for (std::size_t i=0;i<5;++i) {
        near(actualShelf[i],shelf48[i],2e-12,"published shelf coefficient");
        near(actualHigh[i],highpass48[i],2e-12,"published RLB coefficient");
    }
    constexpr std::array<int,9> rates{32000,44100,48000,88200,96000,176400,192000,352800,384000};
    for (int rate:rates) {
        Core core;
        check(core.prepare(rate,1),"prepare rate");
        const std::size_t frames=static_cast<std::size_t>(rate)*31/10;
        std::vector<float> mono(frames);
        for (std::size_t n=0;n<frames;++n)
            mono[n]=static_cast<float>(std::sin(2*pi*1000*n/rate));
        const float* input[]={mono.data()};
        std::size_t offset=0;
        std::uint32_t rng=0x12345678u;
        while (offset<frames) {
            rng=rng*1664525u+1013904223u;
            const auto count=std::min(frames-offset,static_cast<std::size_t>(1+rng%1097));
            const float* block[]={input[0]+offset};
            core.process(block,count);
            core.drain();
            offset+=count;
        }
        const auto& s=core.summary();
        check(s.momentaryValid&&s.shortValid&&s.integratedValid&&s.rangeValid,"full windows valid");
        check(s.momentaryBlocks==28 && s.shortBlocks==2,"sample-grid window counts");
        check(s.lastEndSample==frames,"last exact timestamp");
        check(s.lraUnstable,"LRA first minute quality flag");
        const double expected=-.691+spectralGain(rate,1000)-10*std::log10(2.);
        near(s.momentary,expected,.015,"rate-specific mono settled K sine");
        near(s.shortTerm,expected,.015,"rate-specific short K sine");
        check(core.allocatedBytes()<=24u*1024u*1024u,"24 MiB analyser budget");
    }
}
void partitionAndStereo() {
    constexpr int rate=48000;
    std::vector<double> left(rate*4),right(rate*4);
    for (std::size_t n=0;n<left.size();++n) {
        left[n]=.1*std::sin(2*pi*1000*n/rate);
        right[n]=-.1*std::sin(2*pi*1000*n/rate);
    }
    Core whole,part;
    check(whole.prepare(rate,2)&&part.prepare(rate,2),"stereo prepare");
    const double* input[]={left.data(),right.data()};
    whole.process(input,left.size()); whole.drain();
    std::size_t offset=0;
    std::uint32_t rng=0x8f197a13u;
    while (offset<left.size()) {
        rng=rng*1664525u+1013904223u;
        const auto count=std::min(left.size()-offset,static_cast<std::size_t>(1+rng%877));
        const double* block[]={left.data()+offset,right.data()+offset};
        part.process(block,count); part.drain(); offset+=count;
    }
    // A four-second block deliberately overruns the 25.6 s queue only if the configured
    // queue is reduced; normal capacity holds all 40 points.
    const auto& a=whole.summary(); const auto& b=part.summary();
    check(!a.analysisOverrun&&!b.analysisOverrun,"normal queue coverage");
    check(a.momentaryBlocks==b.momentaryBlocks&&a.shortBlocks==b.shortBlocks,"partition counts");
    near(a.momentary,b.momentary,1e-9,"partition M");
    near(a.shortTerm,b.shortTerm,1e-9,"partition S");
    near(a.integrated,b.integrated,1e-9,"partition I");
    near(a.range,b.range,1e-9,"partition LRA");
    const double expected=-.691+spectralGain(rate,1000)-20;
    near(a.shortTerm,expected,.02,"stereo powers, no waveform cancellation");
}
std::pair<std::array<double,5>,std::array<double,5>> referenceCoefficients(int rate) {
    const double ks=std::tan(pi*1681.974450955533/rate), qs=.7071752369554196;
    const double vh=std::pow(10.,3.99984385397/20.);
    const double vb=std::pow(vh,.4996667741545416);
    const double as=1+ks/qs+ks*ks;
    const std::array<double,5> shelf{(vh+vb*ks/qs+ks*ks)/as,2*(ks*ks-vh)/as,
        (vh-vb*ks/qs+ks*ks)/as,2*(ks*ks-1)/as,(1-ks/qs+ks*ks)/as};
    const double kh=std::tan(pi*38.13547087602444/rate), qh=.5003270373238773;
    const double ah=1+kh/qh+kh*kh;
    const std::array<double,5> highpass{1,-2,1,2*(kh*kh-1)/ah,
        (1-kh/qh+kh*kh)/ah};
    return {shelf,highpass};
}
void dynamicRateLayoutOracle() {
    constexpr std::array<int,9> rates{32000,44100,48000,88200,96000,176400,192000,352800,384000};
    for (int channels : {1,2}) for (int rate : rates) {
        Core core;
        check(core.prepare(rate,channels),"dynamic oracle prepare");
        const auto coefficients=referenceCoefficients(rate);
        std::array<std::array<RefFilter,2>,2> filters{{
            {{{coefficients.first},{coefficients.second}}},
            {{{coefficients.first},{coefficients.second}}}
        }};
        const int hop=rate/10;
        const std::size_t total=static_cast<std::size_t>(rate)*12;
        std::vector<double> hopPowers,momentaryPowers,shortPowers;
        double hopEnergy=0;
        int hopFill=0;
        double expectedMomentary=0,expectedShort=0,expectedMomentaryMax=-1,expectedShortMax=-1;
        std::size_t offset=0;
        std::uint32_t rng=0x8f12a73bu;
        while (offset<total) {
            rng=rng*1664525u+1013904223u;
            const auto frames=std::min(total-offset,static_cast<std::size_t>(1+rng%997));
            std::vector<float> left(frames),right(frames);
            for (std::size_t local=0;local<frames;++local) {
                const auto frame=offset+local;
                const double seconds=static_cast<double>(frame)/rate;
                const double amplitude=seconds<2 ? .1 : (seconds<5 ? .03 :
                    (seconds<7 ? 0.0 : (seconds<9 ? .2 : .05)));
                left[local]=static_cast<float>(amplitude*std::sin(2*pi*997*frame/rate+.11));
                right[local]=static_cast<float>(.73*amplitude*std::sin(2*pi*997*frame/rate+.47));
                double framePower=0;
                for (int channel=0;channel<channels;++channel) {
                    const double input=channel==0?left[local]:right[local];
                    const double shelf=filters[static_cast<std::size_t>(channel)][0].step(input);
                    const double weighted=filters[static_cast<std::size_t>(channel)][1].step(shelf);
                    framePower+=weighted*weighted;
                }
                hopEnergy+=framePower;
                if (++hopFill==hop) {
                    hopPowers.push_back(hopEnergy/hop); hopEnergy=0; hopFill=0;
                    if (hopPowers.size()>=4) {
                        double value=0;
                        for (std::size_t i=hopPowers.size()-4;i<hopPowers.size();++i)
                            value+=hopPowers[i]/4;
                        momentaryPowers.push_back(value); expectedMomentary=value;
                        expectedMomentaryMax=std::max(expectedMomentaryMax,value);
                    }
                    if (hopPowers.size()>=30) {
                        double value=0;
                        for (std::size_t i=hopPowers.size()-30;i<hopPowers.size();++i)
                            value+=hopPowers[i]/30;
                        shortPowers.push_back(value); expectedShort=value;
                        expectedShortMax=std::max(expectedShortMax,value);
                    }
                }
            }
            const float* input[]={left.data(),right.data()};
            core.process(input,frames); core.drain(); offset+=frames;
        }
        const auto& summary=core.summary();
        check(summary.momentaryValid&&summary.shortValid&&summary.integratedValid
                  && summary.rangeValid,"dynamic oracle valid metrics");
        near(summary.momentary,lufs(expectedMomentary),1e-9,"dynamic M exact-list oracle");
        near(summary.shortTerm,lufs(expectedShort),1e-9,"dynamic S exact-list oracle");
        near(summary.momentaryMax,lufs(expectedMomentaryMax),1e-9,"dynamic M max oracle");
        near(summary.shortMax,lufs(expectedShortMax),1e-9,"dynamic S max oracle");
        near(summary.integrated,integrated(momentaryPowers),1e-9,"dynamic I gate oracle");
        near(summary.range,range(shortPowers),1e-9,"dynamic LRA gate/rank oracle");
    }
}
void exactStatistics() {
    Core core;
    check(core.prepare(48000,1,{100,20,4,48000}),"fixture prepare");
    check(!core.summary().integratedValid,"startup I unavailable");
    const std::vector<double> adversarial{90.9991,1.0001,1.0001,1.0001,1.0001,
                                           1.0001,1.0001,1.0001,1.0001,1.0001};
    for (double value:adversarial) check(core.addMomentaryPower(value),"adversarial insertion");
    core.recomputeStatistics();
    near(core.summary().integrated,integrated(adversarial),1e-11,"exact cut-bin correction");
    near(core.summary().integrated,9.309,1e-10,"adversarial 9.309 anchor");
    Core gate;
    check(gate.prepare(48000,1,{200,20,4,48000}),"gate prepare");
    std::vector<double> i{1,.01};
    for (double v:i) gate.addMomentaryPower(v);
    gate.addMomentaryPower(power(-70)); // strict absolute exclusion
    gate.recomputeStatistics();
    near(gate.summary().integrated,-.691,1e-12,"two-stage I gate");
    gate.addMomentaryPower(100);
    i.push_back(100);
    gate.recomputeStatistics();
    near(gate.summary().integrated,integrated(i),1e-10,"prior blocks re-gated");
    Core lra;
    check(lra.prepare(48000,1,{500,20,4,48000}),"LRA fixture prepare");
    const std::vector<double> ranks{power(-70),power(-30),power(-20),power(-10)};
    for (double v:ranks) lra.addShortPower(v);
    lra.recomputeStatistics();
    near(lra.summary().range,range(ranks),1e-10,"inclusive gate and nearest rank");
    check(lra.summary().rangeValid,"LRA with eligible powers");
    Core sections;
    check(sections.prepare(48000,1,{500,20,4,48000}),"section prepare");
    std::vector<double> energies;
    for (int end=30;end<=400;++end) {
        const int first=std::max(0,std::min(end,200)-std::max(end-30,0));
        const int second=std::max(0,std::min(end,400)-std::max(end-30,200));
        energies.push_back((first*power(-20)+second*power(-30))/30);
        sections.addShortPower(energies.back());
    }
    sections.recomputeStatistics();
    near(sections.summary().range,range(energies),1e-9,"ideal sections exact oracle");
    near(sections.summary().range,10,1,"nominal 10 LU sections");
}
void flagsAndLimits() {
    Core startup;
    check(startup.prepare(48000,1,{100,10,8,48000}),"startup prepare");
    std::vector<float> zeros(48000*3);
    const float* z[]={zeros.data()};
    startup.process(z,19199); startup.drain();
    check(!startup.summary().momentaryValid,"M unavailable before exact first endpoint");
    const float* next[]={zeros.data()+19199};
    startup.process(next,1); startup.drain();
    check(startup.summary().momentaryValid&&startup.summary().lastEndSample==19200,
          "M first endpoint inclusive of last sample");
    Core silence;
    check(silence.prepare(48000,1,{100,10,64,48000*4}),"silence prepare");
    silence.process(z,48000*3); silence.drain();
    const auto& ss=silence.summary();
    check(ss.momentaryValid&&ss.shortValid&&std::isinf(ss.momentary)&&std::isinf(ss.shortTerm),"complete silence valid");
    check(!ss.integratedValid&&!ss.rangeValid,"empty gates unavailable");
    Core invalid;
    check(invalid.prepare(48000,1,{100,10,64,48000*4}),"invalid prepare");
    zeros[100]=std::numeric_limits<float>::quiet_NaN();
    invalid.process(z,zeros.size()); invalid.drain();
    check(invalid.summary().invalidInput&&!invalid.summary().momentaryValid,"nonfinite sticky invalidation");
    invalid.reset(); invalid.drain();
    check(!invalid.summary().invalidInput&&!invalid.summary().momentaryValid,"reset invalidity and windows");
    Core gap;
    check(gap.prepare(48000,1,{100,10,8,48000}),"gap prepare");
    zeros[100]=0;
    gap.process(z,9600); gap.discontinuity(); gap.process(z,19200); gap.drain();
    check(gap.summary().momentaryBlocks==1&&gap.summary().shortBlocks==0,"segment-local windows");
    check(gap.summary().lastEndSample==28800&&gap.summary().discontinuity,"gap timestamp and flag");
    bool sawGap=false;
    for (std::size_t i=0;i<gap.historyCount();++i) sawGap |= gap.historyData()[i].gapBefore;
    check(sawGap,"history marks discontinuity break");
    Core queue;
    check(queue.prepare(48000,1,{100,10,2,48000}),"small queue prepare");
    queue.process(z,14400); queue.drain();
    check(queue.summary().analysisOverrun&&!queue.summary().momentaryValid,"queue overflow invalidates generation");
    queue.reset(); queue.drain();
    check(!queue.summary().analysisOverrun,"generation reset clears queue flag");
    queue.process(z,9600); queue.drain();
    const float* afterTwo[]={zeros.data()+9600};
    queue.process(afterTwo,9600); queue.drain();
    check(queue.summary().momentaryValid&&queue.summary().momentaryBlocks==1,
          "new-generation first complete block retained");
    Core storage;
    check(storage.prepare(48000,1,{2,2,4,48000}),"small stats prepare");
    check(storage.addMomentaryPower(1)&&storage.addMomentaryPower(2),"capacity filled");
    check(!storage.addMomentaryPower(3)&&storage.summary().statisticsOverflow,"statistics overflow explicit");
    Core ceiling;
    check(ceiling.prepare(48000,1,{2,2,4,48000}),"range ceiling prepare");
    check(ceiling.addMomentaryPower(power(24))&&ceiling.addMomentaryPower(power(60))
              &&!ceiling.summary().statisticsOverflow,
          "values at and above +24 LUFS go to the catch-all top bin, no overflow");
    Core limited;
    check(limited.prepare(48000,1,{20,10,4,4800}),"small capture limit prepare");
    limited.process(z,9600); limited.drain();
    check(limited.summary().captureLimit&&limited.summary().capturedSamples==4800,"exact sample cap");
    Core unsupported;
    check(unsupported.prepare(22050,1,{10,10,4,2205}),"unsupported prepares for transparent host use");
    unsupported.process(z,2205); unsupported.drain();
    check(unsupported.summary().unsupportedRate&&!unsupported.summary().momentaryValid,"unsupported rate flag");
    Core history;
    check(history.prepare(48000,1,{20,2,8,48000}),"short history prepare");
    history.process(z,28800); history.drain();
    check(history.historyCount()==2&&history.historyNextIndex()==0,
          "history ring wraps without losing whole-capture statistics");
    check(history.summary().momentaryBlocks==3,"history eviction retains I blocks");
    Core quality;
    check(quality.prepare(48000,1),"LRA quality prepare");
    for (int i=0;i<20;++i) { quality.process(z,144000); quality.drain(); }
    check(quality.summary().capturedSamples==2880000&&!quality.summary().lraUnstable,
          "LRA quality turns stable at exactly 60 seconds");
}
void concurrentGenerationStress() {
    Core core;
    check(core.prepare(48000,1),"concurrent prepare");
    std::vector<float> ones(19200,0.1f);
    const float* input[]={ones.data()};
    std::atomic<bool> run{true};
    std::atomic<bool> inconsistent{false};
    std::atomic<bool> rejectedTransition{false};
    std::thread worker([&] {
        while (run.load(std::memory_order_acquire)) {
            if (!core.drain(1)) rejectedTransition.store(true,std::memory_order_release);
            const auto& snapshot = core.summary();
            if ((snapshot.integratedValid || snapshot.momentaryValid)
                && snapshot.capturedSamples < 19200)
                inconsistent.store(true,std::memory_order_release);
        }
        for (int i=0;i<4;++i) core.drain(256);
    });
    // Pushes total 199 + 4 = 203 points, under the 256-slot queue, so analysisOverrun cannot occur
    // and the final-generation check is deterministic; the storm still overlaps resets with drains.
    for (int i=0;i<200;++i) {
        core.reset();
        core.process(input,i==199 ? ones.size() : 4800);
    }
    run.store(false,std::memory_order_release);
    worker.join();
    core.drain(256);
    const auto& s=core.summary();
    check(s.generation==core.generation(),"worker has final generation");
    check(!s.analysisOverrun&&s.momentaryBlocks==1&&s.momentaryValid&&s.integratedValid,
          "concurrent resets retain final generation first point");
    check(!inconsistent.load(std::memory_order_acquire),
          "concurrent reset never publishes valid old statistics with reset duration");
    check(rejectedTransition.load(std::memory_order_acquire),
          "stress exercised a reset-overlapped drain publication rejection");
}
void reprepareStartsFreshMonotoneGeneration() {
    Core core;
    check(core.prepare(48000,1,{100,10,4,48000}),"initial reprepare fixture");
    const auto firstGeneration=core.generation();
    std::vector<float> values(19200,0.1f);
    const float* input[]={values.data()};
    // Leave four points pending, including one complete M point, then replace the queue.
    core.process(input,values.size());
    check(core.prepare(44100,1,{100,10,4,44100}),"second reprepare fixture");
    check(core.generation()>firstGeneration,"generation stays monotone across reprepare");
    core.drain();
    check(core.summary().capturedSamples==0 && core.historyCount()==0
              && !core.summary().momentaryValid && !core.summary().analysisOverrun,
          "reprepare drops pending old queue and starts empty");
    std::vector<float> next(17640,0.2f);
    const float* nextInput[]={next.data()};
    core.process(nextInput,next.size()); core.drain();
    check(core.summary().momentaryValid && core.summary().momentaryBlocks==1
              && core.summary().lastEndSample==17640,
          "replacement queue reads only initialized new-rate points");
}

void subnormalFlush() {
    for (int rate : {44100,48000,192000}) {
        Core core;
        check(core.prepare(rate,1),"subnormal test prepare");
        std::vector<float> sine(static_cast<std::size_t>(rate));
        for (std::size_t n=0;n<sine.size();++n)
            sine[n]=static_cast<float>(.5*std::sin(2*pi*1000*n/rate));
        const float* input[]={sine.data()};
        core.process(input,sine.size()); core.drain();
        check(!core.filterStateIsZero(),"filters hold state after a sine");
        const std::vector<float> zeros(static_cast<std::size_t>(rate)*5);
        const float* silence[]={zeros.data()};
        core.process(silence,zeros.size()); core.drain();
        check(core.filterStateIsZero(),"filter states flush to exact zero in digital silence");
    }
}
// Catch-all top bin: powers at or above +24 LUFS stay exact against a brute-force oracle.
void hotStatisticsCatchAll() {
    for (int scenario=0;scenario<2;++scenario) {
        Core core;
        check(core.prepare(48000,1),"catch-all prepare");
        std::vector<double> values;
        std::uint32_t rng=0x9e3779b9u+static_cast<std::uint32_t>(scenario);
        auto next=[&rng]() { rng=rng*1664525u+1013904223u; return static_cast<double>(rng>>8)/16777216.0; };
        for (int i=0;i<6000;++i) {
            const double pick=next();
            const double position=next();
            double level;
            if (scenario==0) level=pick<.1 ? 24+position*20 : -45+position*30;
            else level=pick<.1 ? 55+position*10 : 10+position*10;
            values.push_back(power(level));
        }
        for (double value : values) {
            check(core.addMomentaryPower(value)&&core.addShortPower(value),"hot value accepted");
        }
        core.recomputeStatistics();
        const auto& s=core.summary();
        check(!s.statisticsOverflow&&s.integratedValid&&s.rangeValid,"catch-all statistics valid");
        near(s.integrated,integrated(values),1e-9,"catch-all integrated matches brute force");
        near(s.range,range(values),1e-9,"catch-all LRA matches brute force");
    }
}
}
int main() {
    try {
        filterAndSchedule(); partitionAndStereo(); dynamicRateLayoutOracle();
        exactStatistics(); flagsAndLimits();
        subnormalFlush(); hotStatisticsCatchAll();
        concurrentGenerationStress(); reprepareStartsFreshMonotoneGeneration();
        std::cout << "Production core tests passed against independent analytical/raw-list references.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
