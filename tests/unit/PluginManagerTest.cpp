// ============================================================
// PluginManager 单元测试
// ============================================================
// Phase03 验收标准：
//   - 编译 Plugin_Dummy.dll 成功
//   - 加载/卸载/重载 成功
//   - 连续热加载 100 次无崩溃、无内存泄漏

#include <gtest/gtest.h>
#include "mc/pluginmgr/PluginManager.h"
#include "mc/pluginmgr/IPlugin.h"

// Plugin_Dummy.dll 位于与测试可执行文件相同的目录
static const char* DUMMY_DLL = "Plugin_Dummy.dll";

// ============================================================
// 基本加载/卸载
// ============================================================

TEST(PluginManagerTest, LoadPlugin)
{
    mc::PluginManager pm;
    EXPECT_TRUE(pm.LoadPlugin(DUMMY_DLL));
    EXPECT_EQ(pm.GetPluginCount(), 1);

    auto* plugin = pm.GetPlugin("Dummy");
    ASSERT_NE(plugin, nullptr);
    EXPECT_STREQ(plugin->GetName(), "Dummy");
}

TEST(PluginManagerTest, UnloadPlugin)
{
    mc::PluginManager pm;
    pm.LoadPlugin(DUMMY_DLL);
    EXPECT_EQ(pm.GetPluginCount(), 1);

    EXPECT_TRUE(pm.UnloadPlugin("Dummy"));
    EXPECT_EQ(pm.GetPluginCount(), 0);
    EXPECT_EQ(pm.GetPlugin("Dummy"), nullptr);
}

TEST(PluginManagerTest, UnloadNonexistentPlugin)
{
    mc::PluginManager pm;
    EXPECT_FALSE(pm.UnloadPlugin("NonExistent"));
}

TEST(PluginManagerTest, LoadInvalidDll)
{
    mc::PluginManager pm;
    EXPECT_FALSE(pm.LoadPlugin("NonExistent.dll"));
    EXPECT_EQ(pm.GetPluginCount(), 0);
}

// ============================================================
// 重载
// ============================================================

TEST(PluginManagerTest, ReloadPlugin)
{
    mc::PluginManager pm;
    EXPECT_TRUE(pm.LoadPlugin(DUMMY_DLL));
    EXPECT_EQ(pm.GetPluginCount(), 1);

    EXPECT_TRUE(pm.ReloadPlugin(DUMMY_DLL));
    EXPECT_EQ(pm.GetPluginCount(), 1);
    EXPECT_NE(pm.GetPlugin("Dummy"), nullptr);
}

// ============================================================
// 100次热加载压力测试
// ============================================================

TEST(PluginManagerTest, ReloadPlugin100Times)
{
    mc::PluginManager pm;
    for (int i = 0; i < 100; ++i)
    {
        EXPECT_TRUE(pm.ReloadPlugin(DUMMY_DLL)) << "Failed at iteration " << i;
        EXPECT_EQ(pm.GetPluginCount(), 1);
        EXPECT_NE(pm.GetPlugin("Dummy"), nullptr);
    }
    // 最终卸载
    EXPECT_TRUE(pm.UnloadPlugin("Dummy"));
    EXPECT_EQ(pm.GetPluginCount(), 0);
}

// ============================================================
// 批量管理
// ============================================================

TEST(PluginManagerTest, UnloadAll)
{
    mc::PluginManager pm;
    pm.LoadPlugin(DUMMY_DLL);
    pm.LoadPlugin(DUMMY_DLL);  // 第二次加载同 DLL 会覆盖（同名字" Dummy"）

    pm.UnloadAll();
    EXPECT_EQ(pm.GetPluginCount(), 0);
}

TEST(PluginManagerTest, GetPluginNames)
{
    mc::PluginManager pm;
    pm.LoadPlugin(DUMMY_DLL);

    auto names = pm.GetPluginNames();
    EXPECT_EQ(names.size(), 1);
    EXPECT_EQ(names[0], "Dummy");
}

// ============================================================
// 移动语义
// ============================================================

TEST(PluginManagerTest, MoveConstructor)
{
    mc::PluginManager pm1;
    pm1.LoadPlugin(DUMMY_DLL);
    EXPECT_EQ(pm1.GetPluginCount(), 1);

    mc::PluginManager pm2(std::move(pm1));
    EXPECT_EQ(pm2.GetPluginCount(), 1);
    EXPECT_EQ(pm1.GetPluginCount(), 0);  // 移后源为空
    EXPECT_NE(pm2.GetPlugin("Dummy"), nullptr);
}

TEST(PluginManagerTest, MoveAssignment)
{
    mc::PluginManager pm1;
    pm1.LoadPlugin(DUMMY_DLL);

    mc::PluginManager pm2;
    pm2 = std::move(pm1);

    EXPECT_EQ(pm2.GetPluginCount(), 1);
    EXPECT_EQ(pm1.GetPluginCount(), 0);
    EXPECT_NE(pm2.GetPlugin("Dummy"), nullptr);
}

// ============================================================
// 析构时自动卸载
// ============================================================

TEST(PluginManagerTest, AutoUnloadOnDestructor)
{
    {
        mc::PluginManager pm;
        pm.LoadPlugin(DUMMY_DLL);
        EXPECT_EQ(pm.GetPluginCount(), 1);
    }
    // 析构完成，无崩溃即通过
    SUCCEED();
}