#include "DeepModel.h"

#include <array>
#include <assert.h>
#include <random>
#include <amp_math.h>

namespace deep_learning_lib
{
    using namespace concurrency;

    DataLayer::DataLayer(int depth, int width, int height)
        : data_(depth * width * height), data_view_(depth, width, height, data_),
        rand_collection_(data_view_.extent), data_generated_(data_view_.extent)
    {
        memory_.reserve(kMemorySize);
        for (int i = 0; i < kMemorySize; i++)
        {
            memory_.emplace_back(data_view_.extent);
        }
    }

    DataLayer::DataLayer(DataLayer&& other)
        : data_(std::move(other.data_)), data_view_(other.data_view_),
        rand_collection_(other.rand_collection_),
        data_generated_(std::move(other.data_generated_)), memory_(std::move(other.memory_))
    {

    }

    void DataLayer::SetData(const std::vector<float>& data)
    {
        assert(data.size() == data_.size());

        // Disgard the data on GPU
        data_view_.discard_data();
        // Copy the data
        data_ = data;
        data_view_.refresh();
    }


    ConvolveLayer::ConvolveLayer(int num_neuron, int neuron_depth, int neuron_width, int neuron_height)
        : weights_(num_neuron * neuron_depth * neuron_width * neuron_height),
        weights_view_(extent<4>(std::array<int, 4>{{ num_neuron, neuron_depth, neuron_width, neuron_height }}.data()), weights_)
    {
    }

    ConvolveLayer::ConvolveLayer(ConvolveLayer&& other)
        : weights_(std::move(other.weights_)), weights_view_(other.weights_view_)
    {
    }

    void ConvolveLayer::PassUp(array_view<const float, 3> bottom_layer,
        array_view<float, 3> top_layer_prob, array_view<float, 3> top_layer_sample,
        tinymt_collection<3>& rand_collection) const
    {
        assert(top_layer_prob.extent[0] /* depth */ == this->neuron_num());

        // readonly
        array_view<const float, 4> neuron_weights = weights_view_;
        // writeonly
        top_layer_prob.discard_data();
        top_layer_sample.discard_data();

        // non-tiled version
        parallel_for_each(top_layer_prob.extent,
            [=](index<3> idx) restrict(amp)
        {
            array_view<const float, 3> current_neuron = neuron_weights[idx[0]];// projection
            index<3> base_idx(0, idx[1], idx[2]);

            float result = 0.0f;

            for (int depth_idx = 0; depth_idx < current_neuron.extent[0]; depth_idx++)
            {
                for (int width_idx = 0; width_idx < current_neuron.extent[1]; width_idx++)
                {
                    for (int height_idx = 0; height_idx < current_neuron.extent[2]; height_idx++)
                    {
                        index<3> neuron_idx(depth_idx, width_idx, height_idx);
                        result += bottom_layer[base_idx + neuron_idx] * current_neuron[neuron_idx];
                    }
                }
            }

            // Logistic activation function. Maybe more types of activation function later.
            float prob = 1.0f / (1.0f + fast_math::expf(-result));
            top_layer_prob[idx] = prob;
            top_layer_sample[idx] = rand_collection[idx].next_single() <= prob ? 1.0f : 0.0f;
        });
    }

    void ConvolveLayer::PassDown(array_view<const float, 3> top_layer,
        array_view<float, 3> bottom_layer_prob, array_view<float, 3> bottom_layer_sample,
        tinymt_collection<3>& rand_collection) const
    {
        assert(top_layer.extent[0] == this->neuron_num());

        // readonly
        array_view<const float, 4> neuron_weights = weights_view_;
        // writeonly
        bottom_layer_prob.discard_data();
        bottom_layer_sample.discard_data();

        // non-tiled version
        parallel_for_each(bottom_layer_prob.extent,
            [=](index<3> idx) restrict(amp)
        {
            float result = 0.0f;
            int cur_depth_idx = idx[0];
            int cur_width_idx = idx[1];
            int cur_height_idx = idx[2];

            for (int neuron_idx = 0; neuron_idx < neuron_weights.extent[0]; neuron_idx++)
            {
                array_view<const float, 3> current_neuron = neuron_weights[neuron_idx];

                for (int width_idx = 0; width_idx < neuron_weights.extent[2]; width_idx++)
                {
                    for (int height_idx = 0; height_idx < neuron_weights.extent[3]; height_idx++)
                    {
                        if (cur_width_idx - width_idx >= 0 && cur_height_idx - height_idx >= 0)
                        {
                            result += current_neuron(cur_depth_idx, cur_width_idx, cur_height_idx) * 
                                top_layer(neuron_idx, cur_width_idx - width_idx, cur_height_idx - height_idx);
                        }
                    }
                }
            }

            // Logistic activation function. Maybe more types of activation function later.
            float prob = 1.0f / (1.0f + fast_math::expf(-result));
            bottom_layer_prob[idx] = prob;
            bottom_layer_sample[idx] = rand_collection[idx].next_single() <= prob ? 1.0f : 0.0f;
        });
    }

    void ConvolveLayer::Train(const DataLayer& bottom_layer, const DataLayer& top_layer, float learning_rate)
    {
        parallel_for_each(weights_view_.extent, [=](index<4> idx) restrict(amp)
        {

        });
    }

    void ConvolveLayer::RandomizeParams(unsigned int seed)
    {
        std::default_random_engine generator(seed);
        std::normal_distribution<float> distribution;

        for (float& w : weights_)
        {
            w = distribution(generator);
        }

        weights_view_.discard_data();
        weights_view_.refresh();
    }

    void DeepModel::AddDataLayer(int depth, int width, int height)
    {
        data_layers_.emplace_back(depth, width, height);
    }

    void DeepModel::AddConvolveLayer(int num_neuron, int neuron_depth, int neuron_width, int neuron_height, unsigned int rand_seed)
    {
        convolve_layers_.emplace_back(num_neuron, neuron_depth, neuron_width, neuron_height);
        convolve_layers_.back().RandomizeParams(rand_seed);
    }

    void DeepModel::PassUp(const std::vector<float>& data)
    {
        auto& bottom_layer = data_layers_.front();
        bottom_layer.SetData(data);

        for (int conv_layer_idx = 0; conv_layer_idx < convolve_layers_.size(); conv_layer_idx++)
        {
            auto& conv_layer = convolve_layers_[conv_layer_idx];
            auto& bottom_data_layer = data_layers_[conv_layer_idx];
            auto& top_data_layer = data_layers_[conv_layer_idx + 1];

            conv_layer.PassUp(bottom_data_layer.data_view_, top_data_layer.data_view_);
        }
    }

    void DeepModel::PassDown()
    {
        // prepare top layer for passing down
        auto& roof_data_layer = data_layers_.back();
        roof_data_layer.data_view_.copy_to(roof_data_layer.data_generated_);

        for (int conv_layer_idx = (int)convolve_layers_.size() - 1; conv_layer_idx >= 0; conv_layer_idx--)
        {
            auto& conv_layer = convolve_layers_[conv_layer_idx];
            auto& bottom_data_layer = data_layers_[conv_layer_idx];
            auto& top_data_layer = data_layers_[conv_layer_idx + 1];

            conv_layer.PassDown(top_data_layer.data_generated_, bottom_data_layer.data_generated_);
        }
    }
}
