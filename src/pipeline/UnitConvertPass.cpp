#include "mc/pipeline/UnitConvertPass.h"
#include "mc/core/Animation.h"
#include "mc/common/Logger.h"
#include <cmath>
#include <string>

namespace mc {

static std::string UnitNameFromScaleToMeter(float scale)
{
    if (std::abs(scale - 1.0f) < 1e-6f) return "m";
    if (std::abs(scale - 0.01f) < 1e-6f) return "cm";
    if (std::abs(scale - 0.001f) < 1e-6f) return "mm";
    if (std::abs(scale - 0.0254f) < 1e-6f) return "inch";
    return "custom";
}

UnitConvertPass::UnitConvertPass(float factor) : m_factor(factor) {}

std::string UnitConvertPass::Name() const { return "UnitConvertPass"; }

void UnitConvertPass::ScaleMeshPositions(Scene& scene) const
{
    for (auto& mesh : scene.meshes)
        for (auto& p : mesh.positions)
        { p.x *= m_factor; p.y *= m_factor; p.z *= m_factor; }
}

void UnitConvertPass::ScaleNodeTranslations(Scene& scene) const
{
    for (auto& node : scene.nodes)
    {
        float* m = node.localMatrix.m;
        m[12] *= m_factor;   // translation X  (column-major index 12)
        m[13] *= m_factor;   // translation Y
        m[14] *= m_factor;   // translation Z
    }
}

void UnitConvertPass::ScaleAnimationTranslations(Scene& scene) const
{
    for (auto& clip : scene.animations)
        for (auto& ch : clip.nodeChannels)
            for (auto& kf : ch.translation.keys)
            {
                kf.value.x *= m_factor;
                kf.value.y *= m_factor;
                kf.value.z *= m_factor;
            }
}

VoidResult UnitConvertPass::Execute(Scene& scene)
{
    constexpr float kEpsilon = 1e-9f;
    if (std::abs(m_factor - 1.0f) < kEpsilon) return {true, ""};

    if (m_factor <= 0.0f)
        return {false, "UnitConvertPass: factor must be > 0"};

    ScaleMeshPositions(scene);
    ScaleNodeTranslations(scene);
    ScaleAnimationTranslations(scene);

    scene.metadata.unitScale *= m_factor;
    scene.metadata.unit = UnitNameFromScaleToMeter(scene.metadata.unitScale);

    Logger::Instance().LogInfo(
        std::string("UnitConvertPass: scale factor = ") + std::to_string(m_factor));
    return {true, ""};
}

} // namespace mc
