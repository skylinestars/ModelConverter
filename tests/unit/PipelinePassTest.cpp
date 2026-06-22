// ============================================================
// Pipeline / IPass 单元测试
// ============================================================
// Phase04 验收标准：
//   - DummyPass 加入 Pipeline 执行后返回成功
//   - 多个 Pass 串联按顺序执行
//   - 某个 Pass 返回错误时 Pipeline 终止并透传该错误
//   - 所有 warnings 被累积合并

#include <gtest/gtest.h>
#include "mc/pipeline/Pipeline.h"
#include "mc/pipeline/IPass.h"
#include "mc/core/Scene.h"

// ============================================================
// 测试用 Pass 实现
// ============================================================

// 始终成功的桩 Pass，记录执行顺序
class DummyPass : public mc::IPass
{
public:
    explicit DummyPass(const std::string& name, std::vector<std::string>* log = nullptr)
        : m_name(name), m_log(log) {}

    mc::VoidResult Execute(mc::Scene& /*scene*/) override
    {
        if (m_log) m_log->push_back(m_name);
        mc::VoidResult r;
        r.ok = true;
        return r;
    }

    std::string Name() const override { return m_name; }

private:
    std::string               m_name;
    std::vector<std::string>* m_log;
};

// 始终失败的 Pass
class FailPass : public mc::IPass
{
public:
    explicit FailPass(const std::string& name, std::vector<std::string>* log = nullptr)
        : m_name(name), m_log(log) {}

    mc::VoidResult Execute(mc::Scene& /*scene*/) override
    {
        if (m_log) m_log->push_back(m_name);
        mc::VoidResult r;
        r.ok    = false;
        r.error = "intentional failure";
        return r;
    }

    std::string Name() const override { return m_name; }

private:
    std::string               m_name;
    std::vector<std::string>* m_log;
};

// 返回 warning 但成功的 Pass
class WarnPass : public mc::IPass
{
public:
    explicit WarnPass(const std::string& name, const std::string& warning)
        : m_name(name), m_warning(warning) {}

    mc::VoidResult Execute(mc::Scene& /*scene*/) override
    {
        mc::VoidResult r;
        r.ok = true;
        r.warnings.push_back(m_warning);
        return r;
    }

    std::string Name() const override { return m_name; }

private:
    std::string m_name;
    std::string m_warning;
};

// ============================================================
// 测试用 Scene（空 Scene 即可）
// ============================================================
static mc::Scene MakeEmptyScene()
{
    return mc::Scene{};
}

// ============================================================
// 基础功能
// ============================================================

TEST(PipelineTest, EmptyPipelineSucceeds)
{
    mc::Pipeline pipeline;
    mc::Scene    scene = MakeEmptyScene();
    auto result = pipeline.Execute(scene);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.error.empty());
}

TEST(PipelineTest, SingleDummyPassSucceeds)
{
    mc::Pipeline pipeline;
    pipeline.AddPass(std::make_unique<DummyPass>("Dummy"));
    EXPECT_EQ(pipeline.PassCount(), 1);

    mc::Scene scene = MakeEmptyScene();
    auto result = pipeline.Execute(scene);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.error.empty());
}

// ============================================================
// 执行顺序
// ============================================================

TEST(PipelineTest, MultiplePassesExecuteInOrder)
{
    std::vector<std::string> log;
    mc::Pipeline pipeline;
    pipeline.AddPass(std::make_unique<DummyPass>("PassA", &log));
    pipeline.AddPass(std::make_unique<DummyPass>("PassB", &log));
    pipeline.AddPass(std::make_unique<DummyPass>("PassC", &log));

    mc::Scene scene = MakeEmptyScene();
    auto result = pipeline.Execute(scene);

    EXPECT_TRUE(result.ok);
    ASSERT_EQ(log.size(), 3);
    EXPECT_EQ(log[0], "PassA");
    EXPECT_EQ(log[1], "PassB");
    EXPECT_EQ(log[2], "PassC");
}

// ============================================================
// 失败传播
// ============================================================

