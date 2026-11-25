#pragma once

#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gasfree {

// Decoded stereo impulse response (loaded/generated on non-RT thread)
struct ImpulseBuffer {
    std::vector<float> left;
    std::vector<float> right;
    double sampleRate = 48000.0;
};

// Minimal PCM/IEEE-float WAV loader + linear resampler for reverb impulses
bool loadWavFile(const std::string& path, ImpulseBuffer& out, double targetSampleRate);

// Generate synthetic plate impulse matching devices/reverb.js WR.makePlate()
ImpulseBuffer makeSyntheticPlate(double sampleRate, double seconds = 2.6);

// Pre-computed frequency-domain partitioned impulse (built off audio thread,
// swapped into UniformPartitionedConvolver with zero allocations on audio thread)
struct PreparedConvolverIR {
    size_t blockSize = 256;
    size_t fftSize = 512;
    size_t numPartitions = 0;
    // Per channel: numPartitions * (fftSize/2 + 1) complex bins
    std::vector<std::complex<float>> irL;
    std::vector<std::complex<float>> irR;
};

std::shared_ptr<const PreparedConvolverIR> prepareConvolverIR(const ImpulseBuffer& ir, size_t blockSize = 256);

// Uniform partitioned overlap-save stereo FFT convolver (RT-safe once initialized)
class StereoPartitionedConvolver {
public:
    explicit StereoPartitionedConvolver(size_t blockSize = 256);

    // Install a precomputed IR (RT-safe: resizes only if partition count exceeds preallocated max)
    void setIR(std::shared_ptr<const PreparedConvolverIR> ir);

    void reset() noexcept;

    // Process one stereo sample (buffers internally in blockSize chunks, zero-latency
    // direct-partitioned output after 1 block = 256 samples, or zero-latency first partition)
    void processSample(float inL, float inR, float& outL, float& outR) noexcept;

private:
    void processPartitionBlock() noexcept;

    size_t blockSize_;
    size_t fftSize_;
    size_t numBins_;
    static constexpr size_t kMaxPartitions = 1024; // up to ~5.4s @ 48kHz with block=256 (or 10.9s with block=512)

    std::shared_ptr<const PreparedConvolverIR> ir_;

    // Input/output staging buffers
    std::vector<float> inBufL_, inBufR_;
    std::vector<float> outBufL_, outBufR_;
    std::vector<float> overlapInL_, overlapInR_; // previous blockSize samples for overlap-save
    size_t pos_ = 0;

    // Frequency-domain delay line (FDL): ring of complex spectra
    std::vector<std::complex<float>> fdlL_, fdlR_;
    size_t fdlPos_ = 0;

    // Scratch buffers for FFT / accumulation
    std::vector<std::complex<float>> fftWork_;
    std::vector<std::complex<float>> accumL_, accumR_;
};

// In-place iterative radix-2 Cooley-Tukey FFT
void fftRadix2(std::vector<std::complex<float>>& a, bool inverse) noexcept;

} // namespace gasfree
