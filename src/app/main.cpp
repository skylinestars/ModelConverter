#include "mc/common/Logger.h"
#include "mc/pluginmgr/PluginManager.h"

int main()
{
    mc::Logger::Instance().LogInfo("Application Started");

    // Phase03 Plugin系统 —— 加载 Dummy 插件
    mc::PluginManager pm;
    if (pm.LoadPlugin("Plugin_Dummy.dll"))
    {
        // 插件 OnLoad 中已输出 "Plugin Loaded: Dummy"
    }

    // 退出时自动卸载（析构函数调用 UnloadAll）
    // 插件 OnUnload 中输出 "Plugin Unloaded: Dummy"
    return 0;
}