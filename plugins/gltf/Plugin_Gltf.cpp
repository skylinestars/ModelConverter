// ============================================================
// Plugin_Gltf —— Phase08/11 GLTF/GLB 导入+导出插件
// ============================================================

#include "mc/pluginmgr/IPlugin.h"
#include "mc/importer/IImporter.h"
#include "mc/exporter/IExporter.h"
#include "mc/common/Logger.h"
#include "GltfSceneConverter.h"
#include "GltfExporter.h"

// tiny_gltf 仅在 GltfSceneConverter.cpp 中定义实现，此处只声明
#include "tiny_gltf.h"

#include <algorithm>
#include <filesystem>
#include <string>

namespace mc {

// ============================================================
// GltfImporter —— IImporter 实现
// ============================================================
class GltfImporter : public IImporter
{
public:
    bool CanImport(const std::string& ext) const override
    {
        std::string lower = ext;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lower == ".gltf" || lower == ".glb";
    }

    VoidResult Import(const std::string& path, Scene& scene) override
    {
        VoidResult result;
        result.ok = true;

        tinygltf::TinyGLTF loader;
        tinygltf::Model model;
        std::string err, warn;

        bool ok = false;
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".glb")
            ok = loader.LoadBinaryFromFile(&model, &err, &warn, path);
        else
            ok = loader.LoadASCIIFromFile(&model, &err, &warn, path);

        if (!warn.empty())
            Logger::Instance().LogInfo(std::string("tinygltf warn: ") + warn);

        if (!ok)
        {
            result.ok    = false;
            result.error = std::string("tinygltf failed to load '") + path + "': " + err;
            return result;
        }

        scene.metadata.asset.sourceFile = path;
        std::string ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (!ext.empty() && ext[0] == '.') ext.erase(ext.begin());
        scene.metadata.asset.sourceFormat = ext.empty() ? "gltf" : ext;

        std::string baseDir;
        std::filesystem::path p(path);
        if (p.has_parent_path())
            baseDir = p.parent_path().string();

        GltfSceneConverter converter;
        return converter.Convert(model, baseDir, scene);
    }
};

// ============================================================
// Plugin_Gltf —— IPlugin 实现
// ============================================================
class Plugin_Gltf : public IPlugin
{
public:
    const char* GetName() const override { return "Gltf"; }

    bool OnLoad() override
    {
        Logger::Instance().LogInfo("Plugin Loaded: Gltf");
        return true;
    }

    void OnUnload() override
    {
        Logger::Instance().LogInfo("Plugin Unloaded: Gltf");
    }

    IImporter* GetImporter() { return &m_importer; }

private:
    GltfImporter m_importer;
};

} // namespace mc

// ============================================================
// DLL 导出
// ============================================================
extern "C"
{
    __declspec(dllexport) mc::IPlugin* CreatePlugin()
    {
        return new mc::Plugin_Gltf();
    }

    __declspec(dllexport) void DestroyPlugin(mc::IPlugin* plugin)
    {
        delete plugin;
    }

    __declspec(dllexport) mc::IImporter* CreateImporter()
    {
        return new mc::GltfImporter();
    }

    __declspec(dllexport) void DestroyImporter(mc::IImporter* importer)
    {
        delete importer;
    }

    __declspec(dllexport) mc::IExporter* CreateExporter()
    {
        return new mc::GltfExporter();
    }

    __declspec(dllexport) void DestroyExporter(mc::IExporter* exporter)
    {
        delete exporter;
    }
}
