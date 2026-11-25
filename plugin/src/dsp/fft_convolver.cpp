#include "fft_convolver.h"
#include "dsp_common.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace gasfree {

void fftRadix2(std::vector<std::complex<float>>& a, bool inverse) noexcept {
    const size_t n = a.size();
    if (n <= 1) return;

    // Bit-reversal permutation
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }

    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = static_cast<float>(TWO_PI / static_cast<double>(len)) * (inverse ? 1.0f : -1.0f);
        const std::complex<float> wlen(std::cos(ang), std::sin(ang));
        const size_t half = len >> 1;
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t j = 0; j < half; ++j) {
                const auto u = a[i + j];
                const auto v = a[i + j + half] * w;
                a[i + j] = u + v;
                a[i + j + half] = u - v;
                w *= wlen;
            }
        }
    }

    if (inverse) {
        const float invN = 1.0f / static_cast<float>(n);
        for (auto& x : a) x *= invN;
    }
}

static uint16_t readU16LE(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static uint32_t readU32LE(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static std::vector<float> resampleLinear(const std::vector<float>& src, double srcSr, double dstSr) {
    if (src.empty() || srcSr <= 0.0 || dstSr <= 0.0 || std::abs(srcSr - dstSr) < 1.0)
        return src;
    const double ratio = srcSr / dstSr;
    const size_t outLen = static_cast<size_t>(std::ceil(static_cast<double>(src.size()) / ratio));
    std::vector<float> dst(outLen);
    for (size_t i = 0; i < outLen; ++i) {
        const double pos = static_cast<double>(i) * ratio;
        const size_t idx = static_cast<size_t>(pos);
        const float frac = static_cast<float>(pos - static_cast<double>(idx));
        const float a = idx < src.size() ? src[idx] : 0.0f;
        const float b = (idx + 1) < src.size() ? src[idx + 1] : a;
        dst[i] = a + (b - a) * frac;
    }
    return dst;
}

bool loadWavFile(const std::string& path, ImpulseBuffer& out, double targetSampleRate) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto sz = f.tellg();
    if (sz < 44) return false;
    f.seekg(0);
    std::vector<uint8_t> raw(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(raw.data()), sz)) return false;

    if (std::memcmp(raw.data(), "RIFF", 4) != 0 || std::memcmp(raw.data() + 8, "WAVE", 4) != 0)
        return false;

    uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0;
    uint32_t wavSampleRate = 0;
    const uint8_t* dataPtr = nullptr;
    uint32_t dataSize = 0;

    size_t off = 12;
    while (off + 8 <= raw.size()) {
        const uint32_t chunkSize = readU32LE(raw.data() + off + 4);
        const uint8_t* body = raw.data() + off + 8;
        if (off + 8 + chunkSize > raw.size()) break;

        if (std::memcmp(raw.data() + off, "fmt ", 4) == 0 && chunkSize >= 16) {
            audioFormat = readU16LE(body);
            numChannels = readU16LE(body + 2);
            wavSampleRate = readU32LE(body + 4);
            bitsPerSample = readU16LE(body + 14);
            if (audioFormat == 0xFFFE && chunkSize >= 40) {
                // WAVE_FORMAT_EXTENSIBLE: subformat GUID first 2 bytes
                audioFormat = readU16LE(body + 24);
            }
        } else if (std::memcmp(raw.data() + off, "data", 4) == 0) {
            dataPtr = body;
            dataSize = chunkSize;
        }
        off += 8 + chunkSize + (chunkSize & 1u);
    }

    if (!dataPtr || numChannels == 0 || wavSampleRate == 0 || bitsPerSample == 0)
        return false;

    const size_t bytesPerSample = bitsPerSample / 8;
    const size_t frameBytes = bytesPerSample * numChannels;
    if (frameBytes == 0) return false;
    const size_t numFrames = dataSize / frameBytes;

    std::vector<float> chL(numFrames), chR(numFrames);

    auto decodeSample = [&](const uint8_t* p) -> float {
        if (audioFormat == 3 && bitsPerSample == 32) {
            float v;
            std::memcpy(&v, p, 4);
            return v;
        }
        if (audioFormat == 1) {
            if (bitsPerSample == 16) {
                int16_t v = static_cast<int16_t>(readU16LE(p));
                return static_cast<float>(v) / 32768.0f;
            }
            if (bitsPerSample == 24) {
                int32_t v = (static_cast<int32_t>(p[0]) | (static_cast<int32_t>(p[1]) << 8) |
                             (static_cast<int32_t>(static_cast<int8_t>(p[2])) << 16));
                return static_cast<float>(v) / 8388608.0f;
            }
            if (bitsPerSample == 32) {
                int32_t v = static_cast<int32_t>(readU32LE(p));
                return static_cast<float>(static_cast<double>(v) / 2147483648.0);
            }
        }
        return 0.0f;
    };

    for (size_t i = 0; i < numFrames; ++i) {
        const uint8_t* frame = dataPtr + i * frameBytes;
        const float l = decodeSample(frame);
        const float r = (numChannels >= 2) ? decodeSample(frame + bytesPerSample) : l;
        chL[i] = l;
        chR[i] = r;
    }

    // Match WebAudio ConvolverNode default equal-power normalization so levels match gasfree.app
    double sumSq = 0.0;
    for (size_t i = 0; i < numFrames; ++i) {
        sumSq += static_cast<double>(chL[i]) * chL[i] + static_cast<double>(chR[i]) * chR[i];
    }
    const double rms = std::sqrt(sumSq / std::max<double>(1.0, static_cast<double>(numFrames) * 2.0));
    if (rms > 1e-7) {
        // WebAudio ConvolverNode uses gainCalibration = 0.00125 -> scale ≈ 0.00125 / rms
        // (matching AudioBuffer normalization in browsers)
        const float norm = static_cast<float>(std::min(1.0, 0.015 / rms));
        for (size_t i = 0; i < numFrames; ++i) {
            chL[i] *= norm;
            chR[i] *= norm;
        }
    }

    out.left = resampleLinear(chL, static_cast<double>(wavSampleRate), targetSampleRate);
    out.right = resampleLinear(chR, static_cast<double>(wavSampleRate), targetSampleRate);
    out.sampleRate = targetSampleRate;
    return !out.left.empty();
}

