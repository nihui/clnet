/*
 * kernel_test.cpp
 *
 *  Created on: 2017/10/5
 *      Author: ZhangHua
 */

#include <iostream>

#include <tensor.hpp>
#include <device_instance.hpp>

using namespace std;
using namespace clnet;

T kernel_test()
{
	T initializer = XavierNormalDistributionInitializer({}, 0, 2.34f);
	int M = optional<int>("M", 2048); //dim_hidden
	int N = optional<int>("N", 512); //batch_size
	int K = optional<int>("K", 2048); //dim_in
	int STEP = optional<int>("step", 4);
	bool parallel = optional<int>("parallel", true);
	T x = Data({N, K}, &initializer, "x");
	T w = Weight({M, K}, "w", &initializer);
	T result = Data({M, N}, nullptr, "gemm");

	auto version1 = new InstantTensor("unroll", {&x, &w}, {}, [](InstantTensor* self, DeviceInstance& I) {}, [](InstantTensor* self, DeviceInstance& I) -> string{
		return std::string{R"CLC(
kernel void gemm(global float* out, const global float* in, const global float* weight, const global float* bias, 
		/*local float* tmp, */const int dim_hidden, const int dim_in)
{
	const int GID = get_global_id(0);
	const int n = GID / dim_hidden;
	const int hidden = GID % dim_hidden;
	const int weight_offset = hidden * dim_in;
	const int in_offset = n * dim_in;
	float z = bias != NULL? bias[hidden] : 0;

#pragma unroll
	for (int i = 0; i < dim_in; i++)
		z += weight[weight_offset + i] * in[in_offset + i];
	out[GID] = z;
}
)CLC"};
	});

	return *new InstantTensor("kernel_test", {&x, &w}, {version1}, [M, N, K, STEP, parallel, &result, &initializer](InstantTensor* self, DeviceInstance& I) {
		auto& kernel = prepare_for_running_kernel(self, I);
		T x = *self->inputs[0];
		T w = *self->inputs[1];
		kernel.setArg(0, I.buffers[&result]);
		kernel.setArg(1, I.buffers[&x]);
		kernel.setArg(2, I.buffers[&w]);
		kernel.setArg(3, nullptr);

		vector<float> baselines;
		for (int m = M; m >= 32; m /= STEP)
			for (int n = N; n >= 8; n /= STEP)
				for (int  k = K; k >= 32; k /= STEP) {
					int64 total = 1LL * M * N * K / m / n / k;
					size_t time = MICROS(0);
					for (int i = 0; i < total; i++) {
						kernel.setArg(4, m);
						kernel.setArg(5, k);
//							cl::NDRange local(2);
						cl::NDRange global(m * n);
						I.queue.enqueueNDRangeKernel(kernel, cl::NullRange, global, cl::NullRange, &I.precondition_events, &I.events[&result]);
//						operate_tensor_data<float>(&result, I, {0, 0}, {2, 4}, result.dimensions);
						if (!parallel)
							wait_for_all_kernels_finished(I);
					}
					if (parallel)
						wait_for_all_kernels_finished(I);
					time = MICROS(time);
					baselines.push_back(time / total / 1000.0f);
					logger << "M=" << m << " \tN=" << n << " \tK=" << k << " \ttimes=" << total << " \tTime="  << time << " \tAverage="  << baselines.back() << "ms" << endl;
				}
	}, [](InstantTensor* self, DeviceInstance& I) -> string{ //This example used as a trial to test OpenCL kernel code
		return std::string{R"CLC(
kernel void gemm(global float* out, const global float* in, const global float* weight, const global float* bias, 
		/*local float* tmp, */const int dim_hidden, const int dim_in)
{
	const int GID = get_global_id(0);
	const int n = GID / dim_hidden;
	const int hidden = GID % dim_hidden;
	const int weight_offset = hidden * dim_in;
	const int in_offset = n * dim_in;
	float z = bias != NULL? bias[hidden] : 0;

//#pragma unroll
	for (int i = 0; i < dim_in; i++)
		z += weight[weight_offset + i] * in[in_offset + i];
	out[GID] = z;
}
)CLC"};
	});
}