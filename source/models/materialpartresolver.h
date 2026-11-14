#pragma once

#include "componentmanager.h"
#include "material.h"
#include "scene.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------------------------
// メッシュ名 / マテリアル名の分類（PMX / VRM / FBX / glTF 共通）
//
// 最適化：
//  - 以前は ResolveMaterialPartId の中で AI_MATKEY_NAME を再取得し、「material + " " + mesh」の
//    文字列を毎回組み立て直してトークン表を最大 8 回走査していた。
//  - ここでは読み込み側ですでに取得済みの名前を受け取り、ASCII だけを小文字化した 1 本のキー
//    （MeshSemanticKey）を 1 回だけ作る。部位 ID とメカクレ用の意味フラグを同じ走査で求め、
//    キーは MeshData に保存して毎フレームの名前フィルター判定でも再利用する。
//  - tolower はロケール依存で UTF-8 の多バイト文字を壊すことがあるため、ASCII だけを変換する。
//    これで「前髪」「目」「眉」などの日本語トークンも安全に照合できる。
// ---------------------------------------------------------------------------------------------

enum MeshSemanticFlags : uint32_t
{
    MeshSemantic_None = 0,
    MeshSemantic_Hair = 1u << 0,
    MeshSemantic_Bangs = 1u << 1,         // 前髪
    MeshSemantic_Eye = 1u << 2,           // 白目 / 瞳 / 虹彩
    MeshSemantic_EyeHighlight = 1u << 3,  // 目のハイライト
    MeshSemantic_Brow = 1u << 4,          // 眉
    MeshSemantic_Lash = 1u << 5,          // まつ毛 / アイライン
    MeshSemantic_Face = 1u << 6,
    MeshSemantic_Skin = 1u << 7,
    MeshSemantic_Cloth = 1u << 8,
    MeshSemantic_Accessory = 1u << 9,
    MeshSemantic_Head = 1u << 10,         // 頭部（SNI 球面法線補間用）
};

// メカクレで髪越しに透かす既定の対象。
inline constexpr uint32_t kMekakureDefaultTargetMask =
    MeshSemantic_Eye | MeshSemantic_EyeHighlight | MeshSemantic_Brow | MeshSemantic_Lash;

inline const vector<string_view> kMaterialPartWearMaterialTokens = {"wear", "other"};

inline const vector<string_view> kMaterialPartWearMeshTokens = {"other", "other02"};

inline const vector<string_view> kMaterialPartAccessoryTokens = {"ribbon", "bow",   "choker",    "collar",   "necklace",
                                                                 "lace",   "frill", "accessory", "ornament", "deco"};

inline const vector<string_view> kMaterialPartHairTokens = {"hair", "kami", "bang", "brow", "eyelash"};

inline const vector<string_view> kMaterialPartSkinTokens = {"skin", "body", "face", "head", "hand",  "arm", "leg",
                                                            "neck", "ear",  "eye",  "iris", "pupil", "hada"};

inline const vector<string_view> kMaterialPartClothTokens = {"cloth", "dress", "shoe", "socks", "skirt"};

// 意味フラグと目のトゥーン部位を判定するトークン。
// VRoid 系 VRM（F00_000_00_FaceBrow_00_FACE など）、MMD 系 PMX（前髪 / 目 / 眉 / まつ毛）、
// 一般的な FBX（Eye_L, Brow, Lashes, Bangs）を想定。
inline const vector<string_view> kSemanticBangsTokens = {"bang", "fringe", "maegami", "front_hair", "fronthair",
                                                         "hair_front", "hairfront", "\xE5\x89\x8D\xE9\xAB\xAA" /*前髪*/};
inline const vector<string_view> kSemanticHairTokens = {"hair", "kami", "\xE9\xAB\xAA" /*髪*/, "\xE3\x81\x8B\xE3\x81\xBF" /*かみ*/};
inline const vector<string_view> kSemanticHighlightTokens = {
    "highlight", "eye_hl", "eyehl", "hilight", "\xE3\x83\x8F\xE3\x82\xA4\xE3\x83\xA9\xE3\x82\xA4\xE3\x83\x88" /*ハイライト*/};
inline const vector<string_view> kSemanticLashTokens = {
    "lash",       "eyeline",    "eye_line", "matsuge",
    "\xE3\x81\xBE\xE3\x81\xA4\xE6\xAF\x9B" /*まつ毛*/, "\xE3\x81\xBE\xE3\x81\xA4\xE3\x81\x92" /*まつげ*/,
    "\xE7\x9D\xAB" /*睫*/, "\xE4\xBA\x8C\xE9\x87\x8D" /*二重*/};
