#include "FbxExporter.h"
#include "mc/core/Mesh.h"
#include "mc/core/Material.h"
#include "mc/core/Texture.h"
#include "mc/core/Node.h"
#include "mc/core/Skeleton.h"
#include "mc/core/Skin.h"
#include "mc/core/Animation.h"
#include "mc/common/Logger.h"

#include <fbxsdk.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace fbxsdk;

namespace mc
{

    // ============================================================
    // FbxBuilder —— 将 mc::Scene 写入 FbxScene
    // 每个方法 < 60 行；Export() 做高层调度。
    // ============================================================
    class FbxBuilder
    {
    public:
        FbxBuilder(FbxManager *mgr, FbxScene *scene, const ExportOptions &opts,
                   const std::filesystem::path &outputDir)
            : m_mgr(mgr), m_fbxScene(scene), m_opts(opts), m_outputDir(outputDir) {}

        void Build(const Scene &mcScene)
        {
            IndexScene(mcScene);
            CreateMaterials(mcScene);
            CreateNodes(mcScene);
            WireRootNodes(mcScene);
            AddSkeletonAttributes(mcScene);
            AddSkins(mcScene);
            AddBlendShapes(mcScene);
            AddAnimations(mcScene);
        }

        // embedTextures=true 时，ResolveTexturePath() 写到磁盘的纹理文件只是
        // 供 FBX SDK 内嵌时读取的临时中转文件，内嵌完成后即可删除，避免和最终自包含的
        // .fbx 一起留下冗余的外部贴图副本。须在 Export() 成功之后调用。
        void CleanupEmbeddedTextureFiles() const
        {
            if (!m_opts.embedTextures)
                return;
            for (const auto &[texId, path] : m_texPathCache)
            {
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }
        }

    private:
        FbxManager *m_mgr;
        FbxScene *m_fbxScene;
        const ExportOptions &m_opts;
        std::filesystem::path m_outputDir;

        std::unordered_map<ObjectID, FbxSurfaceMaterial *> m_matMap;
        std::unordered_map<ObjectID, const Mesh *> m_meshSrcMap;
        std::unordered_map<ObjectID, const Texture *> m_texSrcMap;
        std::unordered_map<ObjectID, std::filesystem::path> m_texPathCache;
        std::unordered_map<ObjectID, FbxMesh *> m_meshMap;
        std::unordered_map<ObjectID, FbxNode *> m_nodeMap;
        // (meshId << 32 | morphIndex) -> FbxBlendShapeChannel*，供权重动画导出查找
        std::unordered_map<uint64_t, FbxBlendShapeChannel *> m_morphChannelMap;
        // 蒙皮网格的 meshId 集合：MakeSceneNode 据此决定是否丢弃节点自身变换
        std::unordered_set<ObjectID> m_skinnedMeshIds;
        // 挂载了蒙皮网格的节点 id 集合：这些节点自身的变换（静态或动画）
        // 必须被完全丢弃（见 MakeSceneNode 的推导），AddAnimations 据此跳过
        // 给它们添加 TRS 动画曲线
        std::unordered_set<ObjectID> m_skinnedMeshNodeIds;

        void IndexScene(const Scene &scene)
        {
            for (const auto &m : scene.meshes)
                m_meshSrcMap[m.id] = &m;
            for (const auto &t : scene.textures)
                m_texSrcMap[t.id] = &t;
            for (const auto &s : scene.skins)
                m_skinnedMeshIds.insert(s.meshId);
            for (const auto &node : scene.nodes)
                if (!node.meshIds.empty() && m_skinnedMeshIds.count(node.meshIds[0]) > 0)
                    m_skinnedMeshNodeIds.insert(node.id);
        }

        static std::string ExtFromMime(const std::string &mime)
        {
            if (mime == "image/jpeg")
                return ".jpg";
            if (mime == "image/webp")
                return ".webp";
            if (mime == "image/bmp")
                return ".bmp";
            return ".png";
        }

        static std::string SafeTextureStem(const Texture &tex)
        {
            std::string stem = tex.name.empty() ? ("tex_" + std::to_string(tex.id)) : tex.name;
            std::replace(stem.begin(), stem.end(), ' ', '_');

            // 部分 GLB 把原始文件名（含扩展名，如 "arch75_034_diffuse.jpg"）整体写进 image.name，
            // 若不剥离，后面再拼接 ExtFromMime() 算出的扩展名会变成 "xxx.jpg.jpg"
            static const std::unordered_set<std::string> kImageExts = {
                ".jpg", ".jpeg", ".png", ".bmp", ".webp", ".tga", ".gif"};
            std::filesystem::path p(stem);
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (kImageExts.count(ext))
                stem = p.stem().string();

            return stem;
        }

        static std::filesystem::path Utf8PathFromString(const std::string &s)
        {
            return std::filesystem::u8path(s);
        }

        std::filesystem::path ResolveTexturePath(const Texture &tex)
        {
            auto cached = m_texPathCache.find(tex.id);
            if (cached != m_texPathCache.end())
                return cached->second;

            std::filesystem::create_directories(m_outputDir);
            std::string stem = SafeTextureStem(tex);

            if (!tex.uri.empty())
            {
                std::filesystem::path srcPath = Utf8PathFromString(tex.uri);
                std::filesystem::path outPath = m_outputDir / srcPath.filename();
                if (outPath.filename().empty())
                    outPath = m_outputDir / Utf8PathFromString(stem + srcPath.extension().u8string());

                std::error_code ec;
                if (std::filesystem::exists(srcPath, ec))
                {
                    std::filesystem::copy_file(srcPath, outPath,
                                               std::filesystem::copy_options::overwrite_existing,
                                               ec);
                    if (!ec)
                    {
                        m_texPathCache[tex.id] = outPath;
                        return outPath;
                    }
                }
            }

            if (!tex.embeddedData.empty())
            {
                std::filesystem::path outPath = m_outputDir / Utf8PathFromString(stem + ExtFromMime(tex.mimeType));
                std::ofstream ofs(outPath, std::ios::binary);
                if (!ofs)
                    return {};
                ofs.write(reinterpret_cast<const char *>(tex.embeddedData.data()),
                          static_cast<std::streamsize>(tex.embeddedData.size()));
                if (!ofs)
                    return {};
                m_texPathCache[tex.id] = outPath;
                return outPath;
            }

            return {};
        }

