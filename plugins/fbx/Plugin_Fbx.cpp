// ============================================================
// Plugin_Fbx —— Phase09 FBX SDK 静态模型导入插件
// ============================================================

#include "mc/pluginmgr/IPlugin.h"
#include "mc/importer/IImporter.h"
#include "mc/exporter/IExporter.h"
#include "mc/common/Logger.h"
#include "FbxSceneConverter.h"
#include "FbxExporter.h"

#include <fbxsdk.h>
#include <algorithm>
#include <string>
#include <filesystem>

using namespace fbxsdk;

namespace mc {

// ============================================================
// FbxImporter —— IImporter 实现
// ============================================================
class FbxImporterImpl : public IImporter
{
public:
    bool CanImport(const std::string& ext) const override
    {
        std::string lower = ext;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lower == ".fbx";
    }

    VoidResult Import(const std::string& path, Scene& scene) override
    {
        VoidResult result;
        result.ok = true;

        FbxManager* manager = FbxManager::Create();
        FbxIOSettings* ios  = FbxIOSettings::Create(manager, IOSROOT);
        manager->SetIOSettings(ios);

        FbxImporter* importer = FbxImporter::Create(manager, "");
        if (!importer->Initialize(path.c_str(), -1, manager->GetIOSettings()))
        {
            std::string err = importer->GetStatus().GetErrorString();
            importer->Destroy();
            manager->Destroy();
            result.ok    = false;
            result.error = std::string("FBX Initialize failed for '") + path + "': " + err;
            return result;
        }

        FbxScene* fbxScene = FbxScene::Create(manager, "Scene");
        if (!importer->Import(fbxScene))
        {
            std::string err = importer->GetStatus().GetErrorString();
            importer->Destroy();
            fbxScene->Destroy();
            manager->Destroy();
            result.ok    = false;
            result.error = std::string("FBX Import failed: ") + err;
            return result;
        }
        importer->Destroy();

        // 资产元信息（路径/格式）由导入器写入
        scene.metadata.asset.sourceFormat = "fbx";
        scene.metadata.asset.sourceFile = path;

        FbxSceneConverter converter;
        result = converter.Convert(manager, fbxScene, scene);

        fbxScene->Destroy();
        manager->Destroy();
        return result;
    }
};

// ============================================================
// Plugin_Fbx —— IPlugin 实现
// ============================================================
class Plugin_Fbx : public IPlugin
{
public:
    const char* GetName() const override { return "Fbx"; }

    bool OnLoad() override
    {
        Logger::Instance().LogInfo("Plugin Loaded: Fbx");
        return true;
    }

    void OnUnload() override
    {
        Logger::Instance().LogInfo("Plugin Unloaded: Fbx");
    }

private:
    FbxImporterImpl m_importer;
};

} // namespace mc

// ============================================================
// DLL 导出
// ============================================================
extern "C"
{
    __declspec(dllexport) mc::IPlugin* CreatePlugin()
    {
        return new mc::Plugin_Fbx();
    }

    __declspec(dllexport) void DestroyPlugin(mc::IPlugin* plugin)
    {
        delete plugin;
    }

    __declspec(dllexport) mc::IImporter* CreateImporter()
    {
        return new mc::FbxImporterImpl();
    }

    __declspec(dllexport) void DestroyImporter(mc::IImporter* importer)
    {
        delete importer;
    }

    __declspec(dllexport) mc::IExporter* CreateExporter()
    {
        return new mc::FbxExporter();
    }

    __declspec(dllexport) void DestroyExporter(mc::IExporter* exporter)
    {
        delete exporter;
    }
}
