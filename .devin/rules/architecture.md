---
trigger: model_decision
description: 这是项目架构说明
---
# ModelConverter
这是一个类似 **Assimp + FBX Converter + USD Converter + GLTF Converter + Blender Import/Export Framework** 的通用三维格式转换平台

## 项目架构
整体思路：`N + N`（每种格式各自一个 Importer + Exporter），所有格式都经过统一 Scene 中转，而非 `N × N` 直连。

```
Importer(FBX/GLTF/OBJ/...) → Scene → [Pipeline Passes] → Scene → Exporter(GLTF/FBX/...)
```

### 目录结构
```
include/mc/
  core/          ← 核心数据结构头文件（Scene、Node、Mesh、Material、Skeleton、Skin、Animation、Texture、Camera、Light、PointInstancer）
  common/        ← 基础工具（Math、Logger、Types、Result）
  importer/      ← IImporter 接口
  exporter/      ← IExporter 接口
  pipeline/      ← IPass 接口 + 各 Pass 头文件
  pluginmgr/     ← IPlugin、PluginManager

src/
  common/        ← Logger 实现
  pipeline/      ← Pass 实现（ValidatePass、UnitConvertPass、AxisConvertPass 等）
  pluginmgr/     ← PluginManager（DLL 扫描/加载/卸载）
  app/           ← CLI 入口 main.cpp

plugins/         ← 格式插件（各自独立 CMake 子项目）
  gltf/          ← GltfExporter、GltfSceneConverter
  fbx/           ← FbxExporter、FbxSceneConverter、FbxMeshHelper
  assimp/        ← AssimpSceneConverter（兜底读 40+ 格式）
  dummy/         ← 示例 Importer 插件
  dummy_exporter/← 示例 Exporter 插件

tests/
  unit/          ← GoogleTest 单元测试
  data/          ← 测试用 FBX/GLB 黄金文件
```

### 核心层（Core）
- **绝不依赖任何第三方库**（Assimp/FBX SDK/OpenUSD 只在 plugins/ 内出现）
- `Scene` 是所有数据的根，平铺存储各类对象（`std::vector<Node/Mesh/Material/...>`）
- **ObjectID**（`uint64_t`）是跨对象唯一引用键，永不复用、永不等于 vector index；所有跨组件引用只用 ObjectID，不用指针
- **三分离**：Node（场景图）≠ Bone（骨骼）≠ Skin（蒙皮绑定），对应 GLTF/USD 规范
- `Result<T>` / `VoidResult`：所有 Importer/Exporter/Pass 操作的统一返回类型，含 ok、error、warnings

### Pipeline 层
- `IPass::Execute(Scene&)` 接口：Passes 按顺序链式修改 Scene
- **必须执行的 Pass**（按顺序）：`ValidatePass`（第一个，只检查不修复）→ `TriangulatePass` → `UnitConvertPass` → `AxisConvertPass` → `HandednessFlipPass`
- **可选 Pass**：`MergeDuplicateVerticesPass`、`LimitBoneInfluencePass`（GLTF 导出前压到 4 权重）、`NormalGeneratePass`、`AnimationOptimizePass` 等
- Importer 只负责"读"，数据校验和修正全部在 Pass 层完成

### Plugin 层
- 每个插件实现 `IImporter` 或 `IExporter` 接口，通过显式 `RegisterPlugin(FormatRegistry&)` 注册（禁用全局静态自动注册）
- `PluginManager` 在运行时扫描 plugins/ 目录，动态加载 .dll/.so，校验 `PLUGIN_INTERFACE_VERSION` 后注册
- `FormatRegistry` 单例，按文件扩展名分发 Importer/Exporter

### 架构红线
1. Core 不依赖第三方库，第三方库仅在 plugins/ 内
2. Node 不内嵌 Mesh（Geometry 与 SceneGraph 分离，实例化基础）
3. Bone ≠ Node（三分离不可破坏）
4. 跨对象引用只用 ObjectID，不用 shared_ptr
5. Plugin 注册必须是显式调用，不靠静态初始化
6. Importer 只读数据，不做修正