        void BindBaseColorTexture(FbxSurfacePhong *fbxMat, const Material &mat)
        {
            auto texIt = m_texSrcMap.find(mat.baseColorTexture.textureId);
            if (texIt == m_texSrcMap.end())
                return;

            std::filesystem::path texPath = ResolveTexturePath(*texIt->second);
            if (texPath.empty())
                return;

            FbxFileTexture *fbxTex = FbxFileTexture::Create(m_fbxScene, (mat.name + "_BaseColor").c_str());
            std::string absPath = std::filesystem::absolute(texPath).u8string();
            std::string relPath = texPath.filename().u8string();
            fbxTex->SetFileName(absPath.c_str());
            fbxTex->SetRelativeFileName(relPath.c_str());
            fbxTex->SetTextureUse(FbxTexture::eStandard);
            fbxTex->SetMappingType(FbxTexture::eUV);
            fbxTex->SetMaterialUse(FbxFileTexture::eModelMaterial);
            fbxTex->SetSwapUV(false);
            fbxTex->SetTranslation(0.0, 0.0);
            fbxTex->SetScale(1.0, 1.0);
            fbxTex->SetRotation(0.0, 0.0);

            // 注意：FBX SDK 的内嵌（EXP_FBX_EMBEDDED，见 Export()）只需要 FbxFileTexture
            // 的文件名指向磁盘上真实存在的文件即可自动嵌入，不需要手动创建 FbxVideo
            // （官方样例 ExportScene03 也是这样做的）。

            fbxMat->Diffuse.ConnectSrcObject(fbxTex);
        }

        // ---- Materials ----
        FbxSurfaceMaterial *MakeMaterial(const Material &mat)
        {
            auto *fbxMat = FbxSurfacePhong::Create(m_fbxScene, mat.name.c_str());
            fbxMat->Diffuse.Set(FbxDouble3(mat.baseColor.x, mat.baseColor.y, mat.baseColor.z));
            fbxMat->DiffuseFactor.Set(mat.baseColor.w);
            fbxMat->Emissive.Set(FbxDouble3(mat.emissive.x, mat.emissive.y, mat.emissive.z));
            fbxMat->Shininess.Set(mat.shininess);
            fbxMat->ShadingModel.Set("Phong");
            fbxMat->TransparencyFactor.Set(1.0 - mat.opacity);
            BindBaseColorTexture(fbxMat, mat);
            return fbxMat;
        }

        void CreateMaterials(const Scene &scene)
        {
            for (const auto &mat : scene.materials)
                m_matMap[mat.id] = MakeMaterial(mat);
        }

        // ---- Mesh control-points & polygon layout ----
        // bakeMat：蒙皮网格节点把自身的世界变换烘焙进顶点数据时使用（见 MakeSceneNode 的说明），
        // 非蒙皮网格传单位矩阵，等价于原来的直接写入。
        void SetControlPoints(FbxMesh *fbxMesh, const Mesh &mcMesh, const FbxAMatrix &bakeMat)
        {
            fbxMesh->InitControlPoints((int)mcMesh.positions.size());
            FbxVector4 *cp = fbxMesh->GetControlPoints();
            for (size_t i = 0; i < mcMesh.positions.size(); ++i)
            {
                FbxVector4 p(mcMesh.positions[i].x, mcMesh.positions[i].y, mcMesh.positions[i].z, 1.0);
                cp[i] = bakeMat.MultT(p);
            }
        }

        void AddNormals(FbxMesh *fbxMesh, const Mesh &mcMesh, const FbxAMatrix &bakeMat)
        {
            if (!m_opts.exportNormals || mcMesh.normals.empty())
                return;
            auto *layer = fbxMesh->GetLayer(0);
            auto *elem = FbxLayerElementNormal::Create(fbxMesh, "");
            elem->SetMappingMode(FbxLayerElement::eByControlPoint);
            elem->SetReferenceMode(FbxLayerElement::eDirect);

            // 法线只跟旋转/缩放有关，不能带平移：用去掉平移分量的 bakeMat 副本
            FbxAMatrix rotMat = bakeMat;
            rotMat.SetT(FbxVector4(0.0, 0.0, 0.0));
            for (const auto &n : mcMesh.normals)
            {
                FbxVector4 nv = rotMat.MultT(FbxVector4(n.x, n.y, n.z, 0.0));
                elem->GetDirectArray().Add(nv);
            }
            layer->SetNormals(elem);
        }

        void AddUVs(FbxMesh *fbxMesh, const Mesh &mcMesh)
        {
            if (!m_opts.exportUVs || mcMesh.uvs.empty() || mcMesh.uvs[0].empty())
                return;
            auto *elem = fbxMesh->CreateElementUV("UVMap");
            elem->SetMappingMode(FbxLayerElement::eByControlPoint);
            elem->SetReferenceMode(FbxLayerElement::eDirect);
            for (const auto &uv : mcMesh.uvs[0])
                elem->GetDirectArray().Add(FbxVector2(uv.x, 1.0 - uv.y));
        }

        int FindNodeMaterialIndex(FbxNode *node, FbxSurfaceMaterial *mat)
        {
            for (int i = 0; i < node->GetMaterialCount(); ++i)
                if (node->GetMaterial(i) == mat)
                    return i;
            return -1;
        }

        int EnsureNodeMaterial(FbxNode *node, ObjectID matId)
        {
            auto it = m_matMap.find(matId);
            if (it == m_matMap.end())
                return -1;

            FbxSurfaceMaterial *fbxMat = it->second;
            int idx = FindNodeMaterialIndex(node, fbxMat);
            if (idx >= 0)
                return idx;
            return node->AddMaterial(fbxMat);
        }

        void EnsureNodeMaterials(FbxNode *node, const Mesh &mcMesh)
        {
            for (const auto &sec : mcMesh.sections)
                EnsureNodeMaterial(node, sec.materialId);
        }

        void AddSectionPolygons(FbxMesh *fbxMesh, const Mesh &mcMesh,
                                uint32_t idxOffset, uint32_t idxCount, int matIdx)
        {
            for (uint32_t i = idxOffset; i + 2 < idxOffset + idxCount; i += 3)
            {
                fbxMesh->BeginPolygon(matIdx);
                fbxMesh->AddPolygon((int)mcMesh.indices[i]);
                fbxMesh->AddPolygon((int)mcMesh.indices[i + 1]);
                fbxMesh->AddPolygon((int)mcMesh.indices[i + 2]);
                fbxMesh->EndPolygon();
            }
        }

