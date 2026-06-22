#include "mc/pluginmgr/PluginManager.h"
#include "mc/common/Logger.h"
#include <algorithm>

namespace mc {

// ============================================================
// PluginManager 实现
// ============================================================

PluginManager::~PluginManager()
{
    UnloadAll();
}

PluginManager::PluginManager(PluginManager&& other) noexcept
    : m_registry(std::move(other.m_registry))
{
}

PluginManager& PluginManager::operator=(PluginManager&& other) noexcept
{
    if (this != &other)
    {
        UnloadAll();
        m_registry = std::move(other.m_registry);
    }
    return *this;
}

bool PluginManager::LoadPlugin(const char* dllPath)
{
    // 加载 DLL
    HMODULE handle = LoadLibraryA(dllPath);
    if (!handle)
    {
        Logger::Instance().LogError("Failed to load DLL: " + std::string(dllPath));
        return false;
    }

    // 查找 CreatePlugin 函数
    auto createFunc = reinterpret_cast<CreatePluginFunc>(
        GetProcAddress(handle, "CreatePlugin"));
    if (!createFunc)
    {
        Logger::Instance().LogError("DLL missing CreatePlugin: " + std::string(dllPath));
        FreeLibrary(handle);
        return false;
    }

    // 查找 DestroyPlugin 函数
    auto destroyFunc = reinterpret_cast<DestroyPluginFunc>(
        GetProcAddress(handle, "DestroyPlugin"));
    if (!destroyFunc)
    {
        Logger::Instance().LogError("DLL missing DestroyPlugin: " + std::string(dllPath));
        FreeLibrary(handle);
        return false;
    }

    // 创建插件实例
    IPlugin* plugin = createFunc();
    if (!plugin)
    {
        Logger::Instance().LogError("CreatePlugin returned nullptr: " + std::string(dllPath));
        FreeLibrary(handle);
        return false;
    }

    // 调用插件初始化
    if (!plugin->OnLoad())
    {
        Logger::Instance().LogError("Plugin OnLoad failed: " + std::string(plugin->GetName()));
        destroyFunc(plugin);
        FreeLibrary(handle);
        return false;
    }

    // 注册到 Registry
    PluginEntry entry;
    entry.handle  = handle;
    entry.plugin  = plugin;
    entry.dllPath = dllPath;
    m_registry[plugin->GetName()] = entry;

    return true;
}

bool PluginManager::UnloadPlugin(const char* pluginName)
{
    auto it = m_registry.find(pluginName);
    if (it == m_registry.end())
    {
        return false;
    }

    PluginEntry& entry = it->second;

    // 调用插件卸载回调
    if (entry.plugin)
    {
        entry.plugin->OnUnload();
    }

    // 销毁插件实例
    auto destroyFunc = reinterpret_cast<DestroyPluginFunc>(
        GetProcAddress(entry.handle, "DestroyPlugin"));
    if (destroyFunc && entry.plugin)
    {
        destroyFunc(entry.plugin);
    }

    // 释放 DLL
    FreeLibrary(entry.handle);

    m_registry.erase(it);
    return true;
}

void PluginManager::UnloadAll()
{
    // 收集所有插件名，避免在遍历中修改 map
    std::vector<std::string> names;
    names.reserve(m_registry.size());
    for (const auto& kv : m_registry)
    {
        names.push_back(kv.first);
    }

    for (const auto& name : names)
    {
        UnloadPlugin(name.c_str());
    }
}

bool PluginManager::ReloadPlugin(const char* dllPath)
{
    // 先通过 DLL 路径找到对应的插件名
    std::string pluginName;
    for (const auto& kv : m_registry)
    {
        if (kv.second.dllPath == dllPath)
        {
            pluginName = kv.first;
            break;
        }
    }

    if (!pluginName.empty())
    {
        UnloadPlugin(pluginName.c_str());
    }

    return LoadPlugin(dllPath);
}

IPlugin* PluginManager::GetPlugin(const char* pluginName) const
{
    auto it = m_registry.find(pluginName);
    return (it != m_registry.end()) ? it->second.plugin : nullptr;
}

std::vector<std::string> PluginManager::GetPluginNames() const
{
    std::vector<std::string> names;
    names.reserve(m_registry.size());
    for (const auto& kv : m_registry)
    {
        names.push_back(kv.first);
    }
    return names;
}

size_t PluginManager::GetPluginCount() const
{
    return m_registry.size();
}

} // namespace mc