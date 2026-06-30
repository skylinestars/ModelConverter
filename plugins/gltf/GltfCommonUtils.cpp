#include "GltfCommonUtils.h"
#include "tiny_gltf.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mc
{

    void MultiplyMat4x4(const float *a, const float *b, float *out)
    {
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
            {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                    sum += a[k * 4 + row] * b[col * 4 + k];
                out[col * 4 + row] = sum;
            }
    }

    void InvertAffineTrsMatrix(const float *matrix, float *out)
    {
        float sx = std::sqrt(matrix[0] * matrix[0] + matrix[1] * matrix[1] + matrix[2] * matrix[2]);
        float sy = std::sqrt(matrix[4] * matrix[4] + matrix[5] * matrix[5] + matrix[6] * matrix[6]);
        float sz = std::sqrt(matrix[8] * matrix[8] + matrix[9] * matrix[9] + matrix[10] * matrix[10]);
        if (sx < 1e-7f)
            sx = 1e-7f;
        if (sy < 1e-7f)
            sy = 1e-7f;
        if (sz < 1e-7f)
            sz = 1e-7f;
        const float scales[3] = {sx, sy, sz};

        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                out[c * 4 + r] = matrix[r * 4 + c] / (scales[r] * scales[c]);

        for (int r = 0; r < 3; ++r)
            out[12 + r] = -(out[r] * matrix[12] + out[4 + r] * matrix[13] + out[8 + r] * matrix[14]);

        out[3] = out[7] = out[11] = 0.0f;
        out[15] = 1.0f;
    }

    std::string ExtractFileNameFromPath(const std::string &path)
    {
        size_t sep = path.find_last_of("/\\");
        return (sep != std::string::npos) ? path.substr(sep + 1) : path;
    }

    std::string DetectMimeTypeFromPath(const std::string &path)
    {
        std::string name = ExtractFileNameFromPath(path);
        size_t dot = name.rfind('.');
        std::string ext = (dot != std::string::npos) ? name.substr(dot) : "";
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".jpg" || ext == ".jpeg")
            return "image/jpeg";
        if (ext == ".webp")
            return "image/webp";
        return "image/png";
    }

    std::string DetectMimeTypeFromBytes(const std::vector<uint8_t> &bytes)
    {
        if (bytes.size() >= 8)
        {
            static const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
            if (std::memcmp(bytes.data(), kPngSig, 8) == 0)
                return "image/png";
        }

        if (bytes.size() >= 3)
        {
            if (bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff)
                return "image/jpeg";
        }

        if (bytes.size() >= 12)
        {
            if (bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F' &&
                bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P')
            {
                return "image/webp";
            }
        }

        return "";
    }

    int AppendImageBufferView(tinygltf::Model &model, const std::vector<uint8_t> &bytes)
    {
        if (bytes.empty())
            return -1;

        auto &buf = model.buffers[0].data;
        size_t byteOffset = buf.size();
        buf.insert(buf.end(), bytes.begin(), bytes.end());
        while (buf.size() % 4)
            buf.push_back(0);

        tinygltf::BufferView bv;
        bv.buffer = 0;
        bv.byteOffset = (int)byteOffset;
        bv.byteLength = (int)bytes.size();
        bv.target = 0;
        int bvIdx = (int)model.bufferViews.size();
        model.bufferViews.push_back(std::move(bv));
        return bvIdx;
    }

    std::vector<float> ReadFloatAccessorValues(const tinygltf::Model &model, int accessorIdx)
    {
        const auto &acc = model.accessors[accessorIdx];
        const auto &bv = model.bufferViews[acc.bufferView];
        const auto &buf = model.buffers[bv.buffer];

        int componentCount = tinygltf::GetNumComponentsInType(acc.type);
        size_t stride = bv.byteStride ? bv.byteStride
                                      : componentCount * tinygltf::GetComponentSizeInBytes(acc.componentType);

        std::vector<float> out;
        out.reserve(acc.count * componentCount);

        const uint8_t *base = buf.data.data() + bv.byteOffset + acc.byteOffset;
        for (size_t i = 0; i < acc.count; ++i)
        {
            const uint8_t *ptr = base + i * stride;
            for (int c = 0; c < componentCount; ++c)
            {
                float val = 0.0f;
                if (acc.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
                {
                    std::memcpy(&val, ptr + c * 4, 4);
                }
                else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
                {
                    uint16_t u;
                    std::memcpy(&u, ptr + c * 2, 2);
                    val = static_cast<float>(u) / 65535.0f;
                }
                else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
                {
                    val = static_cast<float>(ptr[c]) / 255.0f;
                }
                out.push_back(val);
            }
        }

        return out;
    }

    std::vector<uint32_t> ReadIndexAccessorValues(const tinygltf::Model &model, int accessorIdx)
    {
        const auto &acc = model.accessors[accessorIdx];
        const auto &bv = model.bufferViews[acc.bufferView];
        const auto &buf = model.buffers[bv.buffer];

        std::vector<uint32_t> out;
        out.reserve(acc.count);

        const uint8_t *base = buf.data.data() + bv.byteOffset + acc.byteOffset;
        size_t compSize = tinygltf::GetComponentSizeInBytes(acc.componentType);
        size_t stride = bv.byteStride ? bv.byteStride : compSize;

        for (size_t i = 0; i < acc.count; ++i)
        {
            const uint8_t *ptr = base + i * stride;
            uint32_t idx = 0;
            if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
                std::memcpy(&idx, ptr, 4);
            else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
            {
                uint16_t u;
                std::memcpy(&u, ptr, 2);
                idx = u;
            }
            else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
            {
                idx = *ptr;
            }
            out.push_back(idx);
        }

        return out;
    }

    Matrix4 BuildLocalMatrixFromGltfNode(const tinygltf::Node &gNode)
    {
        if (!gNode.matrix.empty())
        {
            const auto &m = gNode.matrix;
            return Matrix4(
                (float)m[0], (float)m[1], (float)m[2], (float)m[3],
                (float)m[4], (float)m[5], (float)m[6], (float)m[7],
                (float)m[8], (float)m[9], (float)m[10], (float)m[11],
                (float)m[12], (float)m[13], (float)m[14], (float)m[15]);
        }

        float tx = gNode.translation.size() >= 3 ? (float)gNode.translation[0] : 0.0f;
        float ty = gNode.translation.size() >= 3 ? (float)gNode.translation[1] : 0.0f;
        float tz = gNode.translation.size() >= 3 ? (float)gNode.translation[2] : 0.0f;

        float sx = gNode.scale.size() >= 3 ? (float)gNode.scale[0] : 1.0f;
        float sy = gNode.scale.size() >= 3 ? (float)gNode.scale[1] : 1.0f;
        float sz = gNode.scale.size() >= 3 ? (float)gNode.scale[2] : 1.0f;

        float qx = gNode.rotation.size() >= 4 ? (float)gNode.rotation[0] : 0.0f;
        float qy = gNode.rotation.size() >= 4 ? (float)gNode.rotation[1] : 0.0f;
        float qz = gNode.rotation.size() >= 4 ? (float)gNode.rotation[2] : 0.0f;
        float qw = gNode.rotation.size() >= 4 ? (float)gNode.rotation[3] : 1.0f;

        float xx = qx * qx;
        float yy = qy * qy;
        float zz = qz * qz;
        float xy = qx * qy;
        float xz = qx * qz;
        float yz = qy * qz;
        float wx = qw * qx;
        float wy = qw * qy;
        float wz = qw * qz;

        float r00 = (1.0f - 2.0f * (yy + zz)) * sx;
        float r10 = (2.0f * (xy + wz)) * sx;
        float r20 = (2.0f * (xz - wy)) * sx;

        float r01 = (2.0f * (xy - wz)) * sy;
        float r11 = (1.0f - 2.0f * (xx + zz)) * sy;
        float r21 = (2.0f * (yz + wx)) * sy;

        float r02 = (2.0f * (xz + wy)) * sz;
        float r12 = (2.0f * (yz - wx)) * sz;
        float r22 = (1.0f - 2.0f * (xx + yy)) * sz;

        return Matrix4(
            r00, r10, r20, 0.0f,
            r01, r11, r21, 0.0f,
            r02, r12, r22, 0.0f,
            tx, ty, tz, 1.0f);
    }

} // namespace mc