        FbxMesh *BuildMesh(FbxNode *meshNode, const Mesh &mcMesh, const FbxAMatrix &bakeMat)
        {
            auto *fbxMesh = FbxMesh::Create(m_fbxScene, mcMesh.name.c_str());
            fbxMesh->CreateLayer();
            SetControlPoints(fbxMesh, mcMesh, bakeMat);
            AddNormals(fbxMesh, mcMesh, bakeMat);
            AddUVs(fbxMesh, mcMesh);

            if (!mcMesh.sections.empty())
            {
                EnsureNodeMaterials(meshNode, mcMesh);
                bool hasNodeMaterial = meshNode->GetMaterialCount() > 0;
                FbxLayerElementMaterial *matElem = nullptr;
                if (hasNodeMaterial)
                {
                    matElem = fbxMesh->CreateElementMaterial();
                    matElem->SetMappingMode(FbxLayerElement::eByPolygon);
                    matElem->SetReferenceMode(FbxLayerElement::eIndexToDirect);
                }

                for (const auto &sec : mcMesh.sections)
                {
                    int matIdx = EnsureNodeMaterial(meshNode, sec.materialId);
                    AddSectionPolygons(fbxMesh, mcMesh, sec.indexOffset, sec.indexCount, matIdx);

                    if (matElem)
                    {
                        int fillIdx = (matIdx >= 0) ? matIdx : 0;
                        uint32_t polyCount = sec.indexCount / 3;
                        for (uint32_t p = 0; p < polyCount; ++p)
                            matElem->GetIndexArray().Add(fillIdx);
                    }
                }
            }
            else if (!mcMesh.indices.empty())
            {
                AddSectionPolygons(fbxMesh, mcMesh, 0, (uint32_t)mcMesh.indices.size(), -1);
            }

            return fbxMesh;
        }

        // ---- Nodes ----
        FbxNode *MakeSceneNode(const Node &mcNode)
        {
            auto *fbxNode = FbxNode::Create(m_fbxScene, mcNode.name.c_str());

            // 设置旋转顺序为 EulerXYZ，与 QuatToEulerDegrees 保持一致
            fbxNode->SetRotationOrder(FbxNode::eSourcePivot, FbxEuler::eOrderXYZ);

            // localMatrix -> FbxAMatrix
            FbxAMatrix mat = ToFbxAMatrix(mcNode.localMatrix);

            // 蒙皮网格节点：自身的局部变换必须被完全丢弃（既不留在节点上，也不烘焙进顶点），
            // 并且必须被 reparent 到场景根节点下（见 WireRootNodes）。
            // 原因（对应 glTF 蒙皮规范的等价推导）：蒙皮顶点的世界坐标由
            //   v_world = Σ w_i · jointGlobal_i · invBindMatrix_i · v_localRaw
            // 给出，公式中不含网格节点自身变换这一项——glTF 官方教程的
            // jointMatrix = meshNodeGlobal^-1 · jointGlobal_i · invBindMatrix_i
            // 里的 meshNodeGlobal^-1 会与渲染时重新乘上的 meshNodeGlobal 精确抵消。
            // 因此 v_localRaw 必须是未经任何变换的原始顶点数据。
            // 若像之前那样把网格节点自身的 localMatrix 烘焙进顶点（bakeMat=mat），
            // 等价于多算了一次该矩阵，只要它不是单位矩阵（例如 RiggedSimple.glb 中
            // mesh 节点 "Cylinder" 与骨骼分属两个独立的根节点子树，自身带一个非单位
            // 旋转矩阵），蒙皮结果就会出现倒置/拉伸。
            // 无论节点自身是静止的还是被 TRS 动画驱动（例如 FishAnimated.glb 的
            // 网格节点 "Fish" 自身也带一段 TRS 动画），都必须丢弃——蒙皮网格节点自身
            // 的变换与是否被动画驱动无关，在渲染时都会被完全抵消。
            // 此外，仅清零局部变换不足以保证 meshNodeGlobal=I：如果网格节点有带变换
            // 的父节点（如 RiggedSimple.glb 中 Cylinder 的父节点 Armature 有旋转），
            // 世界变换仍不为单位矩阵。因此必须同时 reparent 到根节点，才能满足
            // BuildClustersForSkin 中 TransformMatrix=I 的不变量要求。
            bool isSkinnedMesh = !mcNode.meshIds.empty() &&
                                 m_skinnedMeshIds.count(mcNode.meshIds[0]) > 0;

            if (isSkinnedMesh)
            {
                fbxNode->LclTranslation.Set(FbxDouble3(0.0, 0.0, 0.0));
                fbxNode->LclRotation.Set(FbxDouble3(0.0, 0.0, 0.0));
                fbxNode->LclScaling.Set(FbxDouble3(1.0, 1.0, 1.0));
            }
            else
            {
                fbxNode->LclTranslation.Set(mat.GetT());
                Quaternion q = QuaternionFromMatrixRotation(mcNode.localMatrix.m);
                fbxNode->LclRotation.Set(QuatToEulerDegrees(q));
                fbxNode->LclScaling.Set(mat.GetS());
            }

            if (!mcNode.meshIds.empty())
            {
                auto srcIt = m_meshSrcMap.find(mcNode.meshIds[0]);
                if (srcIt != m_meshSrcMap.end())
                {
                    FbxMesh *fbxMesh = nullptr;
                    auto built = m_meshMap.find(mcNode.meshIds[0]);
                    if (built != m_meshMap.end())
                    {
                        fbxMesh = built->second;
                        EnsureNodeMaterials(fbxNode, *srcIt->second);
                    }
                    else
                    {
                        // 蒙皮网格：v_localRaw 须保持未变换的原始顶点数据（见上方推导），
                        // 因此 bakeMat 恒为单位矩阵，不烘焙 mcNode.localMatrix。
                        FbxAMatrix bakeMat;
                        fbxMesh = BuildMesh(fbxNode, *srcIt->second, bakeMat);
                        m_meshMap[mcNode.meshIds[0]] = fbxMesh;
                    }
                    if (fbxMesh)
                        fbxNode->SetNodeAttribute(fbxMesh);
                }
            }

            return fbxNode;
        }

