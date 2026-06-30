#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include "mc/common/Math.h"
#include <string>
#include <vector>

namespace tinygltf
{
    class Model;
    class Node;
}

namespace mc
{

    void MultiplyMat4x4(const float *a, const float *b, float *out);
    void InvertAffineTrsMatrix(const float *matrix, float *out);

    std::string ExtractFileNameFromPath(const std::string &path);
    std::string DetectMimeTypeFromPath(const std::string &path);
    std::string DetectMimeTypeFromBytes(const std::vector<uint8_t> &bytes);

    int AppendImageBufferView(tinygltf::Model &model, const std::vector<uint8_t> &bytes);

    std::vector<float> ReadFloatAccessorValues(const tinygltf::Model &model, int accessorIdx);
    std::vector<uint32_t> ReadIndexAccessorValues(const tinygltf::Model &model, int accessorIdx);
    Matrix4 BuildLocalMatrixFromGltfNode(const tinygltf::Node &gNode);

} // namespace mc
