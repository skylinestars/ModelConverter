// ============================================================
// mc_cli —— Phase07 调试入口
// ============================================================
// 用法：mc_cli <file>
//   动态加载 Plugin_Assimp.dll，导入指定文件，
//   打印 NodeCount / MeshCount / MaterialCount / VertexCount，
//   ValidatePass 验证通过后返回 0，否则返回 1。

#include "mc/common/Logger.h"
#include "mc/core/Scene.h"
#include "mc/importer/IImporter.h"
#include "mc/pipeline/ValidatePass.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <iostream>
#include <string>

using CreateImporterFunc  = mc::IImporter* (*)();
using DestroyImporterFunc = void (*)(mc::IImporter*);

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: mc_cli <file>\n";
        return 1;
    }

    const std::string filePath = argv[1];

    HMODULE hDll = LoadLibraryA(PLUGIN_ASSIMP_FILENAME);
    if (!hDll)
    {
        std::cerr << "Failed to load " PLUGIN_ASSIMP_FILENAME " (error " << GetLastError() << ")\n";
        return 1;
    }

    auto createFn  = reinterpret_cast<CreateImporterFunc>(GetProcAddress(hDll, "CreateImporter"));
    auto destroyFn = reinterpret_cast<DestroyImporterFunc>(GetProcAddress(hDll, "DestroyImporter"));
    if (!createFn || !destroyFn)
    {
        std::cerr << "Export CreateImporter/DestroyImporter not found\n";
        FreeLibrary(hDll);
        return 1;
    }

    mc::IImporter* importer = createFn();

    mc::Scene scene;
    mc::VoidResult importResult = importer->Import(filePath, scene);

    destroyFn(importer);
    FreeLibrary(hDll);

    if (!importResult.ok)
    {
        std::cerr << "Import failed: " << importResult.error << "\n";
        return 1;
    }

    size_t vertexCount = 0;
    for (const auto& mesh : scene.meshes)
        vertexCount += mesh.positions.size();

    std::cout << "File        : " << filePath << "\n";
    std::cout << "NodeCount   : " << scene.NodeCount()     << "\n";
    std::cout << "MeshCount   : " << scene.MeshCount()     << "\n";
    std::cout << "MaterialCount: " << scene.MaterialCount() << "\n";
    std::cout << "VertexCount : " << vertexCount           << "\n";

    mc::ValidatePass vp;
    mc::VoidResult vr = vp.Execute(scene);
    if (!vr.ok)
    {
        std::cerr << "ValidatePass failed: " << vr.error << "\n";
        return 1;
    }
    for (const auto& w : vr.warnings)
        std::cout << "  [warn] " << w << "\n";

    std::cout << "ValidatePass: OK\n";
    return 0;
}