        void CreateNodes(const Scene &scene)
        {
            for (const auto &mcNode : scene.nodes)
            {
                FbxNode *fbxNode = MakeSceneNode(mcNode);
                m_nodeMap[mcNode.id] = fbxNode;
            }
            for (const auto &mcNode : scene.nodes)
            {
                FbxNode *fbxNode = m_nodeMap[mcNode.id];
                for (ObjectID childId : mcNode.children)
                {
                    // 蒙皮网格节点不挂载到原始父节点下，而是由 WireRootNodes
                    // 直接挂到场景根节点，确保其世界变换恒为单位矩阵
                    if (m_skinnedMeshNodeIds.count(childId) > 0)
                        continue;
                    auto it = m_nodeMap.find(childId);
                    if (it != m_nodeMap.end())
                        fbxNode->AddChild(it->second);
                }
            }
        }

        void WireRootNodes(const Scene &scene)
        {
            FbxNode *root = m_fbxScene->GetRootNode();
            for (ObjectID rootId : scene.rootNodes)
            {
                // 蒙皮网格节点跳过，后面统一添加到根节点
                if (m_skinnedMeshNodeIds.count(rootId) > 0)
                    continue;
                auto it = m_nodeMap.find(rootId);
                if (it != m_nodeMap.end())
                    root->AddChild(it->second);
            }
            // 蒙皮网格节点直接挂到场景根节点，确保其世界变换恒为单位矩阵。
            // glTF 规范规定蒙皮网格节点的变换在渲染时被完全忽略，蒙皮结果
            // 完全由骨骼驱动，因此 reparent 不改变视觉效果。
            // 关键不变量：TransformMatrix=I 要求 meshNodeGlobal=I，
            // 仅清零局部变换不够，还必须脱离带变换的父节点链。
            for (const auto &mcNode : scene.nodes)
            {
                if (m_skinnedMeshNodeIds.count(mcNode.id) > 0)
                {
                    auto it = m_nodeMap.find(mcNode.id);
                    if (it != m_nodeMap.end())
                        root->AddChild(it->second);
                }
            }
        }

        // ---- Skeleton / Skin ----
        // mc::Bone 不直接持有 nodeId（与 FbxSceneConverter 导入方向一致的设计），
        // 跨对象引用须按名字匹配场景 Node，见 GltfExporter::BuildSkinJointsFromBones 的相同模式。
        void AddSkeletonAttributes(const Scene &scene)
        {
            for (const auto &mcNode : scene.nodes)
            {
                if (mcNode.type != NodeType::Bone)
                    continue;
                auto it = m_nodeMap.find(mcNode.id);
                if (it == m_nodeMap.end())
                    continue;

                auto *skelAttr = FbxSkeleton::Create(m_fbxScene, (mcNode.name + "_Skel").c_str());
                skelAttr->SetSkeletonType(FbxSkeleton::eLimbNode);
                it->second->SetNodeAttribute(skelAttr);
            }
        }

        static FbxAMatrix ToFbxAMatrix(const Matrix4 &m)
        {
            FbxAMatrix mat;
            for (int col = 0; col < 4; ++col)
                for (int row = 0; row < 4; ++row)
                    mat.mData[col][row] = m.m[col * 4 + row];
            return mat;
        }

        // 从列主序矩阵的旋转部分提取四元数（trace 算法）。
        // 与 FbxAMatrix::GetR() 的欧拉分解相比，trace 算法在 180°旋转等退化配置下
        // 数值稳定、结果唯一确定，不会像欧拉分解那样在等价解之间产生歧义分支
        // （这正是 fish5.glb 静止朝向在不同 FBX 解析器间出现 180°差异的根因：
        // 节点自身旋转四元数恰好是绕 (0,1,1)/√2 轴的 180°旋转，属于 Euler-XYZ
        // 分解的退化配置，FbxAMatrix::GetR() 与本文件动画曲线所用的
        // QuatToEulerDegrees 若各自选中不同的等价解，两者数值上都"正确"，
        // 但组合旋转矩阵可能因解析器内部实现差异被还原为相反朝向）。
        // 静态旋转与动画曲线统一改用同一条 四元数→Euler 路径，消除该不一致。
        static Quaternion QuaternionFromMatrixRotation(const float *m)
        {
            float sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
            float sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
            float sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
            if (sx < 1e-12f)
                sx = 1.0f;
            if (sy < 1e-12f)
                sy = 1.0f;
            if (sz < 1e-12f)
                sz = 1.0f;

            float r00 = m[0] / sx, r01 = m[4] / sy, r02 = m[8] / sz;
            float r10 = m[1] / sx, r11 = m[5] / sy, r12 = m[9] / sz;
            float r20 = m[2] / sx, r21 = m[6] / sy, r22 = m[10] / sz;

            float trace = r00 + r11 + r22, qx, qy, qz, qw;
            if (trace > 0.0f)
            {
                float s = std::sqrt(trace + 1.0f) * 2.0f;
                qw = 0.25f * s;
                qx = (r21 - r12) / s;
                qy = (r02 - r20) / s;
                qz = (r10 - r01) / s;
            }
            else if (r00 > r11 && r00 > r22)
            {
                float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
                qw = (r21 - r12) / s;
                qx = 0.25f * s;
                qy = (r01 + r10) / s;
                qz = (r02 + r20) / s;
            }
            else if (r11 > r22)
            {
                float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
                qw = (r02 - r20) / s;
                qx = (r01 + r10) / s;
                qy = 0.25f * s;
                qz = (r12 + r21) / s;
            }
            else
            {
                float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
                qw = (r10 - r01) / s;
                qx = (r02 + r20) / s;
                qy = (r12 + r21) / s;
                qz = 0.25f * s;
            }

            float qLen = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
            if (qLen > 1e-6f)
            {
                qx /= qLen;
                qy /= qLen;
                qz /= qLen;
                qw /= qLen;
            }
            else
            {
                qx = 0.0f;
                qy = 0.0f;
                qz = 0.0f;
                qw = 1.0f;
            }
            return Quaternion(qx, qy, qz, qw);
        }

        FbxNode *FindNodeByName(const Scene &scene, const std::string &name) const
        {
            for (const auto &n : scene.nodes)
            {
                if (n.name != name)
                    continue;
                auto it = m_nodeMap.find(n.id);
                if (it != m_nodeMap.end())
                    return it->second;
            }
            return nullptr;
        }

        FbxNode *FindMeshFbxNode(const Scene &scene, ObjectID meshId) const
        {
            for (const auto &n : scene.nodes)
            {
                bool hasMesh = std::find(n.meshIds.begin(), n.meshIds.end(), meshId) != n.meshIds.end();
                if (!hasMesh)
                    continue;
                auto it = m_nodeMap.find(n.id);
                if (it != m_nodeMap.end())
                    return it->second;
            }
            return nullptr;
        }

