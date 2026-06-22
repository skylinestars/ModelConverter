#pragma once

namespace mc {

// ============================================================
// Plugin接口
// ============================================================
// 所有插件 DLL 必须实现此接口。
// 注意：DLL 边界安全 —— 接口中不暴露 STL 类型，
// 仅使用 const char* 和基本类型。

class IPlugin
{
public:
    virtual ~IPlugin() = default;

    // 插件唯一标识名，用于日志和注册表查找
    virtual const char* GetName() const = 0;

    // 插件被加载时调用，返回 false 表示初始化失败，插件将被卸载
    virtual bool OnLoad() = 0;

    // 插件被卸载时调用，释放资源
    virtual void OnUnload() = 0;
};

// DLL 导出函数签名（extern "C" 避免 name mangling）
using CreatePluginFunc  = IPlugin* (*)();
using DestroyPluginFunc = void (*)(IPlugin*);

} // namespace mc