inline const vector<string_view> kSemanticBrowTokens = {"brow", "mayu", "\xE7\x9C\x89" /*眉*/};
inline const vector<string_view> kSemanticEyeTokens = {
    "eye",  "iris", "pupil", "hitomi", "shirome",
    "\xE7\x9E\xB3" /*瞳*/, "\xE7\x99\xBD\xE7\x9B\xAE" /*白目*/, "\xE7\x9B\xAE" /*目*/};
inline const vector<string_view> kScleraTokens = {"sclera", "eye_white", "eyewhite", "shirome",
                                                  "\xE7\x99\xBD\xE7\x9B\xAE" /*白目*/};
inline const vector<string_view> kSemanticFaceTokens = {"face", "head", "kao", "\xE9\xA1\x94" /*顔*/};
inline const vector<string_view> kSemanticSkinTokens = {"skin", "body", "hada", "\xE8\x82\x8C" /*肌*/};

inline string MaterialPartToLowerString(string value)
{
    // ASCII だけを小文字化する（UTF-8 の後続バイトを壊さない）。
    for (char& c : value)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

inline bool MaterialPartContainsAnyToken(string_view value, const vector<string_view>& tokens)
{
    for (string_view token : tokens)
    {
        if (value.find(token) != string_view::npos)
        {
            return true;
        }
    }
    return false;
}

struct MeshSemanticInfo
{
    float MaterialPartId = 10.0f;
    uint32_t Flags = MeshSemantic_None;
    string Key; // 小文字化した "material mesh"。名前フィルター用に保存する。
};

// 取得済みの名前から部位 ID と意味フラグを 1 回で求める。
inline MeshSemanticInfo ResolveMeshSemantics(string_view materialNameRaw, string_view meshNameRaw,
                                             unsigned int materialIndex)
{
    MeshSemanticInfo info;
    info.Key.reserve(materialNameRaw.size() + meshNameRaw.size() + 1);
    info.Key.append(materialNameRaw);
    info.Key.push_back(' ');
    info.Key.append(meshNameRaw);
    info.Key = MaterialPartToLowerString(std::move(info.Key));

    const string_view key = info.Key;
    const string_view materialName = key.substr(0, materialNameRaw.size());
    const string_view meshName = key.substr(materialNameRaw.size() + 1);

    // --- 部位 ID（従来の ResolveMaterialPartId と同じ優先順位・同じ結果） ---
    const bool hairToken = MaterialPartContainsAnyToken(key, kMaterialPartHairTokens);
    const bool skinToken = MaterialPartContainsAnyToken(key, kMaterialPartSkinTokens);
    if (MaterialPartContainsAnyToken(materialName, kMaterialPartWearMaterialTokens) ||
        MaterialPartContainsAnyToken(meshName, kMaterialPartWearMeshTokens) ||
        (materialName.find("face_alpha") != string_view::npos && meshName.find("other") != string_view::npos) ||
        MaterialPartContainsAnyToken(key, kMaterialPartAccessoryTokens))
    {
        info.MaterialPartId = 2.0f;
        info.Flags |= MeshSemantic_Accessory;
    }
    else if (hairToken)
    {
        info.MaterialPartId = 1.0f;
    }
    else if (skinToken)
    {
        info.MaterialPartId = 3.0f;
    }
    else if (MaterialPartContainsAnyToken(key, kMaterialPartClothTokens))
    {
        info.MaterialPartId = 2.0f;
        info.Flags |= MeshSemantic_Cloth;
    }
    else if (materialIndex == 0)
    {
        info.MaterialPartId = 1.0f;
    }
    else if (materialIndex == 2)
    {
        info.MaterialPartId = 3.0f;
    }
    else
    {
        info.MaterialPartId = 2.0f;
    }

    // --- 意味フラグ（目・眉・まつ毛は髪より優先して判定） ---
    const bool highlight = MaterialPartContainsAnyToken(key, kSemanticHighlightTokens);
    const bool lash = MaterialPartContainsAnyToken(key, kSemanticLashTokens);
    const bool brow = MaterialPartContainsAnyToken(key, kSemanticBrowTokens);
    const bool eye = MaterialPartContainsAnyToken(key, kSemanticEyeTokens);
    const bool eyeGlint = (highlight && (eye || key.find("hl") != string_view::npos)) ||
                          materialName == "\xE3\x83\x8F\xE3\x82\xA4\xE3\x83\xA9\xE3\x82\xA4\xE3\x83\x88" /*ハイライト*/ ||
                          materialName == "\xE5\x85\x89\xE5\xBD\xA9" /*光彩*/;
    if (eyeGlint)
    {
        info.Flags |= MeshSemantic_EyeHighlight;
    }
    if (lash)
    {
        info.Flags |= MeshSemantic_Lash;
    }
    if (brow)
    {
        info.Flags |= MeshSemantic_Brow;
    }
    if (eye && !lash && !brow)
    {
        info.Flags |= MeshSemantic_Eye;
    }
    if (eyeGlint || (eye && !lash && !brow))
    {
        const bool sclera = MaterialPartContainsAnyToken(key, kScleraTokens);
        info.MaterialPartId = static_cast<float>(eyeGlint ? MeshShadingOverride::ToonEyeHighlight
                                                         : (sclera ? MeshShadingOverride::ToonSclera
                                                                   : MeshShadingOverride::ToonIris));
    }
    const bool facialPart = (info.Flags & kMekakureDefaultTargetMask) != 0;
    if (!facialPart)
    {
        if (MaterialPartContainsAnyToken(key, kSemanticBangsTokens))
        {
            info.Flags |= MeshSemantic_Hair | MeshSemantic_Bangs;
        }
        else if (MaterialPartContainsAnyToken(key, kSemanticHairTokens))
        {
            info.Flags |= MeshSemantic_Hair;
        }
        if (MaterialPartContainsAnyToken(key, kSemanticFaceTokens))
        {
            info.Flags |= MeshSemantic_Face;
        }
        if (MaterialPartContainsAnyToken(key, kSemanticSkinTokens))
        {
            info.Flags |= MeshSemantic_Skin;
        }
        // 頭部判定：Face または Skin のメッシュを頭部として扱う（SNI 球面法線補間用）
        if ((info.Flags & (MeshSemantic_Face | MeshSemantic_Skin)) != 0)
        {
            info.Flags |= MeshSemantic_Head;
        }
    }
    return info;
}

// 互換 API（既存呼び出し用）。新規コードは ResolveMeshSemantics を使う。
inline float ResolveMaterialPartId(const aiScene* scene, const aiMesh* mesh)
{
    if (!scene || !mesh || mesh->mMaterialIndex >= scene->mNumMaterials)
    {
        return 10.0f;
    }
    aiString name;
    scene->mMaterials[mesh->mMaterialIndex]->Get(AI_MATKEY_NAME, name);
    return ResolveMeshSemantics(name.C_Str(), mesh->mName.C_Str(), mesh->mMaterialIndex).MaterialPartId;
}

// カンマ区切りの名前フィルター（小文字化済みキーに対して部分一致）。空なら false。
inline bool MeshSemanticKeyMatchesFilter(string_view key, string_view filter)
{
    size_t begin = 0;
    while (begin < filter.size())
    {
        size_t end = filter.find(',', begin);
        if (end == string_view::npos)
        {
            end = filter.size();
        }
        string_view token = filter.substr(begin, end - begin);
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
        {
            token.remove_prefix(1);
        }
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
        {
            token.remove_suffix(1);
        }
        if (!token.empty())
        {
            const string lowered = MaterialPartToLowerString(string(token));
            if (key.find(lowered) != string_view::npos)
            {
                return true;
            }
        }
        begin = end + 1;
    }
    return false;
}


// メカクレのパーツ番号（MekakurePart と同じ並び：0 目, 1 目ハイライト, 2 眉, 3 まつ毛, 4 顔, 5 追加対象名）。
// 対象マスクで有効なビットのうち、細かい部位を優先する（ハイライト > まつ毛 > 眉 > 目 > 顔）。
inline int ResolveMekakurePartIndex(uint32_t semanticFlags, uint32_t targetMask)
{
    const uint32_t flags = semanticFlags & targetMask;
    if (flags & MeshSemantic_EyeHighlight)
    {
        return 1;
    }
    if (flags & MeshSemantic_Lash)
    {
        return 3;
    }
    if (flags & MeshSemantic_Brow)
    {
        return 2;
    }
    if (flags & MeshSemantic_Eye)
    {
        return 0;
    }
    if (flags & MeshSemantic_Face)
    {
        return 4;
    }
    return 5;
}