        // 同时构建 FbxCluster 并把同一份绑定姿态矩阵记录进 bindPose。
        // 关键点 1：bindPose 里的矩阵必须和 cluster 的 TransformLinkMatrix 完全一致，
        // 不能用 boneNode->EvaluateGlobalTransform() 现取（那是节点当前/静止姿态，
        // 如果静止姿态与真正的绑定姿态不一致，查看器在没有 BindPose 时
        // 会拿当前姿态去配 TransformLinkMatrix，导致蒙皮网格变形/倒置/拉伸）。
        // 关键点 2：cluster->TransformMatrix 固定写单位矩阵，不要填网格节点的世界变换。
        // FBX SDK 官方公式确实支持非单位的 TransformMatrix，Blender Legacy 等基于 FBX SDK
        // 的导入器能正确处理；但绝大多数非 SDK / 严格遵循事实标准的解析器
        // （Blender 现行 io_scene_fbx、fbxreview、f3d、Win11 3D 查看器等）按照业界
        // 事实惯例，默认 TransformMatrix 恒为单位矩阵，会忽略或错误处理非单位值，
        // 表现为蒙皮网格倒置/拉伸。
        // 这个选择与 MakeSceneNode + WireRootNodes 中的处理是同一个不变量的三面，必须配对成立：
        // 完整蒙皮公式为 v_world(t) = meshNodeGlobal(t) · Σ w_i · linkGlobal_i(t) ·
        // linkGlobal_i(bind)^-1 · TransformMatrix(bind) · v_local。TransformMatrix
        // 固定为单位矩阵后，要让公式正确简化为"忽略网格节点自身变换"，就必须同时
        // 保证 meshNodeGlobal(t) 恒为单位矩阵——这需要三步配对操作：
        //   1) MakeSceneNode 中清零蒙皮网格节点的 LclTranslation/Rotation/Scaling
        //   2) WireRootNodes 中将蒙皮网格节点 reparent 到场景根节点（脱离带变换的父节点链）
        //   3) AddAnimations 中跳过给蒙皮网格节点添加 TRS 动画曲线
        // 缺少步骤 2 时，即使局部变换清零，父节点的旋转仍会使世界变换≠I，
        // 导致 Blender/Babylon.js 等解析器中蒙皮与骨骼之间出现旋转偏差。
        // 若以后要改动这里的 TransformMatrix，必须同步检查上述三步。
        void BuildClustersForSkin(const Scene &scene, const Skeleton &skel,
                                  FbxSkin *fbxSkin, FbxPose *bindPose,
                                  std::vector<FbxCluster *> &clusters)
        {
            const FbxAMatrix identity;

            for (size_t bi = 0; bi < skel.bones.size(); ++bi)
            {
                const Bone &bone = skel.bones[bi];
                FbxNode *boneNode = FindNodeByName(scene, bone.name);
                if (!boneNode)
                    continue;

                auto *cluster = FbxCluster::Create(m_fbxScene, (bone.name + "_Cluster").c_str());
                cluster->SetLink(boneNode);
                cluster->SetLinkMode(FbxCluster::eNormalize);

                // bone.inverseBindPose 是绑定姿态世界矩阵的逆，求逆得到绑定姿态本身
                FbxAMatrix bindWorld = ToFbxAMatrix(bone.inverseBindPose).Inverse();
                cluster->SetTransformLinkMatrix(bindWorld);
                cluster->SetTransformMatrix(identity);

                clusters[bi] = cluster;
                fbxSkin->AddCluster(cluster);

                if (bindPose->Find(boneNode) < 0)
                    bindPose->Add(boneNode, FbxMatrix(bindWorld));
            }
        }

        void AddSkinControlPointWeights(const Mesh &mcMesh, const std::vector<FbxCluster *> &clusters)
        {
            for (uint32_t vi = 0; vi < mcMesh.skinInfluences.size(); ++vi)
            {
                for (const auto &inf : mcMesh.skinInfluences[vi])
                {
                    if (inf.joint >= clusters.size() || !clusters[inf.joint])
                        continue;
                    clusters[inf.joint]->AddControlPointIndex((int)vi, inf.weight);
                }
            }
        }

        void AddSkins(const Scene &scene)
        {
            if (scene.skins.empty())
                return;

            int skinCount = 0;
            for (const auto &mcSkin : scene.skins)
            {
                const Skeleton *skel = nullptr;
                for (const auto &s : scene.skeletons)
                    if (s.id == mcSkin.skeletonId)
                    {
                        skel = &s;
                        break;
                    }
                if (!skel || skel->bones.empty())
                    continue;

                auto meshIt = m_meshMap.find(mcSkin.meshId);
                auto meshSrcIt = m_meshSrcMap.find(mcSkin.meshId);
                if (meshIt == m_meshMap.end() || meshSrcIt == m_meshSrcMap.end())
                    continue;

                FbxMesh *fbxMesh = meshIt->second;
                const Mesh *mcMesh = meshSrcIt->second;
                if (mcMesh->skinInfluences.empty())
                    continue;

                auto *fbxSkin = FbxSkin::Create(m_fbxScene, mcSkin.name.c_str());

                FbxNode *meshNode = FindMeshFbxNode(scene, mcSkin.meshId);
                FbxAMatrix meshWorld = meshNode ? meshNode->EvaluateGlobalTransform() : FbxAMatrix();

                auto *bindPose = FbxPose::Create(m_fbxScene, (mcSkin.name + "_BindPose").c_str());
                bindPose->SetIsBindPose(true);
                if (meshNode)
                    bindPose->Add(meshNode, FbxMatrix(meshWorld));

                std::vector<FbxCluster *> clusters(skel->bones.size(), nullptr);
                BuildClustersForSkin(scene, *skel, fbxSkin, bindPose, clusters);
                AddSkinControlPointWeights(*mcMesh, clusters);

                fbxMesh->AddDeformer(fbxSkin);
                m_fbxScene->AddPose(bindPose);
                ++skinCount;
            }

            Logger::Instance().LogInfo(
                "FbxExporter: exported " + std::to_string(skinCount) + " skin(s).");
        }

