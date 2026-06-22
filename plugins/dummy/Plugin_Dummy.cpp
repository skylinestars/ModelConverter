// ============================================================
// Plugin_Dummy —— Phase03 验收用桩插件
// ============================================================
// 编译为 Plugin_Dummy.dll，被 mc_cli.exe 动态加载。
// 仅输出日志，不执行任何实际转换工作。

#pragma once
#include "mc/pluginmgr/IPlugin.h"
#include "mc/common/Logger.h"
#include <cstdio>

namespace mc {

class Plugin_Dummy : public IPlugin
{
public:
    const char* GetName() const override
    {
        return "Dummy";
    }

    bool OnLoad() override
    {
        Logger::Instance().LogInfo("Plugin Loaded: Dummy");
        return true;
    }

    void OnUnload() override
    {
        Logger::Instance().LogInfo("Plugin Unloaded: Dummy");
    }
};

} // namespace mc

// DLL 导出 —— extern "C" 避免 C++ name mangling
extern "C"
{
    __declspec(dllexport) mc::IPlugin* CreatePlugin()
    {
        return new mc::Plugin_Dummy();
    }

    __declspec(dllexport) void DestroyPlugin(mc::IPlugin* plugin)
    {
        delete plugin;
    }
}