ImpulseBuffer makeSyntheticPlate(double sampleRate, double seconds) {
    ImpulseBuffer out;
    out.sampleRate = sampleRate;
    const size_t n = static_cast<size_t>(std::llround(sampleRate * seconds));
    out.left.resize(n);
    out.right.resize(n);
    FastRng rng;
    rng.seed(0x504C4154u); // "PLAT"

    double sumSq = 0.0;
    for (int c = 0; c < 2; ++c) {
        auto& data = (c == 0) ? out.left : out.right;
        double y = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(n);
            const double a = 0.85 - 0.75 * t;
            y += a * (rng.nextBipolar() - y);
            const double env = std::exp(-6.9 * t) * std::min(1.0, static_cast<double>(i) / (sampleRate * 0.002));
            const float v = static_cast<float>(y * env);
            data[i] = v;
            sumSq += static_cast<double>(v) * v;
        }
    }
    const double rms = std::sqrt(sumSq / std::max<double>(1.0, static_cast<double>(n) * 2.0));
    if (rms > 1e-7) {
        const float norm = static_cast<float>(std::min(1.0, 0.015 / rms));
        for (size_t i = 0; i < n; ++i) {
            out.left[i] *= norm;
            out.right[i] *= norm;
        }
    }
    return out;
}

std::shared_ptr<const PreparedConvolverIR> prepareConvolverIR(const ImpulseBuffer& ir, size_t blockSize) {
    auto prep = std::make_shared<PreparedConvolverIR>();
    prep->blockSize = blockSize;
    prep->fftSize = blockSize * 2;
    const size_t maxSamples = blockSize * 512; // cap at ~5.4s @ 48kHz (blockSize=512)
    const size_t len = std::min(ir.left.size(), maxSamples);
    prep->numPartitions = (len + blockSize - 1) / blockSize;
    if (prep->numPartitions == 0) return prep;

    const size_t bins = prep->fftSize;
    prep->irL.assign(prep->numPartitions * bins, {0.0f, 0.0f});
    prep->irR.assign(prep->numPartitions * bins, {0.0f, 0.0f});

    std::vector<std::complex<float>> work(prep->fftSize);
    for (size_t p = 0; p < prep->numPartitions; ++p) {
        const size_t start = p * blockSize;
        const size_t count = std::min(blockSize, len - start);

        // Left
        std::fill(work.begin(), work.end(), std::complex<float>(0.0f, 0.0f));
        for (size_t i = 0; i < count; ++i) work[i] = {ir.left[start + i], 0.0f};
        fftRadix2(work, false);
        std::copy(work.begin(), work.end(), prep->irL.begin() + p * bins);

        // Right
        std::fill(work.begin(), work.end(), std::complex<float>(0.0f, 0.0f));
        for (size_t i = 0; i < count; ++i) work[i] = {ir.right[start + i], 0.0f};
        fftRadix2(work, false);
        std::copy(work.begin(), work.end(), prep->irR.begin() + p * bins);
    }
    return prep;
}

StereoPartitionedConvolver::StereoPartitionedConvolver(size_t blockSize)
    : blockSize_(blockSize), fftSize_(blockSize * 2), numBins_(blockSize * 2) {
    inBufL_.assign(blockSize_, 0.0f);
    inBufR_.assign(blockSize_, 0.0f);
    outBufL_.assign(blockSize_, 0.0f);
    outBufR_.assign(blockSize_, 0.0f);
    overlapInL_.assign(blockSize_, 0.0f);
    overlapInR_.assign(blockSize_, 0.0f);
    fftWork_.assign(fftSize_, {0.0f, 0.0f});
    accumL_.assign(numBins_, {0.0f, 0.0f});
    accumR_.assign(numBins_, {0.0f, 0.0f});
    // Pre-allocate FDL for up to 512 partitions to avoid audio-thread allocations
    fdlL_.assign(512 * numBins_, {0.0f, 0.0f});
    fdlR_.assign(512 * numBins_, {0.0f, 0.0f});
}