        // ---- BlendShape（Morph Target）----
        void AddBlendShapeChannelsForMesh(const Mesh &mcMesh, FbxMesh *fbxMesh)
        {
            auto *blendShape = FbxBlendShape::Create(m_fbxScene, (mcMesh.name + "_BlendShape").c_str());
            fbxMesh->AddDeformer(blendShape);

            for (size_t ti = 0; ti < mcMesh.morphTargets.size(); ++ti)
            {
                const MorphTarget &mt = mcMesh.morphTargets[ti];
                std::string chName = mt.name.empty() ? ("Morph_" + std::to_string(ti)) : mt.name;

                auto *shape = FbxShape::Create(m_fbxScene, chName.c_str());
                shape->InitControlPoints((int)mcMesh.positions.size());
                FbxVector4 *sp = shape->GetControlPoints();
                for (size_t vi = 0; vi < mcMesh.positions.size(); ++vi)
                {
                    const Vec3 &base = mcMesh.positions[vi];
                    Vec3 delta = (vi < mt.positionDeltas.size()) ? mt.positionDeltas[vi] : Vec3();
                    sp[vi] = FbxVector4(base.x + delta.x, base.y + delta.y, base.z + delta.z, 1.0);
                }

                auto *channel = FbxBlendShapeChannel::Create(m_fbxScene, chName.c_str());
                channel->AddTargetShape(shape);
                blendShape->AddBlendShapeChannel(channel);

                uint64_t key = (static_cast<uint64_t>(mcMesh.id) << 32) | static_cast<uint64_t>(ti);
                m_morphChannelMap[key] = channel;
            }
        }

        void AddBlendShapes(const Scene &scene)
        {
            int meshCount = 0;
            for (const auto &mcMesh : scene.meshes)
            {
                if (mcMesh.morphTargets.empty())
                    continue;
                auto meshIt = m_meshMap.find(mcMesh.id);
                if (meshIt == m_meshMap.end())
                    continue;

                AddBlendShapeChannelsForMesh(mcMesh, meshIt->second);
                ++meshCount;
            }

            if (meshCount > 0)
                Logger::Instance().LogInfo(
                    "FbxExporter: exported blend shapes for " + std::to_string(meshCount) + " mesh(es).");
        }

        // ---- Animations（Phase14）----
        // Quaternion → Euler 角度（度），EulerXYZ 旋转顺序
        static FbxDouble3 QuatToEulerDegrees(const Quaternion &q)
        {
            float invLen = 1.0f / std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            float x = q.x * invLen, y = q.y * invLen, z = q.z * invLen, w = q.w * invLen;

            float sinRy = 2.0f * (w * y - z * x);
            sinRy = std::clamp(sinRy, -1.0f, 1.0f);

            constexpr float kRadToDeg = 180.0f / 3.14159265358979f;

            // 万向锁（|sinRy|≈1，即 Y≈±90°）：此时 X、Z 两轴退化为只剩 1 个自由度
            // （只有 X∓Z 这个组合角有意义），常规 atan2 的分子分母同时趋近 0，
            // 对四元数的浮点噪声极度敏感，逐帧取值会在退化区间内剧烈来回跳动
            // （即"动画抖动"问题的根因）。退化时按惯例把 Z 固定为 0，
            // 用一个数值稳定的组合角公式把全部旋转折算到 X 上。
            constexpr float kGimbalEpsilon = 0.9999f;
            if (std::fabs(sinRy) > kGimbalEpsilon)
            {
                float r01 = 2.0f * (x * y - w * z);
                float r02 = 2.0f * (x * z + w * y);
                float thetaX = (sinRy > 0.0f) ? std::atan2(r01, r02) : std::atan2(-r01, -r02);
                return FbxDouble3(thetaX * kRadToDeg, std::asin(sinRy) * kRadToDeg, 0.0);
            }

            return FbxDouble3(
                std::atan2(2.0f * (w * x + y * z), 1.0f - 2.0f * (x * x + y * y)) * kRadToDeg,
                std::asin(sinRy) * kRadToDeg,
                std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (y * y + z * z)) * kRadToDeg);
        }

        // 归一化角度到 (-180, 180]
        static double NormalizeAngle(double a)
        {
            return a - std::floor((a + 180.0) / 360.0) * 360.0;
        }

        // EulerXYZ 替代解：(X+180, 180-Y, Z+180) 给出相同的旋转
        static FbxDouble3 AltEuler(const FbxDouble3 &e)
        {
            return FbxDouble3(
                NormalizeAngle(e[0] + 180.0),
                NormalizeAngle(180.0 - e[1]),
                NormalizeAngle(e[2] + 180.0));
        }

        // 从主解和替代解中选出与 prev 最连续的欧拉表示（先逐轴 unwrap 再比距离）
        static FbxDouble3 UnwrapEuler(const FbxDouble3 &curr, const FbxDouble3 &prev)
        {
            FbxDouble3 candidates[2] = {curr, AltEuler(curr)};

            // 对每个候选先做逐轴 unwrap（用 floor 公式，对任意输入都正确）
            for (auto &c : candidates)
                for (int i = 0; i < 3; ++i)
                    c[i] -= std::floor((c[i] - prev[i] + 180.0) / 360.0) * 360.0;

            auto sqDist = [](const FbxDouble3 &a, const FbxDouble3 &b)
            {
                double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
                return dx * dx + dy * dy + dz * dz;
            };

            return sqDist(candidates[0], prev) <= sqDist(candidates[1], prev)
                       ? candidates[0]
                       : candidates[1];
        }

        void AddAnimCurve(FbxAnimCurve *curve, const std::vector<KeyFrame<float>> &keys,
                          FbxAnimCurveDef::EInterpolationType interp)
        {
            if (!curve)
                return;
            curve->KeyModifyBegin();
            for (const auto &kf : keys)
            {
                FbxTime fbxtime;
                fbxtime.SetSecondDouble(kf.time);
                int keyIdx = curve->KeyAdd(fbxtime);
                curve->KeySetValue(keyIdx, kf.value);
                curve->KeySetInterpolation(keyIdx, interp);
            }
            curve->KeyModifyEnd();
        }

        void AddAnimCurve(FbxAnimCurve *curve, const std::vector<KeyFrame<Vec3>> &keys,
                          int component, FbxAnimCurveDef::EInterpolationType interp)
        {
            if (!curve)
                return;
            curve->KeyModifyBegin();
            for (const auto &kf : keys)
            {
                FbxTime fbxtime;
                fbxtime.SetSecondDouble(kf.time);
                int keyIdx = curve->KeyAdd(fbxtime);
                float val = (&kf.value.x)[component];
                curve->KeySetValue(keyIdx, val);
                curve->KeySetInterpolation(keyIdx, interp);
            }
            curve->KeyModifyEnd();
        }

