/* Copyright 2022-2023 The Regents of the University of California, through Lawrence
 *           Berkeley National Laboratory (subject to receipt of any required
 *           approvals from the U.S. Dept. of Energy). All rights reserved.
 *
 * This file is part of ImpactX.
 *
 * Authors: Alex Bojanich, Chad Mitchell, Axel Huebl
 * License: BSD-3-Clause-LBNL
 */
#include "WakeConvolution.H"
#include "particles/ImpactXParticleContainer.H"

#ifdef ImpactX_USE_FFT
#include <ablastr/math/fft/AnyFFT.H>
#endif

#include <AMReX_REAL.H>

#include <algorithm>
#include <cmath>
#ifndef ImpactX_USE_FFT
#include <stdexcept>
#endif

namespace impactx::particles::wakefields
{
    amrex::Real alpha (amrex::Real s)
    {
        using namespace amrex::literals;

        return 1_rt - impactx::particles::wakefields::alpha_1 * std::sqrt(s) - (1_rt - 2_rt * impactx::particles::wakefields::alpha_1) * s;
    }

    amrex::Real w_t_rf (
        amrex::Real s,
        amrex::Real a,
        amrex::Real g,
        amrex::Real L
    )
    {
        using namespace amrex::literals;

        amrex::Real const s0 = (0.169_rt * std::pow(a, 1.79_rt) * std::pow(g, 0.38_rt)) / std::pow(L, 1.17_rt);
        amrex::Real const term = std::sqrt(std::abs(s) / s0) * std::exp(-std::sqrt(std::abs(s) / s0));
        return (4 * impactx::particles::wakefields::Z0 * ablastr::constant::SI::c * s0 * unit_step(s)) / (amrex::Real(M_PI) * std::pow(a, 4)) * term;
    }

    amrex::Real w_l_rf (
        amrex::Real s,
        amrex::Real a,
        amrex::Real g,
        amrex::Real L
    )
    {
        using namespace amrex::literals;

        amrex::Real const s00 = g * std::pow((a / (alpha(g / L) * L)), 2) / 8.0_rt;
        return (impactx::particles::wakefields::Z0 * ablastr::constant::SI::c * unit_step(s) * std::exp(-std::sqrt(std::abs(s) / s00))) / (amrex::Real(M_PI) * std::pow(a, 2));
    }

    void convolve_fft (
        amrex::Gpu::DeviceVector<amrex::Real> const & beam_profile_slope,
        amrex::Gpu::DeviceVector<amrex::Real> const & wake_func,
        amrex::Real delta_t,
        amrex::Gpu::DeviceVector<amrex::Real> & result,
        int padding_factor
    )
    {
    #ifdef ImpactX_USE_FFT
        int const beam_profile_slope_size = beam_profile_slope.size();
        int const wake_func_size = wake_func.size();

        // Length of convolution result
        int const original_n = beam_profile_slope_size + wake_func_size - 1;  // Output size is n = 2N - 1, where N = size of signals 1,2

        // Add padding factor to control amount of zero-padding
        int const n = static_cast<int>(original_n * padding_factor);

        // Allocate memory for FFT inputs and outputs
        using ablastr::math::anyfft::Complex;

        // Allocate memory for 'n' real numbers for inputs and complex outputs
        amrex::Gpu::DeviceVector<amrex::Real> in1(n);
        amrex::Gpu::DeviceVector<amrex::Real> in2(n);
        amrex::Gpu::DeviceVector<Complex> out1(n);
        amrex::Gpu::DeviceVector<Complex> out2(n);
        amrex::Gpu::DeviceVector<Complex> conv_result(n);
        amrex::Gpu::DeviceVector<amrex::Real> out3(n);

        // Zero-pad the input arrays to be the size of the convolution output length 'n'
        amrex::Real * const dptr_in1 = in1.data();
        amrex::Real * const dptr_in2 = in2.data();
        amrex::Real const * const dptr_beam_profile_slope = beam_profile_slope.data();
        amrex::Real const * const dptr_wake_func = wake_func.data();
        amrex::ParallelFor(n, [=] AMREX_GPU_DEVICE(int i)
        {
            if (i < beam_profile_slope_size)
            {
                dptr_in1[i] = std::isfinite(dptr_beam_profile_slope[i]) ? dptr_beam_profile_slope[i] : 0;  // Print NaN was produced if 0
            }
            else
            {
                dptr_in1[i] = 0;
            }

            if (i < wake_func_size)
            {
                dptr_in2[i] = std::isfinite(dptr_wake_func[i]) ? dptr_wake_func[i] : 0;  // Print NaN was produced if 0
            }
            else
            {
                dptr_in2[i] = 0;
            }
        });

        // Define Forward FFT
        // TODO: n does not change usually, so we can keep the plans alive over the simulation
        //       runtime. To do that, we can make this function a functor class.
        auto p1 = ablastr::math::anyfft::CreatePlan(
            amrex::IntVect{n}, in1.data(), out1.data(), ablastr::math::anyfft::direction::R2C, 1

        );
        auto p2 = ablastr::math::anyfft::CreatePlan(
            amrex::IntVect{n}, in2.data(), out2.data(), ablastr::math::anyfft::direction::R2C, 1
        );

        // Perform Forward FFT - Convert inputs into frequency domain
        // Gives out1,out2, the FFT-transformed input arrays of in1, in2
        ablastr::math::anyfft::Execute(p1);
        ablastr::math::anyfft::Execute(p2);

        // Perform FFT Multiplication - FFT Element-wise multiplication in frequency space
        Complex * const dptr_conv_result = conv_result.data();
        Complex const * const dptr_out1 = out1.data();
        Complex const * const dptr_out2 = out2.data();
        amrex::ParallelFor(n, [=] AMREX_GPU_DEVICE (int i) noexcept
        {
            using ablastr::math::anyfft::multiply;
            multiply(dptr_conv_result[i], dptr_out1[i], dptr_out2[i]);
        });

        // Define Backward FFT - Revert from frequency domain to time/space domain
        // TODO: n does not change usually, so we can keep the plans alive over the simulation
        //       runtime. To do that, we can make this function a functor class.
        amrex::Real * const dptr_out3 = out3.data();
        auto p3 = ablastr::math::anyfft::CreatePlan(
            amrex::IntVect{n}, dptr_out3, dptr_conv_result, ablastr::math::anyfft::direction::C2R, 1
        );

        // Perform Backward FFT
        ablastr::math::anyfft::Execute(p3);

        // Normalize result by the output size and multiply result by bin size
        amrex::Real * const dptr_result = result.data();
        amrex::ParallelFor(n, [=] AMREX_GPU_DEVICE (int i) noexcept
        {
            dptr_result[i] = dptr_out3[i] / n * delta_t;
        });

        // Clean up intermediate declarations
        // TODO: n does not change usually, so we can keep the plans alive over the simulation
        //       runtime. To do that, we can make this function a functor class.
        ablastr::math::anyfft::DestroyPlan(p1);
        ablastr::math::anyfft::DestroyPlan(p2);
        ablastr::math::anyfft::DestroyPlan(p3);
    #else
        throw std::runtime_error("convolve_fft: To use this function, recompile with ImpactX_FFT=ON.");
    #endif
    }
}