TEST(PipelineTest, FailPassStopsPipeline)
{
    std::vector<std::string> log;
    mc::Pipeline pipeline;
    pipeline.AddPass(std::make_unique<DummyPass>("PassA", &log));
    pipeline.AddPass(std::make_unique<FailPass>("FailingPass", &log));
    pipeline.AddPass(std::make_unique<DummyPass>("PassC", &log));  // 不应执行

    mc::Scene scene = MakeEmptyScene();
    auto result = pipeline.Execute(scene);

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
    // FailingPass 名称应出现在错误信息中
    EXPECT_NE(result.error.find("FailingPass"), std::string::npos);
    // PassC 不应被执行
    ASSERT_EQ(log.size(), 2);
    EXPECT_EQ(log[0], "PassA");
    EXPECT_EQ(log[1], "FailingPass");
}

TEST(PipelineTest, FailPassErrorMessagePropagated)
{
    mc::Pipeline pipeline;
    pipeline.AddPass(std::make_unique<FailPass>("MyFailPass"));

    mc::Scene scene = MakeEmptyScene();
    auto result = pipeline.Execute(scene);

    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("intentional failure"), std::string::npos);
}

// ============================================================
// Warnings 累积
// ============================================================

TEST(PipelineTest, WarningsAccumulatedFromMultiplePasses)
{
    mc::Pipeline pipeline;
    pipeline.AddPass(std::make_unique<WarnPass>("PassA", "warn from A"));
    pipeline.AddPass(std::make_unique<WarnPass>("PassB", "warn from B"));
    pipeline.AddPass(std::make_unique<DummyPass>("PassC"));

    mc::Scene scene = MakeEmptyScene();
    auto result = pipeline.Execute(scene);

    EXPECT_TRUE(result.ok);
    ASSERT_EQ(result.warnings.size(), 2);
    // warnings 中应包含 Pass 名称前缀
    EXPECT_NE(result.warnings[0].find("PassA"), std::string::npos);
    EXPECT_NE(result.warnings[1].find("PassB"), std::string::npos);
}

TEST(PipelineTest, WarningsPreservedOnFailure)
{
    mc::Pipeline pipeline;
    pipeline.AddPass(std::make_unique<WarnPass>("PassA", "warn from A"));
    pipeline.AddPass(std::make_unique<FailPass>("FailPass"));

    mc::Scene scene = MakeEmptyScene();
    auto result = pipeline.Execute(scene);

    EXPECT_FALSE(result.ok);
    // 失败前的 warnings 仍应保留
    ASSERT_EQ(result.warnings.size(), 1);
    EXPECT_NE(result.warnings[0].find("PassA"), std::string::npos);
}

// ============================================================
// PassCount / Clear
// ============================================================

TEST(PipelineTest, PassCount)
{
    mc::Pipeline pipeline;
    EXPECT_EQ(pipeline.PassCount(), 0);
    pipeline.AddPass(std::make_unique<DummyPass>("A"));
    pipeline.AddPass(std::make_unique<DummyPass>("B"));
    EXPECT_EQ(pipeline.PassCount(), 2);
}

TEST(PipelineTest, ClearResetsPassList)
{
    mc::Pipeline pipeline;
    pipeline.AddPass(std::make_unique<DummyPass>("A"));
    pipeline.AddPass(std::make_unique<DummyPass>("B"));
    EXPECT_EQ(pipeline.PassCount(), 2);

    pipeline.Clear();
    EXPECT_EQ(pipeline.PassCount(), 0);

    mc::Scene scene = MakeEmptyScene();
    auto result = pipeline.Execute(scene);
    EXPECT_TRUE(result.ok);
}

// ============================================================
// 移动语义
// ============================================================

TEST(PipelineTest, MoveConstructor)
{
    std::vector<std::string> log;
    mc::Pipeline p1;
    p1.AddPass(std::make_unique<DummyPass>("A", &log));
    EXPECT_EQ(p1.PassCount(), 1);

    mc::Pipeline p2(std::move(p1));
    EXPECT_EQ(p2.PassCount(), 1);
    EXPECT_EQ(p1.PassCount(), 0);

    mc::Scene scene = MakeEmptyScene();
    auto result = p2.Execute(scene);
    EXPECT_TRUE(result.ok);
    ASSERT_EQ(log.size(), 1);
    EXPECT_EQ(log[0], "A");
}