void StereoPartitionedConvolver::setIR(std::shared_ptr<const PreparedConvolverIR> ir) {
    if (ir && ir->blockSize == blockSize_) {
        ir_ = std::move(ir);
        reset();
    }
}

void StereoPartitionedConvolver::reset() noexcept {
    std::fill(inBufL_.begin(), inBufL_.end(), 0.0f);
    std::fill(inBufR_.begin(), inBufR_.end(), 0.0f);
    std::fill(outBufL_.begin(), outBufL_.end(), 0.0f);
    std::fill(outBufR_.begin(), outBufR_.end(), 0.0f);
    std::fill(overlapInL_.begin(), overlapInL_.end(), 0.0f);
    std::fill(overlapInR_.begin(), overlapInR_.end(), 0.0f);
    std::fill(fdlL_.begin(), fdlL_.end(), std::complex<float>(0.0f, 0.0f));
    std::fill(fdlR_.begin(), fdlR_.end(), std::complex<float>(0.0f, 0.0f));
    pos_ = 0;
    fdlPos_ = 0;
}

void StereoPartitionedConvolver::processSample(float inL, float inR, float& outL, float& outR) noexcept {
    if (!ir_ || ir_->numPartitions == 0) {
        outL = 0.0f;
        outR = 0.0f;
        return;
    }

    inBufL_[pos_] = inL;
    inBufR_[pos_] = inR;
    outL = outBufL_[pos_];
    outR = outBufR_[pos_];

    if (++pos_ >= blockSize_) {
        pos_ = 0;
        processPartitionBlock();
    }
}

void StereoPartitionedConvolver::processPartitionBlock() noexcept {
    const size_t numParts = std::min<size_t>(ir_->numPartitions, 512);
    if (numParts == 0) return;

    // Overlap-save: [prev blockSize samples, current blockSize samples] -> FFT
    for (size_t i = 0; i < blockSize_; ++i) {
        fftWork_[i] = {overlapInL_[i], 0.0f};
        fftWork_[blockSize_ + i] = {inBufL_[i], 0.0f};
        overlapInL_[i] = inBufL_[i];
    }
    fftRadix2(fftWork_, false);
    std::copy(fftWork_.begin(), fftWork_.end(), fdlL_.begin() + fdlPos_ * numBins_);

    for (size_t i = 0; i < blockSize_; ++i) {
        fftWork_[i] = {overlapInR_[i], 0.0f};
        fftWork_[blockSize_ + i] = {inBufR_[i], 0.0f};
        overlapInR_[i] = inBufR_[i];
    }
    fftRadix2(fftWork_, false);
    std::copy(fftWork_.begin(), fftWork_.end(), fdlR_.begin() + fdlPos_ * numBins_);

    std::fill(accumL_.begin(), accumL_.end(), std::complex<float>(0.0f, 0.0f));
    std::fill(accumR_.begin(), accumR_.end(), std::complex<float>(0.0f, 0.0f));

    // Real-input spectrum has conjugate symmetry: only compute bins 0..fftSize/2 (half the work!)
    const size_t halfBins = (fftSize_ >> 1) + 1;
    for (size_t p = 0; p < numParts; ++p) {
        const size_t slot = (fdlPos_ + 512 - p) & 511u;
        const auto* xL = fdlL_.data() + slot * numBins_;
        const auto* xR = fdlR_.data() + slot * numBins_;
        const auto* hL = ir_->irL.data() + p * numBins_;
        const auto* hR = ir_->irR.data() + p * numBins_;
        for (size_t k = 0; k < halfBins; ++k) {
            accumL_[k] += xL[k] * hL[k];
            accumR_[k] += xR[k] * hR[k];
        }
    }
    // Mirror Hermitian conjugate for bins halfBins..fftSize-1 before IFFT
    for (size_t k = 1; k < (fftSize_ >> 1); ++k) {
        accumL_[fftSize_ - k] = std::conj(accumL_[k]);
        accumR_[fftSize_ - k] = std::conj(accumR_[k]);
    }

    fdlPos_ = (fdlPos_ + 1) & 511u;

    // IFFT and take second half (valid linear convolution samples)
    fftRadix2(accumL_, true);
    fftRadix2(accumR_, true);
    for (size_t i = 0; i < blockSize_; ++i) {
        outBufL_[i] = accumL_[blockSize_ + i].real();
        outBufR_[i] = accumR_[blockSize_ + i].real();
    }
}

} // namespace gasfree
