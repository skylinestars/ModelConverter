// ============================================================
// Plugin_DummyExporter —— Phase10 验收用桩导出插件
// ============================================================

#include "mc/pluginmgr/IPlugin.h"
#include "mc/exporter/IExporter.h"
#include "mc/common/Logger.h"

#include <algorithm>
#include <string>

namespace mc {

// ============================================================
// DummyExporter —— IExporter 实现
// ============================================================
class DummyExporter : public IExporter
{
public:
    bool CanExport(const std::string& ext) const override
    {
        std::string lower = ext;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lower == ".dummy";
    }

    VoidResult Export(const Scene& scene, ExportContext& ctx) override
    {
        VoidResult result;
        result.ok = true;

        Logger::Instance().LogInfo(
            std::string("[DummyExporter] Exporting to: ") + ctx.outputPath
        );
        Logger::Instance().LogInfo(
            std::string("[DummyExporter] NodeCount=")     + std::to_string(scene.NodeCount())     +
            " MeshCount="     + std::to_string(scene.MeshCount())     +
            " MaterialCount=" + std::to_string(scene.MaterialCount())
        );

        ctx.nodesExported     = scene.NodeCount();
        ctx.meshesExported    = scene.MeshCount();
        ctx.materialsExported = scene.MaterialCount();
        ctx.texturesExported  = scene.TextureCount();

        if (ctx.outputPath.empty())
        {
            result.ok    = false;
            result.error = "DummyExporter: outputPath is empty";
        }

        return result;
    }
};

// ============================================================
// Plugin_DummyExporter —— IPlugin 实现
// ============================================================
class Plugin_DummyExporter : public IPlugin
{
public:
    const char* GetName() const override { return "DummyExporter"; }

    bool OnLoad() override
    {
        Logger::Instance().LogInfo("Plugin Loaded: DummyExporter");
        return true;
    }

    void OnUnload() override
    {
        Logger::Instance().LogInfo("Plugin Unloaded: DummyExporter");
    }
};

} // namespace mc

// ============================================================
// DLL 导出
// ============================================================
extern "C"
{
    __declspec(dllexport) mc::IPlugin* CreatePlugin()
    {
        return new mc::Plugin_DummyExporter();
    }

    __declspec(dllexport) void DestroyPlugin(mc::IPlugin* plugin)
    {
        delete plugin;
    }

    __declspec(dllexport) mc::IExporter* CreateExporter()
    {
        return new mc::DummyExporter();
    }

    __declspec(dllexport) void DestroyExporter(mc::IExporter* exporter)
    {
        delete exporter;
    }
}
