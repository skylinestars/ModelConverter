#include "mc/pipeline/Pipeline.h"
#include "mc/common/Logger.h"

namespace mc {

void Pipeline::AddPass(std::unique_ptr<IPass> pass)
{
    m_passes.push_back(std::move(pass));
}

void Pipeline::Clear()
{
    m_passes.clear();
}

size_t Pipeline::PassCount() const
{
    return m_passes.size();
}

VoidResult Pipeline::Execute(Scene& scene)
{
    VoidResult finalResult;
    finalResult.ok = true;

    for (const auto& pass : m_passes)
    {
        Logger::Instance().LogInfo("[Pipeline] Running pass: " + pass->Name());

        VoidResult r = pass->Execute(scene);

        // 累积所有 warnings
        for (const auto& w : r.warnings)
        {
            finalResult.warnings.push_back("[" + pass->Name() + "] " + w);
        }

        if (!r.ok)
        {
            finalResult.ok    = false;
            finalResult.error = "[" + pass->Name() + "] " + r.error;
            Logger::Instance().LogError("[Pipeline] Pass failed: " + pass->Name() + " — " + r.error);
            return finalResult;
        }
    }

    return finalResult;
}

} // namespace mc