        static FbxAnimCurveDef::EInterpolationType ToFbxInterp(AnimationInterpolation interp)
        {
            switch (interp)
            {
            case AnimationInterpolation::Step:
                return FbxAnimCurveDef::eInterpolationConstant;
            case AnimationInterpolation::CubicSpline:
                return FbxAnimCurveDef::eInterpolationCubic;
            default:
                return FbxAnimCurveDef::eInterpolationLinear;
            }
        }

        void AddNodeAnimation(FbxNode *fbxNode, const NodeAnimation &nodeAnim,
                              FbxAnimLayer *layer)
        {
            FbxAnimCurveDef::EInterpolationType tInterp = ToFbxInterp(nodeAnim.translation.interpolation);
            FbxAnimCurveDef::EInterpolationType rInterp = ToFbxInterp(nodeAnim.rotation.interpolation);
            FbxAnimCurveDef::EInterpolationType sInterp = ToFbxInterp(nodeAnim.scale.interpolation);

            // Translation
            if (!nodeAnim.translation.keys.empty())
            {
                FbxAnimCurveNode *tNode = fbxNode->LclTranslation.GetCurveNode(layer, true);
                if (tNode)
                {
                    AddAnimCurve(tNode->CreateCurve(tNode->GetName(), FBXSDK_CURVENODE_COMPONENT_X),
                                 nodeAnim.translation.keys, 0, tInterp);
                    AddAnimCurve(tNode->CreateCurve(tNode->GetName(), FBXSDK_CURVENODE_COMPONENT_Y),
                                 nodeAnim.translation.keys, 1, tInterp);
                    AddAnimCurve(tNode->CreateCurve(tNode->GetName(), FBXSDK_CURVENODE_COMPONENT_Z),
                                 nodeAnim.translation.keys, 2, tInterp);
                }
            }

            // Rotation: Quaternion → Euler（度），逐帧 unwrap 保证角度连续
            if (!nodeAnim.rotation.keys.empty())
            {
                FbxAnimCurveNode *rNode = fbxNode->LclRotation.GetCurveNode(layer, true);
                if (rNode)
                {
                    FbxAnimCurve *rx = rNode->CreateCurve(rNode->GetName(), FBXSDK_CURVENODE_COMPONENT_X);
                    FbxAnimCurve *ry = rNode->CreateCurve(rNode->GetName(), FBXSDK_CURVENODE_COMPONENT_Y);
                    FbxAnimCurve *rz = rNode->CreateCurve(rNode->GetName(), FBXSDK_CURVENODE_COMPONENT_Z);

                    if (rx)
                        rx->KeyModifyBegin();
                    if (ry)
                        ry->KeyModifyBegin();
                    if (rz)
                        rz->KeyModifyBegin();

                    // 以节点已设置的静态 LclRotation（MakeSceneNode 中由 FbxAMatrix::GetR() 得到）
                    // 作为 unwrap 的起点，而不是从"无参考"开始。否则第一个关键帧的 Euler
                    // 候选选择与静态姿态无关，可能选到数学等价但数值上相差 180°/翻转的另一组解，
                    // 造成查看器在"显示静态姿态"与"求值动画第 0 帧"之间出现可见的瞬间翻转
                    // （例如 fish5 鱼头在 Blender 初次导入时方向错误，播放动画后又恢复正常）。
                    FbxDouble3 prevEuler = fbxNode->LclRotation.Get();
                    bool hasPrev = true;

                    for (const auto &kf : nodeAnim.rotation.keys)
                    {
                        FbxDouble3 euler = QuatToEulerDegrees(kf.value);

                        if (hasPrev)
                            euler = UnwrapEuler(euler, prevEuler);

                        FbxTime fbxtime;
                        fbxtime.SetSecondDouble(kf.time);

                        if (rx)
                        {
                            int ki = rx->KeyAdd(fbxtime);
                            rx->KeySetValue(ki, static_cast<float>(euler[0]));
                            rx->KeySetInterpolation(ki, rInterp);
                        }
                        if (ry)
                        {
                            int ki = ry->KeyAdd(fbxtime);
                            ry->KeySetValue(ki, static_cast<float>(euler[1]));
                            ry->KeySetInterpolation(ki, rInterp);
                        }
                        if (rz)
                        {
                            int ki = rz->KeyAdd(fbxtime);
                            rz->KeySetValue(ki, static_cast<float>(euler[2]));
                            rz->KeySetInterpolation(ki, rInterp);
                        }

                        prevEuler = euler;
                        hasPrev = true;
                    }

                    if (rx)
                        rx->KeyModifyEnd();
                    if (ry)
                        ry->KeyModifyEnd();
                    if (rz)
                        rz->KeyModifyEnd();
                }
            }

            // Scale
            if (!nodeAnim.scale.keys.empty())
            {
                FbxAnimCurveNode *sNode = fbxNode->LclScaling.GetCurveNode(layer, true);
                if (sNode)
                {
                    AddAnimCurve(sNode->CreateCurve(sNode->GetName(), FBXSDK_CURVENODE_COMPONENT_X),
                                 nodeAnim.scale.keys, 0, sInterp);
                    AddAnimCurve(sNode->CreateCurve(sNode->GetName(), FBXSDK_CURVENODE_COMPONENT_Y),
                                 nodeAnim.scale.keys, 1, sInterp);
                    AddAnimCurve(sNode->CreateCurve(sNode->GetName(), FBXSDK_CURVENODE_COMPONENT_Z),
                                 nodeAnim.scale.keys, 2, sInterp);
                }
            }
        }

        void AddMorphAnimation(const MorphAnimation &morphAnim, FbxAnimLayer *layer)
        {
            uint64_t key = (static_cast<uint64_t>(morphAnim.meshId) << 32) | morphAnim.morphIndex;
            auto it = m_morphChannelMap.find(key);
            if (it == m_morphChannelMap.end())
                return;

            FbxAnimCurveDef::EInterpolationType interp = ToFbxInterp(morphAnim.weights.interpolation);
            FbxAnimCurve *curve = it->second->DeformPercent.GetCurve(layer, true);
            if (!curve)
                return;

            curve->KeyModifyBegin();
            for (const auto &kf : morphAnim.weights.keys)
            {
                FbxTime fbxtime;
                fbxtime.SetSecondDouble(kf.time);
                int keyIdx = curve->KeyAdd(fbxtime);
                // mc 内部权重 0..1 → FBX DeformPercent 0..100
                curve->KeySetValue(keyIdx, kf.value * 100.0f);
                curve->KeySetInterpolation(keyIdx, interp);
            }
            curve->KeyModifyEnd();
        }

