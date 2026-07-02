// ============================================================
// mc_cli —— 模型格式转换命令行工具
// ============================================================
// 用法：mc_cli <input> <output>
//   根据文件扩展名自动选择导入/导出插件：
//   - .fbx        → Fbx 插件
//   - .gltf/.glb  → Gltf 插件
//   - 其他        → Assimp 插件（Assimp 不支持则提示暂不支持）

#include "mc/common/Logger.h"
#include "mc/core/Scene.h"
#include "mc/importer/IImporter.h"
#include "mc/exporter/IExporter.h"
#include "mc/pipeline/Pipeline.h"
#include "mc/pipeline/UnitConvertPass.h"
#include "mc/pipeline/AxisConvertPass.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using CreateImporterFunc  = mc::IImporter* (*)();
using DestroyImporterFunc = void (*)(mc::IImporter*);
using CreateExporterFunc  = mc::IExporter* (*)();
using DestroyExporterFunc = void (*)(mc::IExporter*);

// RAII 日志重定向：构造时打开文件并重定向 cout/cerr，析构时自动恢复
struct LogRedirect
{
    std::ofstream       file;
    std::streambuf*     oldCout = nullptr;
    std::streambuf*     oldCerr = nullptr;

    explicit LogRedirect(const std::string& path)
    {
        file.open(path, std::ios::out | std::ios::trunc);
        oldCout = std::cout.rdbuf(file.rdbuf());
        oldCerr = std::cerr.rdbuf(file.rdbuf());
        mc::Logger::Instance().SetLogFile(path);
        std::cout << "Log file: " << path << "\n";
    }

    ~LogRedirect()
    {
        std::cout.rdbuf(oldCout);
        std::cerr.rdbuf(oldCerr);
        file.close();
    }

    LogRedirect(const LogRedirect&) = delete;
    LogRedirect& operator=(const LogRedirect&) = delete;
};

