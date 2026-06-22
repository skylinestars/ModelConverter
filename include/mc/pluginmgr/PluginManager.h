#pragma once

#include "mc/pluginmgr/IPlugin.h"
#include <string>
#include <unordered_map>
#include <vector>

// Windows DLL 加载
#ifdef _WIN32
#include <windows.h>
#endif

namespace mc {

// ============================================================
// PluginManager
// ============================================================
// 负责 DLL 的加载、卸载、热重载。
// 内部维护一个注册表（Registry），将插件名映射到 DLL 句柄和 IPlugin 实例。
// Phase03 不要求线程安全，后续阶段可按需加锁。

class PluginManager
{
public:
    PluginManager()  = default;
    ~PluginManager();

    // 禁止拷贝
    PluginManager(const PluginManager&)            = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // 移动
    PluginManager(PluginManager&&)            noexcept;
    PluginManager& operator=(PluginManager&&) noexcept;

    // 加载 DLL 并创建插件实例
    // dllPath: 插件 DLL 的完整路径
    // 返回 true 表示加载成功，false 表示失败
    bool LoadPlugin(const char* dllPath);

    // 卸载指定名称的插件
    // 返回 true 表示成功卸载，false 表示未找到该插件
    bool UnloadPlugin(const char* pluginName);

    // 卸载所有已加载的插件
    void UnloadAll();

    // 热重载：先卸载再加载同一个 DLL
    // 返回 true 表示重载成功
    bool ReloadPlugin(const char* dllPath);

    // 根据名称查找插件实例
    // 返回 nullptr 表示未找到
    IPlugin* GetPlugin(const char* pluginName) const;

    // 获取所有已加载插件的名称列表
    std::vector<std::string> GetPluginNames() const;

    // 已加载插件数量
    size_t GetPluginCount() const;

private:
    // 插件注册表条目
    struct PluginEntry
    {
        HMODULE  handle  = nullptr;  // DLL 句柄
        IPlugin* plugin  = nullptr;  // 插件实例
        std::string dllPath;         // DLL 路径，用于 Reload
    };

    // 插件名 -> PluginEntry
    std::unordered_map<std::string, PluginEntry> m_registry;
};

} // namespace mc