        void AddAnimations(const Scene &scene)
        {
            if (scene.animations.empty())
                return;

            int exportedCount = 0;
            for (const auto &clip : scene.animations)
            {
                if (clip.nodeChannels.empty() && clip.morphChannels.empty())
                    continue;

                // 创建 FbxAnimStack
                FbxAnimStack *animStack = FbxAnimStack::Create(m_fbxScene, clip.name.c_str());
                FbxTime startFbxTime, stopFbxTime;
                startFbxTime.SetSecondDouble(clip.startTime);
                stopFbxTime.SetSecondDouble(clip.endTime);
                FbxTimeSpan timeSpan;
                timeSpan.Set(startFbxTime, stopFbxTime);
                animStack->SetLocalTimeSpan(timeSpan);

                // 创建 FbxAnimLayer
                FbxAnimLayer *layer = FbxAnimLayer::Create(m_fbxScene, "BaseLayer");
                animStack->AddMember(layer);

                // 为每个 NodeAnimation 添加曲线（含骨骼，骨骼节点与普通节点同一套 TRS 通道逻辑）
                for (const auto &nodeAnim : clip.nodeChannels)
                {
                    auto nodeIt = m_nodeMap.find(nodeAnim.nodeId);
                    if (nodeIt == m_nodeMap.end())
                        continue;

                    // 蒙皮网格节点自身的变换已在 MakeSceneNode 中强制清零并丢弃
                    // （渲染时会被完全抵消），此处不再给它添加 TRS 动画曲线，
                    // 否则等于重新引入一份非单位变换，抵消上面的修复。
                    if (m_skinnedMeshNodeIds.count(nodeAnim.nodeId) > 0)
                        continue;

                    AddNodeAnimation(nodeIt->second, nodeAnim, layer);
                }

                // 为每个 MorphAnimation 添加 BlendShape 权重曲线
                for (const auto &morphAnim : clip.morphChannels)
                    AddMorphAnimation(morphAnim, layer);

                ++exportedCount;
            }

            Logger::Instance().LogInfo(
                "FbxExporter: exported " + std::to_string(exportedCount) +
                " animation clip(s).");
        }
    };

    // ============================================================
    // CanExport
    // ============================================================
    bool FbxExporter::CanExport(const std::string &ext) const
    {
        std::string lower = ext;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lower == ".fbx";
    }

    // ============================================================
    // Export  （高层调度，< 60 行）
    // ============================================================
    VoidResult FbxExporter::Export(const Scene &scene, ExportContext &ctx)
    {
        if (ctx.outputPath.empty())
            return {false, "FbxExporter: outputPath is empty"};

        FbxManager *mgr = FbxManager::Create();
        FbxIOSettings *ios = FbxIOSettings::Create(mgr, IOSROOT);
        mgr->SetIOSettings(ios);
        // 受 ctx.options.embedTextures 控制：true 时把已连接 FbxVideo 的纹理文件内容内嵌进 .fbx；
        // false 时仅写外部文件、FBX 内部只保留文件名引用。顺序参照官方样例 Common.cxx::SaveScene，
        // 须在 Initialize() 之前设置。
        ios->SetBoolProp(EXP_FBX_MATERIAL, true);
        ios->SetBoolProp(EXP_FBX_TEXTURE, true);
        ios->SetBoolProp(EXP_FBX_EMBEDDED, ctx.options.embedTextures);
        ios->SetBoolProp(EXP_FBX_SHAPE, true);
        ios->SetBoolProp(EXP_FBX_ANIMATION, true);
        ios->SetBoolProp(EXP_FBX_GLOBAL_SETTINGS, true);

        FbxScene *fbxScene = FbxScene::Create(mgr, "ExportedScene");
        fbxScene->GetGlobalSettings().SetAxisSystem(FbxAxisSystem::MayaYUp);
        fbxScene->GetGlobalSettings().SetSystemUnit(FbxSystemUnit::m);

        FbxBuilder builder(mgr, fbxScene, ctx.options,
                           std::filesystem::path(ctx.outputPath).parent_path());
        builder.Build(scene);

        // 已验证：本项目内置的 FBX SDK 2020.3.9 的 Binary writer 不会执行
        // EXP_FBX_EMBEDDED 内嵌（无论 IOSettings 如何设置，连接的 FbxVideo 都不会写入文件），
        // 而 ASCII writer 行为正常、能正确内嵌媒体。因此 embedTextures=true 时改用 ASCII 格式导出，
        // 换取贴图真正内嵌；false 时仍用更紧凑的 Binary 格式（此时本就不需要内嵌）。
        int fileFormat = mgr->GetIOPluginRegistry()->GetNativeWriterFormat();
        /*if (ctx.options.embedTextures)
        {
            int asciiFmt = mgr->GetIOPluginRegistry()->FindWriterIDByDescription("FBX ascii (*.fbx)");
            if (asciiFmt >= 0) fileFormat = asciiFmt;
        }*/

        fbxsdk::FbxExporter *fbxExp = fbxsdk::FbxExporter::Create(mgr, "");
        bool initOk = fbxExp->Initialize(ctx.outputPath.c_str(), fileFormat, mgr->GetIOSettings());
        if (!initOk)
        {
            std::string err = fbxExp->GetStatus().GetErrorString();
            fbxExp->Destroy();
            fbxScene->Destroy();
            mgr->Destroy();
            return {false, std::string("FbxExporter: Initialize failed: ") + err};
        }

        bool exportOk = fbxExp->Export(fbxScene);
        fbxExp->Destroy();
        fbxScene->Destroy();

        if (!exportOk)
        {
            mgr->Destroy();
            return {false, std::string("FbxExporter: export failed for '") + ctx.outputPath + "'"};
        }

        builder.CleanupEmbeddedTextureFiles();
        mgr->Destroy();

        ctx.meshesExported = scene.MeshCount();
        ctx.materialsExported = scene.MaterialCount();
        ctx.texturesExported = scene.TextureCount();
        ctx.nodesExported = scene.NodeCount();

        Logger::Instance().LogInfo(
            std::string("FbxExporter: exported ") +
            std::to_string(ctx.meshesExported) + " mesh(es), " +
            std::to_string(ctx.materialsExported) + " material(s) -> " + ctx.outputPath);

        return {true, ""};
    }

} // namespace mc
