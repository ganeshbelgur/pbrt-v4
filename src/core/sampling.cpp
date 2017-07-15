
/*
    pbrt source code is Copyright(c) 1998-2016
                        Matt Pharr, Greg Humphreys, and Wenzel Jakob.

    This file is part of pbrt.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:

    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
    IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
    TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */


// core/sampling.cpp*
#include "sampling.h"

using gtl::ArraySlice;
using gtl::MutableArraySlice;

namespace pbrt {

// Sampling Function Definitions
void StratifiedSample1D(MutableArraySlice<Float> samples, RNG &rng,
                        bool jitter) {
    Float invNSamples = (Float)1 / samples.size();
    for (size_t i = 0; i < samples.size(); ++i) {
        Float delta = jitter ? rng.UniformFloat() : 0.5f;
        samples[i] = std::min((i + delta) * invNSamples, OneMinusEpsilon);
    }
}

void StratifiedSample2D(MutableArraySlice<Point2f> samp, int nx, int ny,
                        RNG &rng, bool jitter) {
    CHECK_EQ(samp.size(), (size_t)nx * (size_t)ny);
    Float dx = (Float)1 / nx, dy = (Float)1 / ny;
    int offset = 0;
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x, ++offset) {
            Float jx = jitter ? rng.UniformFloat() : 0.5f;
            Float jy = jitter ? rng.UniformFloat() : 0.5f;
            samp[offset].x = std::min((x + jx) * dx, OneMinusEpsilon);
            samp[offset].y = std::min((y + jy) * dy, OneMinusEpsilon);
        }
}

void LatinHypercube(MutableArraySlice<Float> samples, int nDim, RNG &rng) {
    // Generate LHS samples along diagonal
    Float invNSamples = (Float)1 / samples.size();
    for (size_t i = 0; i < samples.size(); ++i)
        for (int j = 0; j < nDim; ++j) {
            Float sj = (i + (rng.UniformFloat())) * invNSamples;
            samples[nDim * i + j] = std::min(sj, OneMinusEpsilon);
        }

    // Permute LHS samples in each dimension
    for (int i = 0; i < nDim; ++i) {
        for (size_t j = 0; j < samples.size(); ++j) {
            size_t other = j + rng.UniformUInt32(samples.size() - j);
            std::swap(samples[nDim * j + i], samples[nDim * other + i]);
        }
    }
}

Point2f RejectionSampleDisk(RNG &rng) {
    Point2f p;
    do {
        p.x = 1 - 2 * rng.UniformFloat();
        p.y = 1 - 2 * rng.UniformFloat();
    } while (p.x * p.x + p.y * p.y > 1);
    return p;
}

Vector3f UniformSampleHemisphere(const Point2f &u) {
    Float z = u[0];
    Float r = SafeSqrt(1 - z * z);
    Float phi = 2 * Pi * u[1];
    return Vector3f(r * std::cos(phi), r * std::sin(phi), z);
}

Float UniformHemispherePdf() { return Inv2Pi; }

Vector3f UniformSampleSphere(const Point2f &u) {
    Float z = 1 - 2 * u[0];
    Float r = SafeSqrt(1 - z * z);
    Float phi = 2 * Pi * u[1];
    return Vector3f(r * std::cos(phi), r * std::sin(phi), z);
}

Float UniformSpherePdf() { return Inv4Pi; }

Point2f UniformSampleDisk(const Point2f &u) {
    Float r = std::sqrt(u[0]);
    Float theta = 2 * Pi * u[1];
    return Point2f(r * std::cos(theta), r * std::sin(theta));
}

Point2f ConcentricSampleDisk(const Point2f &u) {
    // Map uniform random numbers to $[-1,1]^2$
    Point2f uOffset = 2.f * u - Vector2f(1, 1);

    // Handle degeneracy at the origin
    if (uOffset.x == 0 && uOffset.y == 0) return Point2f(0, 0);

    // Apply concentric mapping to point
    Float theta, r;
    if (std::abs(uOffset.x) > std::abs(uOffset.y)) {
        r = uOffset.x;
        theta = PiOver4 * (uOffset.y / uOffset.x);
    } else {
        r = uOffset.y;
        theta = PiOver2 - PiOver4 * (uOffset.x / uOffset.y);
    }
    return r * Point2f(std::cos(theta), std::sin(theta));
}

Float UniformConePdf(Float cosThetaMax) {
    return 1 / (2 * Pi * (1 - cosThetaMax));
}

Vector3f UniformSampleCone(const Point2f &u, Float cosThetaMax) {
    Float cosTheta = (1 - u[0]) + u[0] * cosThetaMax;
    Float sinTheta = SafeSqrt(1 - cosTheta * cosTheta);
    Float phi = u[1] * 2 * Pi;
    return Vector3f(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta,
                    cosTheta);
}

Vector3f UniformSampleCone(const Point2f &u, Float cosThetaMax,
                           const Vector3f &x, const Vector3f &y,
                           const Vector3f &z) {
    Float cosTheta = Lerp(u[0], cosThetaMax, 1.f);
    Float sinTheta = SafeSqrt(1 - cosTheta * cosTheta);
    Float phi = u[1] * 2 * Pi;
    return std::cos(phi) * sinTheta * x + std::sin(phi) * sinTheta * y +
           cosTheta * z;
}

Point2f UniformSampleTriangle(const Point2f &u) {
    Float su0 = std::sqrt(u[0]);
    return Point2f(1 - su0, u[1] * su0);
}

Distribution2D::Distribution2D(ArraySlice<Float> func, int nu, int nv) {
    CHECK_EQ(func.size(), (size_t)nu * (size_t)nv);
    pConditionalV.reserve(nv);
    for (int v = 0; v < nv; ++v) {
        // Compute conditional sampling distribution for $\tilde{v}$
        pConditionalV.push_back(std::make_unique<Distribution1D>(
            ArraySlice<Float>(func, v * nu, nu)));
    }
    // Compute marginal sampling distribution $p[\tilde{v}]$
    std::vector<Float> marginalFunc;
    marginalFunc.reserve(nv);
    for (int v = 0; v < nv; ++v)
        marginalFunc.push_back(pConditionalV[v]->funcInt);
    pMarginal = std::make_unique<Distribution1D>(marginalFunc);
}

}  // namespace pbrt
