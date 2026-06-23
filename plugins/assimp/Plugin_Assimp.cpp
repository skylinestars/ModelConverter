// ============================================================
// Plugin_Assimp —— Phase06 静态模型导入插件
// ============================================================

#include "mc/pluginmgr/IPlugin.h"
#include "mc/importer/IImporter.h"
#include "mc/common/Logger.h"
#include "AssimpSceneConverter.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <algorithm>
#include <string>
#include <filesystem>

namespace mc {

// ============================================================
// AssimpImporter —— IImporter 实现
// ============================================================
class AssimpImporter : public IImporter
{
public:
    bool CanImport(const std::string& ext) const override
    {
        static const char* supported[] = {
            ".obj", ".stl", ".dae", ".3ds", ".ply", ".x", nullptr
        };
        std::string lower = ext;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        for (int i = 0; supported[i]; ++i)
            if (lower == supported[i]) return true;
        return false;
    }

    VoidResult Import(const std::string& path, Scene& scene) override
    {
        VoidResult result;
        result.ok = true;

        Assimp::Importer importer;
        const aiScene* aiSrc = importer.ReadFile(
            path,
            aiProcess_Triangulate |
            aiProcess_JoinIdenticalVertices |
            aiProcess_GenNormals |
            aiProcess_SortByPType
        );

        if (!aiSrc || (aiSrc->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !aiSrc->mRootNode)
        {
            result.ok    = false;
            result.error = std::string("Assimp failed to load '") + path + "': " +
                           importer.GetErrorString();
            return result;
        }

        scene.metadata.asset.sourceFile = path;
        std::string ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (!ext.empty() && ext[0] == '.') ext.erase(ext.begin());
        scene.metadata.asset.sourceFormat = ext.empty() ? "assimp" : ext;

        AssimpSceneConverter converter;
        return converter.Convert(aiSrc, scene);
    }
};

// ============================================================
// Plugin_Assimp —— IPlugin 实现
// ============================================================
class Plugin_Assimp : public IPlugin
{
public:
    const char* GetName() const override { return "Assimp"; }

    bool OnLoad() override
    {
        Logger::Instance().LogInfo("Plugin Loaded: Assimp");
        return true;
    }

    void OnUnload() override
    {
        Logger::Instance().LogInfo("Plugin Unloaded: Assimp");
    }

    // 供外部获取 IImporter（简单单例方式）
    IImporter* GetImporter() { return &m_importer; }

private:
    AssimpImporter m_importer;
};

} // namespace mc

// ============================================================
// DLL 导出
// ============================================================
extern "C"
{
    __declspec(dllexport) mc::IPlugin* CreatePlugin()
    {
        return new mc::Plugin_Assimp();
    }

    __declspec(dllexport) void DestroyPlugin(mc::IPlugin* plugin)
    {
        delete plugin;
    }

    // 额外导出 IImporter，方便测试直接调用而无需通过 PluginManager
    __declspec(dllexport) mc::IImporter* CreateImporter()
    {
        return new mc::AssimpImporter();
    }

    __declspec(dllexport) void DestroyImporter(mc::IImporter* importer)
    {
        delete importer;
    }
}
