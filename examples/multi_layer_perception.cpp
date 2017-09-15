/*
 * multi_layer_perception.cpp
 *
 *  Created on: 2017/1/31
 *      Author: ZhangHua
 */

#include <iostream>
#include <vector>
#include <functional>

#include <tensor.hpp>
#include <device_instance.hpp>

using namespace std;
using namespace clnet;

struct MLPMonitor : Tensor {

	MLPMonitor(Tensor& loss) { //loss should be put in peers because it should not be run again
		peers.push_back(&loss);
	}
	virtual void run(DeviceInstance& I) override {
		auto optimizer = static_cast<type::IterativeOptimizer*>(peers[1]);
		auto epoch = optimizer->current_epoch(I);
		if (epoch % 2000 != 0)
			return;

		size_t duration = optimizer->milliseconds_since_last(I);
		int n = peers[0]->peers[0]->dimensions[0] * 2000;
		string speed = epoch == 0? to_string(duration) + "ms" : to_string(int(1000.0f * n / duration)) + "/s";
		cout << "[" << I.ID << "," << epoch << "," << speed << "] error rate: " << static_cast<back::Loss*>(peers[0])->L(I)  << endl;
	}
};

T MLP()
{
	const int K = 2, N = 128, HIDDEN = 4096, max_iters = 10001;
	auto generator = new InstantTensor("data_generator", {}, {}, [](InstantTensor* self, DeviceInstance& I) {
		float *x = I.pointers[self->peers[0]], *y = I.pointers[self->peers[1]]; //peers[2]: MiniBatch
		int N = self->peers[0]->dimensions[0];
		int K = self->peers[0]->dimensions[1];
		for (int i = 0; i < N; i++) {
			for (int j = 0; j < K; j++) {
				float value;
				while ((value = rand()) == 0);
				x[i * K + j] = (1 + 3.0f * value / RAND_MAX);
			}
			y[i] = x[i * K] / x[i * K + 1];
		}
	});
	T X = Data({N, K}, generator, "X");
	T Y = Data({N}, generator, "Y");

	T l0_weight = Weight({HIDDEN, K}, "l0_weight");
	T l0_bias = Bias({HIDDEN}, "l0_bias");
	T l1_weight = Weight({1, HIDDEN}, "l1_weight");
	T l1_bias = Bias({1}, "l1_bias");

//	T layer0 = sigmoid(l0_weight * X + l0_bias);
//	T layer1 = softrelu(l1_weight * l0_output + l1_bias);
	T l0_output = FullyConnectedLayer(X, l0_weight, &l0_bias, "sigmoid", "FCLayer_0");
	T l1_output = FullyConnectedLayer(l0_output, l1_weight, &l1_bias, "softrelu", "FCLayer_1");
	T loss = LinearRegressionLoss(l1_output, Y);
	T SGD = StochasticGradientDescentUpdater(loss, 0.00001, 0);

	T initializer = XavierNormalDistributionInitializer(SGD, 0, 2.34f);
	auto monitor = new MLPMonitor(loss);
	return IterativeOptimizer({&initializer}, {&SGD, generator, monitor}, max_iters);
}

T MLP_softmax()
{
	const int P = 28, N = 128, K = 10, max_iters = 20000;
	T sym_x = *new Tensor({N, P}, {}, "X");
	T sym_label = *new Tensor({N}, {}, "label");
	auto generator = new InstantTensor("data_generator", {}, {&sym_x, &sym_label}, [P, N, K](InstantTensor* self, DeviceInstance& I) {
		float *aptr_x = I.pointers[self->peers[0]], *aptr_y = I.pointers[self->peers[1]];
		for (int i = 0; i < N; i++) {
			float sum = 0;
			for (int j = 0; j < P; j++) {
				aptr_x[i * P + j] = i % K * 1.0f;//1.0f * rand() / RAND_MAX; //
				sum += aptr_x[i * P + j];
			}
			aptr_y[i] = i % K;//(int) (sum / P * K); //
		}
		const vector<cl::Event> async;
		self->peers[0]->download(I, &async);
		self->peers[1]->download(I, &async);
	});

	vector<int> layerSizes({512, K});
	int nLayers = layerSizes.size();
	vector<Tensor*> params(nLayers * 2);
	vector<Tensor*> outputs(nLayers);

	for (int i = 0; i < nLayers; i++) {
		string istr = to_string(i);
		params[i] = &Weight({layerSizes[i], i == 0? P : params[i - 1]->dimensions[0]}, string("w") + istr);
		params[i + nLayers] = &Bias({layerSizes[i]}, string("b") + istr);
		outputs[i] = &FullyConnectedLayer(i == 0? sym_x : *outputs[i-1], *params[i], params[i + nLayers], "leakyrelu", string("leaky_fc") + istr);
	}
	T loss = SoftmaxLoss(*outputs[nLayers - 1], sym_label);
	T SGD = StochasticGradientDescentUpdater(loss, 0.0001, 0);

	auto initializer = new InstantTensor("simple_initializer", {}, params, [nLayers](InstantTensor* self, DeviceInstance& I) {
		const vector<cl::Event> async;
		for (int i = 0; i < nLayers; i++) {
			auto weight = self->peers[i], bias = self->peers[i + nLayers];
			auto p = I.pointers[weight];
			for (int n = weight->volume; n > 0; n--)
				*p++ = 0.5f;
			weight->download(I, &async);
			memset(I.pointers[bias], 0, bias->size);
			bias->download(I, &async);
		}
	});

	auto softmax_output = loss.peers[1];
	auto monitor = new InstantTensor("monitor", {}, {}, [K, N, softmax_output, &sym_label](InstantTensor* self, DeviceInstance& I) {
		auto iter = static_cast<type::IterativeOptimizer*>(self->peers[0])->current_epoch(I);
		if (iter % 1000 == 0) {
			cout << "epoch " << iter << endl;

			softmax_output->upload(I);
			auto pred = I.pointers[softmax_output];
			auto target = I.pointers[const_cast<Tensor*>(&sym_label)];
			int right = 0;
			for (int i = 0; i < N; ++i) {
				float mx_p = pred[i * K + 0];
				float p_y = 0;
				for (int j = 0; j < K; ++j) {
					if (pred[i * K + j] > mx_p) {
						mx_p = pred[i * K + j];
						p_y = j;
					}
				}
				if (p_y == target[i]) right++;
			}
			cout << "Accuracy: " << 1.0f * right / N << endl;
		}
	});
	return IterativeOptimizer({initializer, generator}, {&SGD, generator, monitor}, max_iters);
}