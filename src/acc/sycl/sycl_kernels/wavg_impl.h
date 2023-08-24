#ifndef WAVG_IMPL_KERNEL_H_
#define WAVG_IMPL_KERNEL_H_

#include <vector>
#include <algorithm>
#include <iostream>
#include <limits>
#include <chrono>
#include <cassert>
#include <sycl/sycl.hpp>

#include "src/acc/acc_projectorkernel_impl.h"
#include "src/acc/sycl/sycl_dev.h"
#include "src/acc/sycl/sycl_kernels/sycl_utils.h"
#include "src/acc/sycl/sycl_kernels/wavg_gpu.h"

namespace syclKernels
{

	template<bool REFCTF, bool REF3D, bool DATA3D, int block_sz>
	__attribute__((always_inline))
	inline
	void wavg(
		XFLOAT *g_eulers,
		AccProjectorKernel &projector,
		unsigned long image_size, unsigned long orient_num,
		XFLOAT *g_img_real, XFLOAT *g_img_imag,
		XFLOAT *g_trans_x, XFLOAT *g_trans_y, XFLOAT *g_trans_z,
		XFLOAT *g_weights, XFLOAT *g_ctfs,
		XFLOAT *g_wdiff2s_parts, XFLOAT *g_wdiff2s_AA, XFLOAT *g_wdiff2s_XA,
		unsigned long trans_num, XFLOAT  weight_norm,
		XFLOAT  significant_weight, XFLOAT  part_scale,
		virtualSYCL *devAcc)
	{
		devSYCL *dGPU = dynamic_cast<devSYCL*>(devAcc);
        assert(    trans_num  <= std::numeric_limits<int>::max());
        assert(    image_size <= std::numeric_limits<int>::max());
		assert(    orient_num <= std::numeric_limits<int>::max());
		assert(      block_sz <= dGPU->maxItem[1]);
		assert(block_sz*3 + 9 <= dGPU->localMem);

        auto event = dGPU->syclSubmit
        (
            [&](sycl::handler &cgh) //
            {
				using namespace sycl;
				// accessors to device memory
				local_accessor<XFLOAT, 1> s_parts_acc(range<1>(block_sz), cgh);
				local_accessor<XFLOAT, 1> s_sumAA_acc(range<1>(block_sz), cgh);
				local_accessor<XFLOAT, 1> s_sumXA_acc(range<1>(block_sz), cgh);
				local_accessor<XFLOAT, 1> s_eulers_acc(range<1>(9), cgh);

				cgh.parallel_for
				(
					nd_range<2>( range<2>(orient_num, block_sz), range<2>(1, block_sz) ),
					[=](nd_item<2> nit)
 #if defined(__INTEL_LLVM_COMPILER) && defined(INTEL_SG_SIZE)
					[[intel::reqd_sub_group_size(INTEL_SG_SIZE)]]
 #endif
					{
						syclGpuKernels::sycl_kernel_wavg<REFCTF, REF3D, DATA3D, block_sz>
						( //
							nit,
							const_cast<AccProjectorKernel&>(projector), // Why const_cast is needed for compilation?
							g_eulers, static_cast<int>(image_size), static_cast<int>(orient_num),
							g_img_real, g_img_imag, g_trans_x, g_trans_y, g_trans_z,
							g_weights, g_ctfs,
							g_wdiff2s_parts, g_wdiff2s_AA, g_wdiff2s_XA,
							static_cast<int>(trans_num), weight_norm, significant_weight, part_scale,
							s_parts_acc.get_pointer(),
							s_sumAA_acc.get_pointer(),
							s_sumXA_acc.get_pointer(),
							s_eulers_acc.get_pointer()
						);  // End of sycl_kernel_wavg
					}   // End of cgh.parallel_for Lamda function
				);  // End of cgh.parallel_for
			}   // End of dGPU->syclSubmit Lamda function
		);  // End of dGPU->syclSubmit
	}

} // end of namespace syclKernels

#endif /* WAVG_IMPL_KERNEL_H_ */