// 获取小写扩展名（含点，如 ".fbx"）
static std::string GetLowerExt(const std::string& path)
{
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

// 根据输入扩展名选择导入 DLL
static const char* SelectImportDll(const std::string& ext)
{
    if (ext == ".fbx")                     return PLUGIN_FBX_FILENAME;
    if (ext == ".gltf" || ext == ".glb")   return PLUGIN_GLTF_FILENAME;
    return PLUGIN_ASSIMP_FILENAME;
}

// 根据输出扩展名选择导出 DLL，返回 nullptr 表示不支持
static const char* SelectExportDll(const std::string& ext)
{
    if (ext == ".fbx")                     return PLUGIN_FBX_FILENAME;
    if (ext == ".gltf" || ext == ".glb")   return PLUGIN_GLTF_FILENAME;
    return nullptr;
}

// 加载 DLL 并获取 CreateImporter / DestroyImporter
static bool LoadImporterDll(const char* dllName, HMODULE& hDll,
                            CreateImporterFunc& createFn,
                            DestroyImporterFunc& destroyFn)
{
    hDll = LoadLibraryA(dllName);
    if (!hDll)
    {
        std::cerr << "Failed to load " << dllName << " (error " << GetLastError() << ")\n";
        return false;
    }
    createFn  = reinterpret_cast<CreateImporterFunc>(GetProcAddress(hDll, "CreateImporter"));
    destroyFn = reinterpret_cast<DestroyImporterFunc>(GetProcAddress(hDll, "DestroyImporter"));
    if (!createFn || !destroyFn)
    {
        std::cerr << "CreateImporter/DestroyImporter not found in " << dllName << "\n";
        FreeLibrary(hDll);
        return false;
    }
    return true;
}

// 加载 DLL 并获取 CreateExporter / DestroyExporter
static bool LoadExporterDll(const char* dllName, HMODULE& hDll,
                            CreateExporterFunc& createFn,
                            DestroyExporterFunc& destroyFn)
{
    hDll = LoadLibraryA(dllName);
    if (!hDll)
    {
        std::cerr << "Failed to load " << dllName << " (error " << GetLastError() << ")\n";
        return false;
    }
    createFn  = reinterpret_cast<CreateExporterFunc>(GetProcAddress(hDll, "CreateExporter"));
    destroyFn = reinterpret_cast<DestroyExporterFunc>(GetProcAddress(hDll, "DestroyExporter"));
    if (!createFn || !destroyFn)
    {
        std::cerr << "CreateExporter/DestroyExporter not found in " << dllName << "\n";
        FreeLibrary(hDll);
        return false;
    }
    return true;
}

int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        std::cerr << "Usage: mc_cli <input> <output> [--no-embed-textures]\n";
        return 1;
    }

    const std::string inputPath  = argv[1];
    const std::string outputPath = argv[2];

    // 默认贴图内嵌到输出文件（GLB/FBX 均支持）；--no-embed-textures 时改为写外部贴图文件
    bool embedTextures = true;
    for (int i = 3; i < argc; ++i)
        if (std::string(argv[i]) == "--no-embed-textures")
            embedTextures = false;

    // ---- 提取扩展名 ----
    std::string inExt  = GetLowerExt(inputPath);
    std::string outExt = GetLowerExt(outputPath);

    if (inExt.empty())
    {
        std::cerr << "Error: cannot determine input file extension: " << inputPath << "\n";
        return 1;
    }
    if (outExt.empty())
    {
        std::cerr << "Error: cannot determine output file extension: " << outputPath << "\n";
        return 1;
    }

    // ---- 日志重定向到文件（与输出文件同名，扩展名 .log）----
    LogRedirect logRedirect((std::filesystem::path(outputPath).replace_extension(".log")).string());

    // ---- 选择导出插件（先检查，避免导入成功后才报导出不支持）----
    const char* exportDll = SelectExportDll(outExt);
    if (!exportDll)
    {
        std::cerr << "暂不支持导出格式: " << outExt
                  << " (目前仅支持 .fbx / .gltf / .glb)\n";
        return 1;
    }

    const char* importDll = SelectImportDll(inExt);

    // ---- 加载导入 DLL 并导入 ----
    {
        HMODULE hDll = nullptr;
        CreateImporterFunc  createFn  = nullptr;
        DestroyImporterFunc destroyFn = nullptr;

        if (!LoadImporterDll(importDll, hDll, createFn, destroyFn))
            return 1;

        mc::IImporter* importer = createFn();

        // 非 FBX/GLTF 时由 Assimp 兜底，需检查 CanImport
        if (inExt != ".fbx" && inExt != ".gltf" && inExt != ".glb")
        {
            if (!importer->CanImport(inExt))
            {
                std::cerr << "暂不支持导入格式: " << inExt
                          << " (Assimp 不支持此格式)\n";
                destroyFn(importer);
                FreeLibrary(hDll);
                return 1;
            }
        }

        mc::Scene scene;
        mc::VoidResult result = importer->Import(inputPath, scene);

        destroyFn(importer);
        FreeLibrary(hDll);

        if (!result.ok)
        {
            std::cerr << "Import failed: " << result.error << "\n";
            return 1;
        }

        // ---- 统计信息 ----
        size_t vertexCount = 0;
        for (const auto& mesh : scene.meshes)
            vertexCount += mesh.positions.size();

        std::cout << "Input       : " << inputPath << "\n";
        std::cout << "Output      : " << outputPath << "\n";
        std::cout << "NodeCount   : " << scene.NodeCount()     << "\n";
        std::cout << "MeshCount   : " << scene.MeshCount()     << "\n";
        std::cout << "MaterialCnt : " << scene.MaterialCount() << "\n";
        std::cout << "VertexCount : " << vertexCount           << "\n";

        // 运行 Pipeline（单位转换 → 坐标轴转换）
        mc::Pipeline pipeline;
        pipeline.AddPass(std::make_unique<mc::UnitConvertPass>(scene.metadata.unitScale));

        // 源文件为 Z-up 时（如 3DS Max FBX），转换为 GLTF 标准的 Y-up
        if (scene.metadata.upAxis == mc::Axis::Z)
            pipeline.AddPass(std::make_unique<mc::AxisConvertPass>(mc::UpAxis::Z, mc::UpAxis::Y));

        auto pipeResult = pipeline.Execute(scene);

        // ---- 加载导出 DLL 并导出 ----
        HMODULE hExportDll = nullptr;
        CreateExporterFunc  createExportFn  = nullptr;
        DestroyExporterFunc destroyExportFn = nullptr;

        if (!LoadExporterDll(exportDll, hExportDll, createExportFn, destroyExportFn))
            return 1;

        mc::IExporter* exporter = createExportFn();

        mc::ExportContext ctx;
        ctx.outputPath = outputPath;
        ctx.options.embedTextures = embedTextures;

        mc::VoidResult exportResult = exporter->Export(scene, ctx);

        destroyExportFn(exporter);
        FreeLibrary(hExportDll);

        if (!exportResult.ok)
        {
            std::cerr << "Export failed: " << exportResult.error << "\n";
            return 1;
        }

        std::cout << "Export      : OK (" << outExt << ")\n";
    }

    return 0;
}
