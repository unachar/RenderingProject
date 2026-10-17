#include "pch.h"
#include "materialmanager.h"
#include "assetthumbnailmanager.h"
#include "thumbnailvalidation.h"
#include "imguimanager.h"
#include "editorwidgets.h"
#include "materialsystem.h"
#include "modelmanager.h"
#include "virtualgeometry/virtualgeometrydebug.h"
#include "texturemanager.h"
#include "world.h"
#include "scenefile.h"
#include "assetregistry.h"
#include <json/json.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <DirectXCollision.h>
#include <ImGuizmo.h>
#include <imgui_internal.h>
#include <cstring>



using namespace EditorWidgets;

namespace
{
	// アセットブラウザ内でフォルダをドラッグするときのペイロード（wchar_t のパス、終端込み）
	constexpr const char* kAssetDirectoryPayload = "ASSET_DIR";
	// 複数選択をまとめてドラッグするときのペイロード（中身は件数。パスは m_DraggedAssetPaths）
	constexpr const char* kAssetMultiPayload = "ASSET_MULTI";

	// ImGui の入力（UTF-8）→ path。u8path は C++20 で非推奨のため自前で変換
	filesystem::path Utf8ToPath(const char* text)
	{
		if (!text || text[0] == '\0') return {};
		const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
		if (length <= 1) return {};
		wstring wide(static_cast<size_t>(length - 1), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), length);
		return filesystem::path(wide);
	}

	// ---- サムネイルの無いアセットのアイコン（cellSize 四方。サムネイル画像と同じ大きさ・同じ背景）
	void DrawAssetIcon(ImDrawList* drawList, const ImVec2& origin, float size, const filesystem::path& path, bool isDirectory)
	{
		const ImVec2 maxCorner(origin.x + size, origin.y + size);
		const float rounding = (std::max)(2.0f, size * 0.04f);
		// サムネイルの背景色に合わせたタイル
		drawList->AddRectFilled(origin, maxCorner, IM_COL32(30, 33, 42, 255), rounding);

		const float unit = size / 16.0f; // 16 分割のグリッドで形を決めて、サイズに比例させる
		auto at = [&](float x, float y) { return ImVec2(origin.x + x * unit, origin.y + y * unit); };

		string extension = path.extension().generic_string();
		transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });

		if (isDirectory)
		{
			const ImU32 back = IM_COL32(214, 160, 40, 255);
			const ImU32 front = IM_COL32(255, 205, 70, 255);
			// タブ + 奥の板 + 手前の板
			drawList->AddRectFilled(at(2.0f, 3.5f), at(7.0f, 5.5f), back, unit * 0.8f, ImDrawFlags_RoundCornersTop);
			drawList->AddRectFilled(at(2.0f, 4.5f), at(14.0f, 13.0f), back, unit * 0.8f);
			drawList->AddRectFilled(at(2.0f, 6.0f), at(14.0f, 13.0f), front, unit * 0.8f);
			drawList->AddLine(at(2.6f, 6.6f), at(13.4f, 6.6f), IM_COL32(255, 235, 160, 160), (std::max)(1.0f, unit * 0.35f));
			return;
		}

		if (extension == ".material")
		{
			// 球（マテリアルのプレビューと同じ見た目の簡易版）
			const ImVec2 center = at(8.0f, 8.0f);
			const float radius = unit * 5.6f;
			drawList->AddCircleFilled(center, radius, IM_COL32(40, 170, 210, 255), 48);
			drawList->AddCircleFilled(ImVec2(center.x + radius * 0.12f, center.y + radius * 0.12f), radius * 0.82f, IM_COL32(60, 200, 240, 255), 48);
			drawList->AddCircleFilled(ImVec2(center.x - radius * 0.38f, center.y - radius * 0.38f), radius * 0.22f, IM_COL32(225, 248, 255, 230), 24);
			return;
		}

		// ファイル：右上を折った紙 + 種類ごとの色帯 + 拡張子
		ImU32 accent = IM_COL32(120, 170, 230, 255);
		if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" || extension == ".tga" || extension == ".dds" || extension == ".hdr")
			accent = IM_COL32(90, 200, 110, 255);
		else if (extension == ".fbx" || extension == ".obj" || extension == ".gltf" || extension == ".glb" || extension == ".vrm" || extension == ".pmx" || extension == ".vgmesh")
			accent = IM_COL32(245, 150, 70, 255);
		else if (extension == ".scene")
			accent = IM_COL32(110, 140, 255, 255);
		else if (extension == ".hlsl" || extension == ".hlsli" || extension == ".slang")
			accent = IM_COL32(190, 110, 245, 255);
		else if (extension == ".wav" || extension == ".ogg" || extension == ".mp3")
			accent = IM_COL32(240, 200, 80, 255);

		const ImU32 paper = IM_COL32(222, 226, 234, 255);
		const float fold = 3.2f;
		// 本体（折り返し部分を除いた多角形）
		const ImVec2 points[5] = { at(3.5f, 1.5f), at(12.5f - fold, 1.5f), at(12.5f, 1.5f + fold), at(12.5f, 14.5f), at(3.5f, 14.5f) };
		drawList->AddConvexPolyFilled(points, 5, paper);
		// 折り返し
		drawList->AddTriangleFilled(at(12.5f - fold, 1.5f), at(12.5f - fold, 1.5f + fold), at(12.5f, 1.5f + fold), IM_COL32(160, 166, 178, 255));
		// 色帯
		drawList->AddRectFilled(at(3.5f, 9.0f), at(12.5f, 12.6f), accent);
		// 本文の線
		const float lineWidth = (std::max)(1.0f, unit * 0.45f);
		drawList->AddLine(at(5.0f, 5.0f), at(9.0f, 5.0f), IM_COL32(150, 156, 168, 255), lineWidth);
		drawList->AddLine(at(5.0f, 7.0f), at(11.0f, 7.0f), IM_COL32(150, 156, 168, 255), lineWidth);

		// 拡張子（帯に収まるときだけ）
		string label = extension.size() > 1 ? extension.substr(1) : string("FILE");
		transform(label.begin(), label.end(), label.begin(), [](unsigned char c) { return static_cast<char>(toupper(c)); });
		if (label.size() > 5) label.resize(5);
		const float bandWidth = unit * 9.0f, bandHeight = unit * 3.6f;
		const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
		if (textSize.y <= bandHeight + 2.0f && textSize.x <= bandWidth - 2.0f)
		{
			const ImVec2 bandMin = at(3.5f, 9.0f);
			drawList->AddText(ImVec2(bandMin.x + (bandWidth - textSize.x) * 0.5f, bandMin.y + (bandHeight - textSize.y) * 0.5f), IM_COL32(255, 255, 255, 255), label.c_str());
		}
	}

	string PathToUtf8(const filesystem::path& path)
	{
		const auto text = path.generic_u8string();
		return string(text.begin(), text.end());
	}

	filesystem::path CanonicalOf(const filesystem::path& path)
	{
		error_code ec;
		const filesystem::path absolute = filesystem::absolute(path, ec);
		if (ec) return path.lexically_normal();
		filesystem::path canonical = filesystem::weakly_canonical(absolute, ec);
		return ec ? absolute.lexically_normal() : canonical;
	}

	// child が parent と同じ、または parent の中にあるか（字句比較。両方 CanonicalOf 済みを想定）
	bool IsSameOrInside(const filesystem::path& child, const filesystem::path& parent)
	{
		const filesystem::path relative = child.lexically_relative(parent);
		if (relative.empty()) return false;
		const auto first = *relative.begin();
		return first != "..";
	}

	// 作業フォルダからの相対パス（ブラウザが扱う "asset/..." 形式に揃える）
	filesystem::path ToWorkingRelative(const filesystem::path& path)
	{
		error_code ec;
		const filesystem::path relative = filesystem::relative(path, filesystem::current_path(ec), ec);
		return ec || relative.empty() ? path.lexically_normal() : relative;
	}

	// oldPrefix 自身、または oldPrefix/ 以下を指す文字列パスを newPrefix に付け替える
	bool RemapPathString(string& value, const string& oldPrefix, const string& newPrefix)
	{
		if (value.empty() || oldPrefix.empty()) return false;
		string normalized = value;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		if (normalized == oldPrefix)
		{
			value = newPrefix;
			return true;
		}
		if (normalized.size() > oldPrefix.size() && normalized.compare(0, oldPrefix.size(), oldPrefix) == 0 && normalized[oldPrefix.size()] == '/')
		{
			value = newPrefix + normalized.substr(oldPrefix.size());
			return true;
		}
		return false;
	}
}

void ImGuiManager::DrawAssetBrowserWindow()
{
    if (m_AssetBrowserStandalone)
    {
        ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(40, 60), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(1000, 480), ImGuiCond_Once);
    }
    else ImGui::SetNextWindowSize(ImVec2(720.0f, 260.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("プロジェクト", &m_ShowAssetBrowser))
	{
		m_AssetBrowserFocused = false;
		ImGui::End();
		return;
	}
	m_AssetBrowserFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

	error_code ec;
	if (!filesystem::exists(m_AssetRoot, ec))
	{
		filesystem::create_directories(m_AssetRoot, ec);
	}
	if (!filesystem::exists(m_CurrentAssetDirectory, ec) || !filesystem::is_directory(m_CurrentAssetDirectory, ec))
	{
		m_CurrentAssetDirectory = m_AssetRoot;
	}

	ImGui::BeginDisabled(m_CurrentAssetDirectory == m_AssetRoot);
	if (ImGui::Button("上へ"))
	{
		m_CurrentAssetDirectory = m_CurrentAssetDirectory.parent_path();
		m_SelectedAssetPath.clear();
	}
	ImGui::EndDisabled();
	if (m_CurrentAssetDirectory != m_AssetRoot && ImGui::BeginDragDropTarget())
	{
		AcceptAssetMoveDrop(m_CurrentAssetDirectory.parent_path());
		ImGui::EndDragDropTarget();
	}
	ImGui::SameLine();
	if (ImGui::Button("再スキャン"))
    {
        MaterialManager::Refresh();
    }
    ImGui::SameLine();
	if (ImGui::Button("ルート"))
	{
		m_CurrentAssetDirectory = m_AssetRoot;
		m_SelectedAssetPath.clear();
	}
	ImGui::SameLine();
	if (ImGui::Button("+ 作成")) ImGui::OpenPopup("CreateAssetPopup");

	// パンくずリスト：クリックで移動、アセットをドロップするとそのフォルダへ移動
	{
		const filesystem::path relative = m_CurrentAssetDirectory.lexically_relative(m_AssetRoot);
		vector<filesystem::path> crumbs{ m_AssetRoot };
		if (!relative.empty() && relative != "." && *relative.begin() != "..")
		{
			filesystem::path accumulated = m_AssetRoot;
			for (const auto& part : relative)
			{
				if (part.empty() || part == ".") continue;
				accumulated /= part;
				crumbs.push_back(accumulated);
			}
		}
		filesystem::path navigateTo;
		for (size_t index = 0; index < crumbs.size(); ++index)
		{
			ImGui::PushID(static_cast<int>(index));
			if (index > 0)
			{
				ImGui::SameLine(0.0f, 2.0f);
				ImGui::TextDisabled(">");
				ImGui::SameLine(0.0f, 2.0f);
			}
			const string label = PathToUtf8(crumbs[index].filename());
			if (ImGui::SmallButton(label.empty() ? "asset" : label.c_str())) navigateTo = crumbs[index];
			if (ImGui::BeginDragDropTarget())
			{
				AcceptAssetMoveDrop(crumbs[index]);
				ImGui::EndDragDropTarget();
			}
			ImGui::PopID();
		}
		if (!navigateTo.empty() && navigateTo != m_CurrentAssetDirectory)
		{
			m_CurrentAssetDirectory = navigateTo;
			m_SelectedAssetPath.clear();
		}
	}

	static char folderName[128] = "NewFolder";
	static char fileName[128] = "NewMaterial.material";
    static AssetHandle moveHandle = 0;
    static char movePath[512]{};
    bool moveRequested = false;
	if (ImGui::BeginPopup("CreateAssetPopup"))
	{
		ImGui::InputText("フォルダ名", folderName, IM_ARRAYSIZE(folderName));
		if (ImGui::Button("フォルダ作成") && folderName[0] != '\0')
		{
			const filesystem::path path = MakeUniqueAssetPath(m_CurrentAssetDirectory, Utf8ToPath(folderName), {});
			filesystem::create_directories(path, ec);
			if (!ec) AddLog("フォルダ作成: %s", PathToUtf8(path).c_str());
			m_AssetBrowserRescan = true;
		}
		ImGui::Separator();
		ImGui::InputText("ファイル名", fileName, IM_ARRAYSIZE(fileName));
        if (ImGui::Button("Material作成"))
        {
            filesystem::path stem = Utf8ToPath(fileName).stem();
            if (stem.empty()) stem = "NewMaterial";
            const filesystem::path materialPath = MakeUniqueAssetPath(m_CurrentAssetDirectory, stem, ".material");
            CreateAssetFile(materialPath);
            m_SelectedAssetPath = materialPath;
            m_AssetBrowserRescan = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Scene作成"))
        {
            CreateSceneAsset(m_CurrentAssetDirectory);
            ImGui::CloseCurrentPopup();
        }

		if (ImGui::Button("ファイル作成") && fileName[0] != '\0')
		{
			const filesystem::path name = Utf8ToPath(fileName);
			const filesystem::path newPath = MakeUniqueAssetPath(m_CurrentAssetDirectory, name.stem(), name.extension());
			CreateAssetFile(newPath);
			m_AssetBrowserRescan = true;
		}
		ImGui::EndPopup();
	}

    int thumbnailSize = static_cast<int>(AssetThumbnailManager::Settings().Size);
    ImGui::SetNextItemWidth(150);
    if (ImGui::SliderInt("サムネイルサイズ", &thumbnailSize, 32, 256)) AssetThumbnailManager::Settings().Size = static_cast<UINT>(thumbnailSize);
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveProjectSettings();
    ImGui::SameLine();
    if (ImGui::Button("サムネイル更新")) AssetThumbnailManager::Refresh();
    const float cellSize = static_cast<float>(thumbnailSize);
    const float rowHeight = cellSize + ImGui::GetTextLineHeightWithSpacing() + 12.0f;
    static ImGuiTextFilter assetFilter;
	static vector<filesystem::path> pendingDeletePaths;
	bool deleteRequested = false;
	DrawSearchField("AssetSearch", "ファイル名で検索...", assetFilter);
	// 矢印キーは自前で処理するため、このリストでは ImGui のキーボードナビゲーションを止める
	ImGui::BeginChild("AssetList", ImVec2(0, -ImGui::GetTextLineHeightWithSpacing()), true, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs);
	const bool listFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	SyncAssetSelection();

    static vector<filesystem::directory_entry> entries;
    static unordered_map<string, AssetHandle> browserHandles;
    static filesystem::path listedDirectory;
    static string listedFilter;
    static double nextScan = 0;
    const string currentFilter = assetFilter.InputBuf;
    if (m_AssetBrowserRescan || listedDirectory != m_CurrentAssetDirectory || listedFilter != currentFilter || ImGui::GetTime() >= nextScan)
    {
        m_AssetBrowserRescan = false;
        entries.clear();
        browserHandles.clear();
        listedDirectory = m_CurrentAssetDirectory;
        listedFilter = currentFilter;
        nextScan = ImGui::GetTime() + 0.5;
        for (const auto& entry : filesystem::directory_iterator(m_CurrentAssetDirectory, ec))
        {
            if (entry.path().extension() != ".meta" && entry.path().extension() != ".tmp" &&
                assetFilter.PassFilter(entry.path().filename().generic_string().c_str())) entries.push_back(entry);
        }
        sort(entries.begin(), entries.end(), [](const auto& a, const auto& b)
        {
            error_code firstError, secondError;
            const bool firstDirectory = a.is_directory(firstError), secondDirectory = b.is_directory(secondError);
            if (firstDirectory != secondDirectory) return firstDirectory > secondDirectory;
            return a.path().filename().generic_string() < b.path().filename().generic_string();
        });
    }
    if (ec) ImGui::TextWrapped("フォルダを読み込めません: %s", ec.message().c_str());
    else if (entries.empty()) ImGui::TextWrapped("表示するアセットがありません。");

    if (!m_SelectedAssetPath.empty())
    {
        const auto selectedType = AssetRegistry::Classify(m_SelectedAssetPath);
        if (selectedType == AssetType::Mesh)
        {
            VirtualGeometryDebug::DrawAssetInspector(m_SelectedAssetPath);
        }
        if (selectedType != AssetType::Unknown)
        {
            auto& handle = browserHandles[m_SelectedAssetPath.generic_string()];
            if (!handle)
            {
                string error;
                handle = MaterialManager::Registry().Register(m_SelectedAssetPath, selectedType, error);
            }
            AssetThumbnailManager::Request(handle, ThumbnailPriority::Selected);
        }
    }
    const int columnCount = std::clamp(static_cast<int>(ImGui::GetContentRegionAvail().x / (cellSize + 16.0f)), 1, 64);
    const float rowStride = rowHeight + ImGui::GetStyle().CellPadding.y * 2.0f;

    // 表示順のインデックス（Shift 範囲選択・矢印キー用）
    auto indexOf = [&](const filesystem::path& target) -> int
    {
        if (target.empty()) return -1;
        const wstring key = AssetSelectionKey(target);
        for (size_t index = 0; index < entries.size(); ++index)
        {
            if (AssetSelectionKey(entries[index].path()) == key) return static_cast<int>(index);
        }
        return -1;
    };
    // anchor〜target を選択（additive=true なら既存選択に追加）
    auto selectRange = [&](int anchorIndex, int targetIndex, bool additive)
    {
        if (!additive) ClearAssetSelection();
        if (anchorIndex < 0) anchorIndex = targetIndex;
        const int first = (min)(anchorIndex, targetIndex), last = (max)(anchorIndex, targetIndex);
        for (int index = first; index <= last; ++index) AddAssetToSelection(entries[index].path());
        m_SelectedAssetPath = entries[targetIndex].path(); // フォーカス項目（最後に追加）
        AddAssetToSelection(m_SelectedAssetPath);
    };

    // ---- キーボード（Windows エクスプローラーと同じ割り当て）
    const ImGuiIO& io = ImGui::GetIO();
    int keyboardFocusIndex = -1;
    if (listFocused && !io.WantTextInput && !ImGui::IsAnyItemActive() && m_RenamingAssetPath.empty())
    {
        const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
        if (ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_A, false))
        {
            ClearAssetSelection();
            for (const auto& entry : entries) AddAssetToSelection(entry.path());
            if (!entries.empty())
            {
                m_SelectedAssetPath = entries.front().path();
                m_AssetSelectionAnchor = m_SelectedAssetPath;
            }
        }
        else if (!ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ClearAssetSelection();
        }
        else if (!alt && ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !m_SelectedAssetPath.empty())
        {
            pendingDeletePaths = GetSelectedAssetPaths();
            deleteRequested = !pendingDeletePaths.empty();
        }
        else if (!ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_F2, false) && !m_SelectedAssetPath.empty())
        {
            BeginAssetRename(m_SelectedAssetPath);
        }
        else if (ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_C, false))
        {
            CopyAssetsToClipboard(false);
        }
        else if (ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_X, false))
        {
            CopyAssetsToClipboard(true);
        }
        else if (ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_V, false))
        {
            PasteAssetClipboard(m_CurrentAssetDirectory);
        }
        else if (ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_D, false))
        {
            DuplicateSelectedAssets();
        }
        else if (!ctrl && !alt && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) &&
            !m_SelectedAssetPath.empty())
        {
            OpenAssetItem(m_SelectedAssetPath);
        }
        else if ((ImGui::IsKeyPressed(ImGuiKey_Backspace, true) && !ctrl && !alt) ||
            (alt && ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)))
        {
            if (m_CurrentAssetDirectory != m_AssetRoot)
            {
                const filesystem::path previous = m_CurrentAssetDirectory;
                m_CurrentAssetDirectory = m_CurrentAssetDirectory.parent_path();
                SelectAssetOnly(previous); // 戻った先で元のフォルダを選択（エクスプローラーと同じ）
            }
        }
        else if (!alt && !entries.empty())
        {
            // 矢印 / Home / End：フォーカス移動。Shift で範囲拡張、Ctrl はフォーカスだけ動かす（Ctrl+Space で切り替え）
            const int count = static_cast<int>(entries.size());
            const int current = indexOf(m_SelectedAssetPath);
            int target = -1;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) target = current < 0 ? 0 : (min)(current + 1, count - 1);
            else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) target = current < 0 ? 0 : (max)(current - 1, 0);
            else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) target = current < 0 ? 0 : (min)(current + columnCount, count - 1);
            else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) target = current < 0 ? 0 : (max)(current - columnCount, 0);
            else if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) target = 0;
            else if (ImGui::IsKeyPressed(ImGuiKey_End, false)) target = count - 1;
            if (target >= 0)
            {
                if (shift)
                {
                    selectRange(indexOf(m_AssetSelectionAnchor), target, ctrl);
                }
                else if (ctrl)
                {
                    // Ctrl+矢印：選択は変えずにフォーカスだけ移動（フォーカス項目は選択に含める）
                    m_SelectedAssetPath = entries[target].path();
                    AddAssetToSelection(m_SelectedAssetPath);
                    m_AssetSelectionAnchor = m_SelectedAssetPath;
                }
                else
                {
                    SelectAssetOnly(entries[target].path());
                }
                keyboardFocusIndex = target;
            }
            else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Space, false) && current >= 0)
            {
                // Ctrl+Space：フォーカス項目の選択を切り替え
                const filesystem::path focus = entries[current].path();
                if (IsAssetSelected(focus))
                {
                    RemoveAssetFromSelection(focus);
                    m_SelectedAssetPath = m_SelectedAssetPaths.empty() ? filesystem::path{} : m_SelectedAssetPaths.back();
                }
                else
                {
                    AddAssetToSelection(focus);
                    m_SelectedAssetPath = focus;
                }
            }
        }
    }
    // キーボードで動かした項目が見えるようにスクロール（クリッパーで描画されない行もあるので位置を計算する）
    if (keyboardFocusIndex >= 0)
    {
        const float top = ImGui::GetCursorPosY() + static_cast<float>(keyboardFocusIndex / columnCount) * rowStride;
        const float viewHeight = ImGui::GetWindowHeight();
        if (top < ImGui::GetScrollY()) ImGui::SetScrollY(top);
        else if (top + rowStride > ImGui::GetScrollY() + viewHeight) ImGui::SetScrollY(top + rowStride - viewHeight);
    }

    // 矩形（ラバーバンド）選択
    static bool boxPending = false, boxActive = false;
    static ImVec2 boxStart{};
    static vector<filesystem::path> boxBaseSelection;
    struct VisibleCell { int Index; ImVec2 Min, Max; };
    static vector<VisibleCell> visibleCells;
    visibleCells.clear();
    if (ImGui::BeginTable("AssetGrid", columnCount, ImGuiTableFlags_SizingFixedSame))
    {
    for (int column = 0; column < columnCount; ++column) ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, cellSize + 8.0f);
    ImGuiListClipper clipper;
    clipper.Begin((static_cast<int>(entries.size()) + columnCount - 1) / columnCount, rowHeight + ImGui::GetStyle().CellPadding.y * 2.0f);
	while (clipper.Step())
	{
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
        {
            ImGui::TableNextRow(0, rowHeight);
            for (int column = 0; column < columnCount; ++column)
            {
            const int i = row * columnCount + column;
            if (i >= static_cast<int>(entries.size())) break;
            ImGui::TableSetColumnIndex(column);
			const filesystem::path path = entries[i].path();
			const bool isDir = entries[i].is_directory(ec);
            ec.clear();
			const string fileName = path.filename().generic_string();
			string displayName;
			if (fileName.empty())
			{
				displayName = path.generic_string();
			}
			else
			{
				displayName = fileName;
			}
			const string itemId = path.generic_string();

			const bool selected = IsAssetSelected(path);

			ImGui::PushID(itemId.c_str());

            const ImVec2 cellPosition = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::Selectable("##AssetCell", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(cellSize, rowHeight));
            visibleCells.push_back({ i, ImGui::GetItemRectMin(), ImGui::GetItemRectMax() });
            // 右クリックした項目が選択外なら、その項目だけを選択（エクスプローラーと同じ）
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !selected)
            {
                SelectAssetOnly(path);
            }
            // フォルダはドロップ先：ファイル / フォルダをこの中へ移動
            if (isDir && ImGui::BeginDragDropTarget())
            {
                AcceptAssetMoveDrop(path);
                ImGui::EndDragDropTarget();
            }
            ThumbnailValidation::CaptureAssetCell(fileName.c_str(), cellPosition.x, cellPosition.y, selected);

            ThumbnailHandle thumbnail;
            if (!isDir)
            {
                const auto type = AssetRegistry::Classify(path);
                if (type != AssetType::Unknown)
                {
                    string error;
                    const auto key = path.generic_string();
                    auto& handle = browserHandles[key];
                    if (!handle) handle = MaterialManager::Registry().Register(path, type, error);
                    thumbnail = AssetThumbnailManager::Request(handle, selected ? ThumbnailPriority::Selected : ThumbnailPriority::Visible);
                }
            }
            if (thumbnail.State == ThumbnailState::Ready && thumbnail.Image)
            {
                const auto size = FitThumbnail(thumbnail.Image->Width, thumbnail.Image->Height, cellSize);
                const ImVec2 top(cellPosition.x + (cellSize - size.Width) * 0.5f, cellPosition.y + (cellSize - size.Height) * 0.5f);
                // Draw on the cell without replacing the Selectable's interaction item.
                ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(thumbnail.GetImGuiTextureID().ptr), top, ImVec2(top.x + size.Width, top.y + size.Height));
            }
            else
            {
                // サムネイルが無い（フォルダ / 未対応ファイル / 生成待ち / 失敗）ときは、
                // モデル・マテリアルのサムネイルと同じ cellSize 四方のアイコンを描く
                DrawAssetIcon(ImGui::GetWindowDrawList(), cellPosition, cellSize, path, isDir);
            }
            if (thumbnail.State == ThumbnailState::Failed)
                ImGui::GetWindowDrawList()->AddText(ImVec2(cellPosition.x + cellSize - 12, cellPosition.y), IM_COL32(255, 110, 90, 255), "!");
            const ImVec2 labelPosition(cellPosition.x, cellPosition.y + cellSize + 4.0f);
            ImGui::GetWindowDrawList()->PushClipRect(labelPosition, ImVec2(cellPosition.x + cellSize, labelPosition.y + ImGui::GetTextLineHeight()), true);
            const bool renamingThis = !m_RenamingAssetPath.empty() && path == m_RenamingAssetPath;
            if (!renamingThis) ImGui::GetWindowDrawList()->AddText(labelPosition, ImGui::GetColorU32(ImGuiCol_Text), displayName.c_str());
            ImGui::GetWindowDrawList()->PopClipRect();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", displayName.c_str());
            // 選択はクリックを離したときに確定する（AllowDoubleClick の Selectable は離した時に clicked）。
            // そのため、選択済みの複数項目を掴んでドラッグしても選択は崩れない。
            if (clicked)
			{
				const bool ctrl = ImGui::GetIO().KeyCtrl, shift = ImGui::GetIO().KeyShift;
				const bool doubleClicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
				if (shift)
				{
					// Shift：起点〜クリック項目を範囲選択。Ctrl+Shift：既存選択に範囲を追加
					selectRange(indexOf(m_AssetSelectionAnchor), i, ctrl);
				}
				else if (ctrl)
				{
					// Ctrl：1件ずつ追加 / 解除。起点もこの項目にする
					if (IsAssetSelected(path)) RemoveAssetFromSelection(path);
					else
					{
						AddAssetToSelection(path);
						m_SelectedAssetPath = path;
					}
					m_AssetSelectionAnchor = path;
				}
				else
				{
					SelectAssetOnly(path);
					if (doubleClicked)
					{
						OpenAssetItem(path);
					}
				}
			}

			if (ImGui::BeginPopupContextItem("AssetItemContext"))
			{
				const size_t selectionCount = m_SelectedAssetKeys.size();
				if (selectionCount > 1)
				{
					// 複数選択時のメニュー
					ImGui::Text("%d 個の項目を選択中", static_cast<int>(selectionCount));
					ImGui::Separator();
					if (ImGui::MenuItem("切り取り", "Ctrl+X")) CopyAssetsToClipboard(true);
					if (ImGui::MenuItem("コピー", "Ctrl+C")) CopyAssetsToClipboard(false);
					if (isDir && ImGui::MenuItem("このフォルダに貼り付け", "Ctrl+V", false, !m_AssetClipboard.empty())) PasteAssetClipboard(path);
					if (ImGui::MenuItem("複製", "Ctrl+D")) DuplicateSelectedAssets();
					ImGui::Separator();
					if (ImGui::MenuItem("削除", "Delete"))
					{
						pendingDeletePaths = GetSelectedAssetPaths();
						deleteRequested = !pendingDeletePaths.empty();
					}
					ImGui::EndPopup();
				}
				else
				{
				ImGui::TextUnformatted(path.filename().generic_string().c_str());
				ImGui::Separator();
				if (ImGui::MenuItem("切り取り", "Ctrl+X")) CopyAssetsToClipboard(true);
				if (ImGui::MenuItem("コピー", "Ctrl+C")) CopyAssetsToClipboard(false);
				if (isDir && ImGui::MenuItem("このフォルダに貼り付け", "Ctrl+V", false, !m_AssetClipboard.empty())) PasteAssetClipboard(path);
				if (ImGui::MenuItem("複製", "Ctrl+D")) DuplicateSelectedAssets();
				ImGui::Separator();
				if (ImGui::MenuItem("名前を変更", "F2"))
				{
					BeginAssetRename(path);
				}
				if (isDir && ImGui::MenuItem("開く"))
				{
					m_CurrentAssetDirectory = path;
					m_SelectedAssetPath.clear();
				}
				if (isDir && ImGui::BeginMenu("このフォルダに作成"))
				{
					DrawAssetCreateMenuItems(path);
					ImGui::EndMenu();
				}
				if (!isDir && IsSceneFile(path) && ImGui::MenuItem("シーンを開く"))
				{
					RequestOpenScene(path);
				}
				if (!isDir && IsTextureFile(path) && ImGui::MenuItem("選択中にテクスチャ適用"))
				{
					const string relative = MakeRelativeAssetPath(path);
					if (m_SelectedEntity != g_kINVALID_ENTITY && Registry::IsAlive(m_SelectedEntity))
					{
						MaterialSystem::SetTexture(m_SelectedEntity, relative.c_str());
						AddLog("テクスチャ適用: %s", relative.c_str());
					}
				}
				if (!isDir && AssetRegistry::Classify(path) == AssetType::Material)
                {
                    string error;
                    const auto handle = MaterialManager::Registry().Register(path, AssetType::Material, error);
                    if (ImGui::MenuItem("マテリアルを編集"))
                    {
                        OpenMaterialAsset(handle);
                    }
                    if (ImGui::MenuItem("Duplicate"))
                    {
                        auto copyPath = path.parent_path() / (path.stem().string() + "_Copy.material");
                        unsigned int suffix = 1;
                        while (filesystem::exists(copyPath))
                        {
                            copyPath = path.parent_path() / (path.stem().string() + "_Copy" + to_string(suffix++) + ".material");
                        }
                        if (!MaterialManager::Registry().Duplicate(handle, copyPath, error))
                        {
                            AddLog("Duplicate failed: %s", error.c_str());
                        }
                    }
                    if (ImGui::MenuItem("Rename / Move"))
                    {
                        moveHandle = handle;
                        strncpy_s(movePath, path.generic_string().c_str(), _TRUNCATE);
                        moveRequested = true;
                    }
                }
                if (!isDir && AssetRegistry::Classify(path) != AssetType::Unknown && AssetRegistry::Classify(path) != AssetType::Material && ImGui::MenuItem("Rename / Move"))
                {
                    string error;
                    moveHandle = MaterialManager::Registry().Register(path, AssetRegistry::Classify(path), error);
                    strncpy_s(movePath, path.generic_string().c_str(), _TRUNCATE);
                    moveRequested = moveHandle != 0;
                }
                if (ImGui::MenuItem("削除", "Delete"))
                {
					pendingDeletePaths = { path };
					deleteRequested = true;
				}
				ImGui::EndPopup();
				}
			}

			BeginAssetDragSource(path, isDir);

			// リネーム入力欄はセルの最後に描く（右クリック / ドラッグの対象を Selectable のままにするため）
			if (renamingThis)
			{
				DrawAssetRenameField(path, isDir, labelPosition, cellSize);
			}

            ImGui::PopID();
            }
        }
    }
    ImGui::EndTable();
    }

	// ---- 背景クリック：選択解除 / 矩形選択の開始（Ctrl を押していれば既存選択に追加）
	if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
		m_RenamingAssetPath.empty())
	{
		boxPending = true;
		boxActive = false;
		boxStart = ImGui::GetMousePos();
		boxBaseSelection = ImGui::GetIO().KeyCtrl ? GetSelectedAssetPaths() : vector<filesystem::path>{};
		if (!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) ClearAssetSelection();
	}
	if (boxPending)
	{
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			boxPending = boxActive = false;
		}
		else
		{
			const ImVec2 mouse = ImGui::GetMousePos();
			if (!boxActive && (fabsf(mouse.x - boxStart.x) > 4.0f || fabsf(mouse.y - boxStart.y) > 4.0f)) boxActive = true;
			if (boxActive)
			{
				const ImVec2 boxMin((min)(boxStart.x, mouse.x), (min)(boxStart.y, mouse.y));
				const ImVec2 boxMax((max)(boxStart.x, mouse.x), (max)(boxStart.y, mouse.y));
				ClearAssetSelection();
				for (const auto& base : boxBaseSelection) AddAssetToSelection(base);
				int lastHit = -1;
				for (const auto& cell : visibleCells)
				{
					if (cell.Max.x >= boxMin.x && cell.Min.x <= boxMax.x && cell.Max.y >= boxMin.y && cell.Min.y <= boxMax.y)
					{
						AddAssetToSelection(entries[cell.Index].path());
						lastHit = cell.Index;
					}
				}
				if (lastHit >= 0)
				{
					m_SelectedAssetPath = entries[lastHit].path();
					m_AssetSelectionAnchor = m_SelectedAssetPath;
				}
				else if (!boxBaseSelection.empty())
				{
					m_SelectedAssetPath = boxBaseSelection.back();
				}
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				drawList->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_Header, 0.35f));
				drawList->AddRect(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_HeaderActive));
				// 端に寄せたら自動スクロール
				const ImVec2 windowMin = ImGui::GetWindowPos();
				const float windowBottom = windowMin.y + ImGui::GetWindowHeight();
				if (mouse.y < windowMin.y + 8.0f) ImGui::SetScrollY(ImGui::GetScrollY() - rowStride * 0.25f);
				else if (mouse.y > windowBottom - 8.0f) ImGui::SetScrollY(ImGui::GetScrollY() + rowStride * 0.25f);
			}
		}
	}

	// 背景の右クリック（アイテム上では開かない）。アイテムの後に呼ぶことで、ホバー判定がこのフレームの結果になる
	if (ImGui::BeginPopupContextWindow("AssetBackgroundContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
	{
		if (ImGui::MenuItem("貼り付け", "Ctrl+V", false, !m_AssetClipboard.empty())) PasteAssetClipboard(m_CurrentAssetDirectory);
		if (ImGui::MenuItem("すべて選択", "Ctrl+A"))
		{
			ClearAssetSelection();
			for (const auto& entry : entries) AddAssetToSelection(entry.path());
			if (!entries.empty()) { m_SelectedAssetPath = entries.front().path(); m_AssetSelectionAnchor = m_SelectedAssetPath; }
		}
		ImGui::Separator();
		DrawAssetCreateMenuItems(m_CurrentAssetDirectory);
		ImGui::EndPopup();
	}
	// 背景へのドロップ：表示中のフォルダへ移動（フォルダセルの上はセル側が優先）
	if (ImGuiWindow* listWindow = ImGui::GetCurrentWindow())
	{
		if (ImGui::BeginDragDropTargetCustom(listWindow->InnerRect, ImGui::GetID("AssetListBackgroundDrop")))
		{
			AcceptAssetMoveDrop(m_CurrentAssetDirectory);
			ImGui::EndDragDropTarget();
		}
	}
    ImGui::EndChild();
    if (moveRequested)
    {
        ImGui::OpenPopup("Rename / Move Asset");
    }
    if (ImGui::BeginPopupModal("Rename / Move Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputText("Destination", movePath, sizeof(movePath));
        if (ImGui::Button("Move"))
        {
            string error;
            if (MaterialManager::Registry().Move(moveHandle, movePath, error))
            {
                MaterialManager::Refresh();
                ImGui::CloseCurrentPopup();
            }
            else
            {
                AddLog("Move failed: %s", error.c_str());
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
	if (deleteRequested) ImGui::OpenPopup("アセットを削除");
	if (ImGui::BeginPopupModal("アセットを削除", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		if (pendingDeletePaths.size() > 1) ImGui::Text("%d 個の項目を削除します", static_cast<int>(pendingDeletePaths.size()));
		for (size_t index = 0; index < pendingDeletePaths.size() && index < 10; ++index)
		{
			ImGui::BulletText("%s", PathToUtf8(pendingDeletePaths[index]).c_str());
		}
		if (pendingDeletePaths.size() > 10) ImGui::TextDisabled("…ほか %d 件", static_cast<int>(pendingDeletePaths.size() - 10));
		ImGui::TextUnformatted("フォルダ内のファイルも削除されます。この操作は元に戻せません。");
		ImGui::Separator();
		if (ImGui::Button("削除する") || ImGui::IsKeyPressed(ImGuiKey_Enter, false))
		{
			for (const auto& target : pendingDeletePaths)
			{
				DeleteAssetPath(target);
				filesystem::remove(target.wstring() + L".meta", ec);
				// 削除したものがクリップボードに残らないように
				m_AssetClipboard.erase(remove_if(m_AssetClipboard.begin(), m_AssetClipboard.end(),
					[&](const filesystem::path& item) { return AssetSelectionKey(item) == AssetSelectionKey(target); }), m_AssetClipboard.end());
			}
            MaterialManager::Refresh();
			ClearAssetSelection();
			pendingDeletePaths.clear();
			m_AssetBrowserRescan = true;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("キャンセル") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			pendingDeletePaths.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	if (m_SelectedAssetKeys.size() > 1)
		ImGui::TextDisabled("%d 件 / %d 個選択 / Ctrl+クリック: 追加・解除 / Shift+クリック: 範囲 / ドラッグ: まとめて移動", static_cast<int>(entries.size()), static_cast<int>(m_SelectedAssetKeys.size()));
	else
		ImGui::TextDisabled("%d 件 / ダブルクリック: 開く・適用 / Ctrl・Shift+クリック: 複数選択 / ドラッグ: 配置・移動 / 右クリック: 作成", static_cast<int>(entries.size()));
	ImGui::End();
}

void ImGuiManager::ProcessDroppedFiles()
{
	if (m_DroppedFiles.empty())
	{
		return;
	}

	vector<string> dropped;
	dropped.swap(m_DroppedFiles);
	for (const string& path : dropped)
	{
		ImportAssetFile(path);
	}
}

void ImGuiManager::ImportAssetFile(const filesystem::path& sourcePath)
{
	error_code ec;
	if (!filesystem::exists(sourcePath, ec))
	{
		return;
	}

	filesystem::path destination = m_CurrentAssetDirectory / sourcePath.filename();
	if (filesystem::equivalent(sourcePath, destination, ec))
	{
		return;
	}

	if (filesystem::is_directory(sourcePath, ec))
	{
		filesystem::copy(sourcePath, destination, filesystem::copy_options::recursive | filesystem::copy_options::overwrite_existing, ec);
		if (!ec)
		{
			AddLog("アセット取り込み: %s", destination.generic_string().c_str());
		}
		return;
	}

	filesystem::copy_file(sourcePath, destination, filesystem::copy_options::overwrite_existing, ec);
	if (!ec && IsTextureFile(destination))
	{
		const string relative = MakeRelativeAssetPath(destination);
		TextureManager::LoadTexture(relative.c_str());
		AddLog("テクスチャ取り込み: %s", relative.c_str());
	}
	else if (!ec)
	{
		AddLog("アセット取り込み: %s", destination.generic_string().c_str());
	}
}

void ImGuiManager::CreateAssetFile(const filesystem::path& path)
{
	error_code ec;
	if (filesystem::exists(path, ec))
	{
		AddLog("作成スキップ（既に存在）: %s", path.generic_string().c_str());
		return;
	}

    if (AssetRegistry::Classify(path) == AssetType::Material)
    {
        string error;
        if (MaterialSerializer::Save(path, MaterialAssetData{}, error))
        {
            const auto handle = MaterialManager::Registry().Register(path, AssetType::Material, error);
            if (handle != 0)
            {
                AddLog("Material created: %s", path.generic_string().c_str());
                OpenMaterialAsset(handle);
            }
        }
        if (!error.empty())
        {
            AddLog("Material creation failed: %s", error.c_str());
        }
        return;
    }
	ofstream file(path);
	if (!file)
	{
		AddLog("作成失敗: %s", path.generic_string().c_str());
		return;
	}

    file << "# 新規アセット\n";

	AddLog("ファイル作成: %s", path.generic_string().c_str());
}

void ImGuiManager::DeleteAssetPath(const filesystem::path& path)
{
	error_code ec;
	if (path.empty() || !filesystem::exists(path, ec))
	{
		return;
	}

	filesystem::path relativeToRoot = filesystem::relative(path, m_AssetRoot, ec);
	if (ec || relativeToRoot.empty() || relativeToRoot.generic_string().starts_with(".."))
	{
		AddLog("削除中止（asset外）: %s", path.generic_string().c_str());
		return;
	}

	if (filesystem::is_directory(path, ec))
	{
		filesystem::remove_all(path, ec);
	}
	else
	{
		filesystem::remove(path, ec);
	}

	if (ec)
	{
		AddLog("削除失敗: %s", path.generic_string().c_str());
	}
	else
	{
		AddLog("削除: %s", path.generic_string().c_str());
	}
}

void ImGuiManager::PlaceAssetInScene(const filesystem::path& path, const ImVec2& sceneMouse)
{
	const string relative = MakeRelativeAssetPath(path);
	if (IsTextureFile(path))
	{
		TextureManager::LoadTexture(relative.c_str());
		if (m_SelectedEntity != g_kINVALID_ENTITY && Registry::IsAlive(m_SelectedEntity))
		{
			MaterialSystem::SetTexture(m_SelectedEntity, relative.c_str());
			AddLog("ドラッグ&ドロップでテクスチャ適用: %s", relative.c_str());
		}
		return;
	}

	if (!IsModelFile(path))
	{
		AddLog("シーンD&D無視: %s", relative.c_str());
		return;
	}

	XMVECTOR rayOrigin;
	XMVECTOR rayDir;
	if (!GetSceneViewRay(sceneMouse, rayOrigin, rayDir))
	{
		return;
	}

	float y = 0.0f;
	float originY = XMVectorGetY(rayOrigin);
	float dirY = XMVectorGetY(rayDir);
	float t;
	if (fabsf(dirY) > 0.0001f)
	{
		t = (y - originY) / dirY;
	}
	else
	{
		t = 5.0f;
	}
	if (t < 0.0f) t = 5.0f;
	XMVECTOR hit = XMVectorAdd(rayOrigin, XMVectorScale(rayDir, t));
	XMFLOAT3 pos{};
	XMStoreFloat3(&pos, hit);

	auto entity = World::CreateEntity()
		.Add<NameComponent>()
		.Add<TransformComponent>()
		.Add<MeshComponent>()
		.Add<AABBComponent>()
		.Add<MaterialComponent>()
		.Add<StaticModelComponent>();

	entity.SetName(path.stem().generic_string());
	const bool isConvert = ShouldConvertModelByPath(relative);
	auto& transform = entity.Get<TransformComponent>();
	transform.Position = pos;
	transform.Scale = { 0.03f, 0.03f, 0.03f };
	transform.Rotation = GetDefaultModelRotationByPath(relative, isConvert);
	transform.IsDirty = true;

	entity.Get<StaticModelComponent>().IsConvert = isConvert;
	entity.Get<StaticModelComponent>().ModelPath = relative;
	const int modelId = ModelManager::LoadStaticModel(relative.c_str(), isConvert);
	if (modelId < 0)
	{
		AddLog("モデル読み込み失敗: %s", relative.c_str());
		World::DestroyEntity(entity);
		return;
	}
	entity.Get<StaticModelComponent>().ModelId = modelId;
	if (auto* model = ModelManager::GetStaticModel(modelId))
	{
		entity.Get<AABBComponent>().Center = model->GetAabbCenter();
		entity.Get<AABBComponent>().Extents = model->GetAabbExtents();
	}
	else
	{
		entity.Get<AABBComponent>().Center = { 0.0f, 1.0f, 0.0f };
		entity.Get<AABBComponent>().Extents = { 0.7f, 1.0f, 0.7f };
	}

	auto& material = entity.Get<MaterialComponent>();
	material.TextureID = TextureManager::GetDefaultTextureIndex();
	material.UseTexture = true;
	material.ReceivingPostProcess = true;

	m_SelectedEntity = entity.GetID();
	AddLog("モデル配置: %s at %.2f %.2f %.2f", relative.c_str(), pos.x, pos.y, pos.z);
}

bool ImGuiManager::IsTextureFile(const filesystem::path& path)
{
	string ext = path.extension().generic_string();
	transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
	return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga" || ext == ".dds";
}

bool ImGuiManager::IsModelFile(const filesystem::path& path)
{
	string ext = path.extension().generic_string();
	transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
	return ext == ".fbx" || ext == ".obj" || ext == ".vrm" || ext == ".pmx" || ext == ".gltf" || ext == ".glb" || ext == ".vgmesh";
}

string ImGuiManager::MakeRelativeAssetPath(const filesystem::path& path)
{
	if (path.is_relative())
	{
		return path.generic_string();
	}

	error_code ec;
	filesystem::path relative = filesystem::relative(path, filesystem::current_path(), ec);
	if (ec)
	{
		relative = path;
	}
	return relative.generic_string();
}


// ============================================================================
// アセットブラウザ：作成メニュー / ユニーク名 / D&D 移動
// ============================================================================
filesystem::path ImGuiManager::MakeUniqueAssetPath(const filesystem::path& directory, const filesystem::path& stem, const filesystem::path& extension)
{
	error_code ec;
	const filesystem::path baseStem = stem.empty() ? filesystem::path("New") : stem;
	auto occupied = [&](const filesystem::path& candidate)
	{
		return filesystem::exists(candidate, ec) || filesystem::exists(filesystem::path(candidate.wstring() + L".meta"), ec);
	};
	filesystem::path candidate = directory / baseStem;
	candidate += extension;
	for (unsigned int suffix = 1; occupied(candidate) && suffix < 10000; ++suffix)
	{
		candidate = directory / baseStem;
		candidate += L" " + to_wstring(suffix);
		candidate += extension;
	}
	return candidate;
}

void ImGuiManager::DrawAssetCreateMenuItems(const filesystem::path& directory)
{
	error_code ec;
	if (ImGui::MenuItem("フォルダ"))
	{
		const filesystem::path path = MakeUniqueAssetPath(directory, "NewFolder", {});
		if (filesystem::create_directories(path, ec) && !ec)
		{
			AddLog("フォルダ作成: %s", PathToUtf8(path).c_str());
			m_SelectedAssetPath = path;
		}
		else
		{
			AddLog("フォルダ作成失敗: %s", PathToUtf8(path).c_str());
		}
		m_AssetBrowserRescan = true;
	}
	ImGui::Separator();
	if (ImGui::MenuItem("マテリアル"))
	{
		CreateMaterialAsset(directory);
	}
	if (ImGui::MenuItem("シーン"))
	{
		CreateSceneAsset(directory);
	}
	ImGui::Separator();
	if (ImGui::MenuItem("空ファイル"))
	{
		const filesystem::path path = MakeUniqueAssetPath(directory, "NewFile", ".txt");
		CreateAssetFile(path);
		m_SelectedAssetPath = path;
		m_AssetBrowserRescan = true;
	}
}

void ImGuiManager::CreateMaterialAsset(const filesystem::path& directory)
{
	error_code ec;
	filesystem::create_directories(directory, ec);
	const filesystem::path path = MakeUniqueAssetPath(directory, "NewMaterial", ".material");
	CreateAssetFile(path); // 保存 → GUID 登録 → マテリアルエディターを開く
	if (filesystem::exists(path, ec))
	{
		m_SelectedAssetPath = path;
	}
	m_AssetBrowserRescan = true;
}

void ImGuiManager::CreateSceneAsset(const filesystem::path& directory)
{
	error_code ec;
	filesystem::create_directories(directory, ec);
	const filesystem::path path = MakeUniqueAssetPath(directory, "NewScene", SceneFile::kExtension);

	// 空のシーン。createDefaults=true のシーンは、開いたときにエンティティが無ければ
	// 「新規シーン」と同じ既定のカメラと太陽を作る（Unity の新規シーンと同じ構成）。
	Json::Value root(Json::objectValue);
	root["format"] = "DX12Scene";
	root["version"] = SceneFile::kVersion;
	root["createDefaults"] = true;
	root["entities"] = Json::Value(Json::arrayValue);
	Json::StreamWriterBuilder writer;
	writer["indentation"] = "  ";
	string error;
	if (!WriteAssetFile(path, Json::writeString(writer, root), error))
	{
		AddLog("シーン作成失敗: %s", error.c_str());
		return;
	}
	m_SelectedAssetPath = path;
	m_AssetBrowserRescan = true;
	AddLog("シーン作成: %s（ダブルクリックで開く）", PathToUtf8(path).c_str());
}

bool ImGuiManager::IsInsideAssetRoot(const filesystem::path& path)
{
	const filesystem::path root = CanonicalOf(m_AssetRoot);
	const filesystem::path target = CanonicalOf(path);
	const filesystem::path relative = target.lexically_relative(root);
	return !relative.empty() && relative != "." && *relative.begin() != "..";
}

void ImGuiManager::BeginAssetDragSource(const filesystem::path& path, bool isDirectory)
{
	if (!ImGui::BeginDragDropSource())
	{
		return;
	}
	const string label = PathToUtf8(path.filename());
	if (m_SelectedAssetKeys.size() > 1 && IsAssetSelected(path))
	{
		// 複数選択中の項目を掴んだ：選択全体をまとめて運ぶ（フォルダへの移動専用）
		m_DraggedAssetPaths = GetSelectedAssetPaths();
		const int count = static_cast<int>(m_DraggedAssetPaths.size());
		ImGui::SetDragDropPayload(kAssetMultiPayload, &count, sizeof(count));
		ImGui::Text("%d 個の項目", count);
	}
	else if (isDirectory)
	{
		const wstring wide = path.wstring();
		ImGui::SetDragDropPayload(kAssetDirectoryPayload, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
		ImGui::Text("フォルダ: %s", label.c_str());
	}
	else
	{
		const string relative = MakeRelativeAssetPath(path);
		const auto type = AssetRegistry::Classify(path);
		bool sent = false;
		if (type == AssetType::Material || type == AssetType::Texture)
		{
			string error;
			const AssetDragPayload payload{ MaterialManager::Registry().Register(path, type, error), type };
			if (payload.Handle != 0)
			{
				ImGui::SetDragDropPayload(AssetPayloadType, &payload, sizeof(payload));
				sent = true;
			}
		}
		if (!sent)
		{
			// シーンへの配置（モデル）とフォルダ移動の両方が受け取る
			ImGui::SetDragDropPayload("ASSET_PATH", relative.c_str(), relative.size() + 1);
		}
		ImGui::TextUnformatted(label.c_str());
	}
	ImGui::TextDisabled("フォルダにドロップで移動");
	ImGui::EndDragDropSource();
}

void ImGuiManager::AcceptAssetMoveDrop(const filesystem::path& destinationDirectory)
{
	if (ImGui::AcceptDragDropPayload(kAssetMultiPayload))
	{
		const vector<filesystem::path> sources = m_DraggedAssetPaths;
		m_DraggedAssetPaths.clear();
		vector<filesystem::path> moved;
		for (const auto& item : sources)
		{
			if (MoveAssetPath(item, destinationDirectory)) moved.push_back(m_LastRelocatedAssetPath);
		}
		// 移動先フォルダを表示中なら、移動した項目を選択したままにする
		if (!moved.empty())
		{
			ClearAssetSelection();
			if (CanonicalOf(destinationDirectory) == CanonicalOf(m_CurrentAssetDirectory))
			{
				for (const auto& item : moved) AddAssetToSelection(item);
				m_SelectedAssetPath = moved.back();
				m_AssetSelectionAnchor = moved.front();
			}
		}
		return;
	}
	filesystem::path source;
	if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(AssetPayloadType))
	{
		if (payload->DataSize == sizeof(AssetDragPayload))
		{
			AssetDragPayload asset{};
			memcpy(&asset, payload->Data, sizeof(asset));
			source = MaterialManager::Registry().Resolve(asset.Handle);
		}
	}
	else if (const ImGuiPayload* pathPayload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
	{
		const char* text = static_cast<const char*>(pathPayload->Data);
		if (text && text[0] != '\0') source = filesystem::path(text);
	}
	else if (const ImGuiPayload* directoryPayload = ImGui::AcceptDragDropPayload(kAssetDirectoryPayload))
	{
		const wchar_t* text = static_cast<const wchar_t*>(directoryPayload->Data);
		if (text && text[0] != L'\0') source = filesystem::path(text);
	}
	if (!source.empty())
	{
		MoveAssetPath(source, destinationDirectory);
	}
}

bool ImGuiManager::MoveAssetPath(const filesystem::path& source, const filesystem::path& destinationDirectory)
{
	error_code ec;
	const filesystem::path root = CanonicalOf(m_AssetRoot);
	const filesystem::path from = CanonicalOf(source);
	const filesystem::path toDirectory = CanonicalOf(destinationDirectory);

	if (!filesystem::exists(from, ec) || !IsInsideAssetRoot(from))
	{
		AddLog("移動中止（asset外または存在しない）: %s", PathToUtf8(source).c_str());
		return false;
	}
	if (!filesystem::is_directory(toDirectory, ec) || !(toDirectory == root || IsInsideAssetRoot(toDirectory)))
	{
		AddLog("移動中止（移動先が不正）: %s", PathToUtf8(destinationDirectory).c_str());
		return false;
	}
	if (from.parent_path() == toDirectory)
	{
		return false; // 同じフォルダ：何もしない
	}
	const bool isDirectory = filesystem::is_directory(from, ec);
	if (isDirectory && IsSameOrInside(toDirectory, from))
	{
		AddLog("フォルダを自分自身の中へは移動できません: %s", PathToUtf8(from.filename()).c_str());
		return false;
	}

	// 同名があれば "名前 1" のように連番を付ける
	filesystem::path target = toDirectory / from.filename();
	if (filesystem::exists(target, ec) || filesystem::exists(filesystem::path(target.wstring() + L".meta"), ec))
	{
		target = isDirectory
			? MakeUniqueAssetPath(toDirectory, from.filename(), {})
			: MakeUniqueAssetPath(toDirectory, from.stem(), from.extension());
	}
	return RelocateAssetPath(from, target, isDirectory, "移動");
}

// from（正規化済み）→ target へ実際に動かし、.meta・ブラウザ状態・シーン内参照を付け替える。移動とリネームで共用
bool ImGuiManager::RelocateAssetPath(const filesystem::path& from, const filesystem::path& target, bool isDirectory, const char* verb)
{
	error_code ec;
	// 大文字小文字だけの変更（Windows では同じファイル扱い）にも対応するため、一時名を経由する
	const bool caseOnly = filesystem::equivalent(from, target, ec) && from.filename() != target.filename();
	ec.clear();
	if (caseOnly)
	{
		filesystem::path temporary = from;
		temporary += L".renaming";
		filesystem::rename(from, temporary, ec);
		if (!ec) filesystem::rename(temporary, target, ec);
		if (ec)
		{
			error_code rollback;
			filesystem::rename(temporary, from, rollback);
		}
	}
	else
	{
		filesystem::rename(from, target, ec);
	}
	if (ec)
	{
		AddLog("%s失敗: %s (%s)", verb, PathToUtf8(from).c_str(), ec.message().c_str());
		return false;
	}
	// ファイルの .meta（GUID）も一緒に動かす。GUID が変わらないので、マテリアル・テクスチャの参照は切れない
	if (!isDirectory)
	{
		const filesystem::path fromMeta(from.wstring() + L".meta");
		if (filesystem::exists(fromMeta, ec))
		{
			filesystem::rename(fromMeta, filesystem::path(target.wstring() + L".meta"), ec);
			if (ec)
			{
				error_code rollback;
				filesystem::rename(target, from, rollback);
				AddLog("%s失敗（.meta）: %s (%s)", verb, PathToUtf8(from).c_str(), ec.message().c_str());
				return false;
			}
		}
	}

	// ブラウザの状態を付け替える（パスは "asset/..." の作業フォルダ相対で保持している）
	const filesystem::path oldRelative = ToWorkingRelative(from);
	const filesystem::path newRelative = ToWorkingRelative(target);
	const string oldText = oldRelative.generic_string();
	const string newText = newRelative.generic_string();
	auto remap = [&](filesystem::path& value)
	{
		if (value.empty()) return false;
		const filesystem::path canonical = CanonicalOf(value);
		if (canonical == from || (isDirectory && IsSameOrInside(canonical, from)))
		{
			value = newRelative / ToWorkingRelative(canonical).lexically_relative(oldRelative);
			value = value.lexically_normal();
			return true;
		}
		return false;
	};
	// 移動元自身は存在しないので CanonicalOf では比較できない場合がある → 文字列でも判定
	auto remapByText = [&](filesystem::path& value)
	{
		string text = value.generic_string();
		if (RemapPathString(text, oldText, newText))
		{
			value = filesystem::path(text);
			return true;
		}
		return remap(value);
	};
	remapByText(m_SelectedAssetPath);
	remapByText(m_CurrentAssetDirectory);
	if (remapByText(m_ScenePath))
	{
		SaveProjectSettings(); // cfg の scene= を新しい場所に
	}

	// シーン内の文字列パス参照（モデル・テクスチャ）を付け替える。次回保存で新しいパスが書かれる
	int remappedReferences = 0;
	for (EntityID entity : CollectSceneEntities())
	{
		if (ComponentManager::HasComponent<StaticModelComponent>(entity))
		{
			auto& model = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
			remappedReferences += RemapPathString(model.ModelPath, oldText, newText) ? 1 : 0;
		}
		if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
		{
			auto& model = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
			remappedReferences += RemapPathString(model.ModelPath, oldText, newText) ? 1 : 0;
			for (auto& animationPath : model.AnimationPaths)
			{
				remappedReferences += RemapPathString(animationPath, oldText, newText) ? 1 : 0;
			}
		}
		if (ComponentManager::HasComponent<MaterialComponent>(entity))
		{
			auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
			remappedReferences += RemapPathString(material.TexturePath, oldText, newText) ? 1 : 0;
			remappedReferences += RemapPathString(material.NormalMapPath, oldText, newText) ? 1 : 0;
		}
	}

	MaterialManager::Refresh(); // .meta から GUID → 新しいパスを再登録
	m_AssetBrowserRescan = true;
	m_LastRelocatedAssetPath = newRelative;
	// 複数選択・クリップボード内のパスも付け替える
	{
		vector<filesystem::path> selection = m_SelectedAssetPaths;
		const filesystem::path focus = m_SelectedAssetPath;
		for (auto& item : selection) remapByText(item);
		m_SelectedAssetKeys.clear();
		m_SelectedAssetPaths.clear();
		for (const auto& item : selection) AddAssetToSelection(item);
		if (!focus.empty()) AddAssetToSelection(m_SelectedAssetPath);
		for (auto& item : m_AssetClipboard) remapByText(item);
		remapByText(m_AssetSelectionAnchor);
	}
	if (remappedReferences > 0)
	{
		AddLog("%s: %s → %s（シーン内参照 %d 件を更新）", verb, oldText.c_str(), newText.c_str(), remappedReferences);
	}
	else
	{
		AddLog("%s: %s → %s", verb, oldText.c_str(), newText.c_str());
	}
	return true;
}


// ============================================================================
// アセットブラウザ：F2 / 右クリック「名前を変更」によるその場リネーム
//  ・ファイルは拡張子を除いた名前だけを編集（拡張子を変えると種類が変わり参照が切れるため）
//  ・Enter / 他をクリック（フォーカス外れ）で確定、Esc で取り消し
//  ・.meta（GUID）も一緒にリネームするので、マテリアル・テクスチャ参照は切れない
// ============================================================================
void ImGuiManager::BeginAssetRename(const filesystem::path& path)
{
	error_code ec;
	if (path.empty() || !filesystem::exists(path, ec) || !IsInsideAssetRoot(path))
	{
		return;
	}
	const bool isDirectory = filesystem::is_directory(path, ec);
	const string name = PathToUtf8(isDirectory ? path.filename() : path.stem());
	strncpy_s(m_AssetRenameBuffer, name.c_str(), _TRUNCATE);
	m_RenamingAssetPath = path;
	m_SelectedAssetPath = path;
	m_AssetRenameFrames = 0;
}

void ImGuiManager::DrawAssetRenameField(const filesystem::path& path, bool isDirectory, const ImVec2& position, float width)
{
	const ImVec2 restore = ImGui::GetCursorScreenPos();
	ImGui::SetCursorScreenPos(ImVec2(position.x, position.y - 2.0f));
	ImGui::SetNextItemWidth(width);
	if (m_AssetRenameFrames == 0)
	{
		ImGui::SetKeyboardFocusHere();
	}
	const bool entered = ImGui::InputText("##AssetRename", m_AssetRenameBuffer, IM_ARRAYSIZE(m_AssetRenameBuffer),
		ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
	const bool active = ImGui::IsItemActive();
	const bool deactivated = ImGui::IsItemDeactivated();
	const bool cancelled = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
	ImGui::SetCursorScreenPos(restore);
	++m_AssetRenameFrames;

	if (cancelled)
	{
		m_RenamingAssetPath.clear();
		return;
	}
	if (entered || deactivated)
	{
		const string newName = m_AssetRenameBuffer;
		const filesystem::path source = path;
		m_RenamingAssetPath.clear();
		RenameAssetPath(source, newName, isDirectory);
		return;
	}
	// フォーカスを取れなかった（別ウィンドウのクリック等）場合は取り消す
	if (!active && m_AssetRenameFrames > 2)
	{
		m_RenamingAssetPath.clear();
	}
}

bool ImGuiManager::RenameAssetPath(const filesystem::path& source, const string& newNameUtf8, bool isDirectory)
{
	// 前後の空白を除く
	const size_t first = newNameUtf8.find_first_not_of(" \t");
	const size_t last = newNameUtf8.find_last_not_of(" \t.");
	if (first == string::npos || last == string::npos || last < first)
	{
		AddLog("名前が空のためリネームを取り消しました");
		return false;
	}
	const string trimmed = newNameUtf8.substr(first, last - first + 1);
	if (trimmed.find_first_of("\\/:*?\"<>|") != string::npos)
	{
		AddLog("名前に使えない文字が含まれています（\\ / : * ? \" < > |）: %s", trimmed.c_str());
		return false;
	}
	string upper = trimmed;
	transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(toupper(c)); });
	for (const char* reserved : { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "LPT1", "LPT2", "LPT3" })
	{
		if (upper == reserved)
		{
			AddLog("Windows の予約名は使えません: %s", trimmed.c_str());
			return false;
		}
	}

	error_code ec;
	const filesystem::path from = CanonicalOf(source);
	if (!filesystem::exists(from, ec) || !IsInsideAssetRoot(from))
	{
		AddLog("リネーム中止（asset外または存在しない）: %s", PathToUtf8(source).c_str());
		return false;
	}
	filesystem::path newFileName = Utf8ToPath(trimmed.c_str());
	if (!isDirectory)
	{
		newFileName += from.extension(); // 拡張子は維持
	}
	if (newFileName == from.filename())
	{
		return false; // 変更なし
	}
	const filesystem::path target = from.parent_path() / newFileName;
	const bool caseOnly = filesystem::equivalent(from, target, ec);
	ec.clear();
	if (!caseOnly && (filesystem::exists(target, ec) || filesystem::exists(filesystem::path(target.wstring() + L".meta"), ec)))
	{
		AddLog("同じ名前のファイル / フォルダが既にあります: %s", PathToUtf8(newFileName).c_str());
		return false;
	}
	return RelocateAssetPath(from, target, isDirectory, "リネーム");
}

// ============================================================================
// アセットブラウザ：複数選択とクリップボード（Windows エクスプローラー準拠）
//  クリック            : 単一選択          Ctrl+クリック : 追加 / 解除
//  Shift+クリック      : 範囲選択          Ctrl+Shift+クリック : 範囲を追加
//  背景ドラッグ        : 矩形選択（Ctrl で追加）
//  Ctrl+A すべて選択 / Esc 解除 / 矢印・Home・End 移動（Shift で範囲）/ Ctrl+Space 切替
//  Ctrl+C コピー / Ctrl+X 切り取り / Ctrl+V 貼り付け / Ctrl+D 複製 / Delete 削除 / F2 名前変更
//  Enter 開く / Backspace・Alt+↑ 上のフォルダ
// ============================================================================
wstring ImGuiManager::AssetSelectionKey(const filesystem::path& path)
{
	wstring key = path.lexically_normal().generic_wstring();
	// Windows のファイル名は大文字小文字を区別しない
	transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
	while (key.size() > 1 && key.back() == L'/') key.pop_back();
	return key;
}

bool ImGuiManager::IsAssetSelected(const filesystem::path& path)
{
	return !m_SelectedAssetKeys.empty() && m_SelectedAssetKeys.contains(AssetSelectionKey(path));
}

void ImGuiManager::ClearAssetSelection()
{
	m_SelectedAssetPaths.clear();
	m_SelectedAssetKeys.clear();
	m_SelectedAssetPath.clear();
}

void ImGuiManager::SelectAssetOnly(const filesystem::path& path)
{
	ClearAssetSelection();
	AddAssetToSelection(path);
	m_SelectedAssetPath = path;
	m_AssetSelectionAnchor = path;
}

void ImGuiManager::AddAssetToSelection(const filesystem::path& path)
{
	if (path.empty()) return;
	if (m_SelectedAssetKeys.insert(AssetSelectionKey(path)).second)
	{
		m_SelectedAssetPaths.push_back(path);
	}
	if (m_SelectedAssetPath.empty()) m_SelectedAssetPath = path;
}

void ImGuiManager::RemoveAssetFromSelection(const filesystem::path& path)
{
	const wstring key = AssetSelectionKey(path);
	if (m_SelectedAssetKeys.erase(key) == 0) return;
	m_SelectedAssetPaths.erase(remove_if(m_SelectedAssetPaths.begin(), m_SelectedAssetPaths.end(),
		[&](const filesystem::path& item) { return AssetSelectionKey(item) == key; }), m_SelectedAssetPaths.end());
	if (AssetSelectionKey(m_SelectedAssetPath) == key)
	{
		m_SelectedAssetPath = m_SelectedAssetPaths.empty() ? filesystem::path{} : m_SelectedAssetPaths.back();
	}
}

// 他の処理が m_SelectedAssetPath を直接書き換えた場合（作成・フォルダ移動など）に複数選択を合わせる
void ImGuiManager::SyncAssetSelection()
{
	if (m_SelectedAssetPath.empty())
	{
		if (!m_SelectedAssetKeys.empty())
		{
			m_SelectedAssetPaths.clear();
			m_SelectedAssetKeys.clear();
		}
		return;
	}
	if (!IsAssetSelected(m_SelectedAssetPath))
	{
		const filesystem::path focus = m_SelectedAssetPath;
		SelectAssetOnly(focus);
	}
}

vector<filesystem::path> ImGuiManager::GetSelectedAssetPaths()
{
	vector<filesystem::path> result;
	result.reserve(m_SelectedAssetPaths.size());
	error_code ec;
	for (const auto& path : m_SelectedAssetPaths)
	{
		if (filesystem::exists(path, ec)) result.push_back(path);
	}
	return result;
}

void ImGuiManager::OpenAssetItem(const filesystem::path& path)
{
	error_code ec;
	if (filesystem::is_directory(path, ec))
	{
		m_CurrentAssetDirectory = path;
		ClearAssetSelection();
		return;
	}
	if (IsSceneFile(path))
	{
		RequestOpenScene(path); // 未保存なら確認ポップアップ
		return;
	}
	if (AssetRegistry::Classify(path) == AssetType::Material)
	{
		string error;
		OpenMaterialAsset(MaterialManager::Registry().Register(path, AssetType::Material, error));
		return;
	}
	if (IsTextureFile(path))
	{
		const string relative = MakeRelativeAssetPath(path);
		TextureManager::LoadTexture(relative.c_str());
		if (m_SelectedEntity != g_kINVALID_ENTITY && Registry::IsAlive(m_SelectedEntity))
		{
			MaterialSystem::SetTexture(m_SelectedEntity, relative.c_str());
		}
	}
}

void ImGuiManager::CopyAssetsToClipboard(bool cut)
{
	m_AssetClipboard = GetSelectedAssetPaths();
	m_AssetClipboardCut = cut && !m_AssetClipboard.empty();
	if (!m_AssetClipboard.empty())
	{
		AddLog("%s: %d 個の項目", cut ? "切り取り" : "コピー", static_cast<int>(m_AssetClipboard.size()));
	}
}

// ファイル / フォルダを destinationDirectory にコピーする。.meta はコピーしない（GUID の重複を防ぐ。新しい GUID が振られる）
bool ImGuiManager::CopyAssetPath(const filesystem::path& source, const filesystem::path& destinationDirectory, filesystem::path* outTarget)
{
	error_code ec;
	const filesystem::path from = CanonicalOf(source);
	const filesystem::path toDirectory = CanonicalOf(destinationDirectory);
	if (!filesystem::exists(from, ec) || !IsInsideAssetRoot(from))
	{
		AddLog("コピー中止（asset外または存在しない）: %s", PathToUtf8(source).c_str());
		return false;
	}
	if (!filesystem::is_directory(toDirectory, ec) || !(toDirectory == CanonicalOf(m_AssetRoot) || IsInsideAssetRoot(toDirectory)))
	{
		AddLog("コピー中止（コピー先が不正）: %s", PathToUtf8(destinationDirectory).c_str());
		return false;
	}
	const bool isDirectory = filesystem::is_directory(from, ec);
	if (isDirectory && IsSameOrInside(toDirectory, from))
	{
		AddLog("フォルダを自分自身の中へはコピーできません: %s", PathToUtf8(from.filename()).c_str());
		return false;
	}
	// 同じ場所なら "名前 1"（Unity の複製と同じ）
	const filesystem::path target = isDirectory
		? MakeUniqueAssetPath(toDirectory, from.filename(), {})
		: MakeUniqueAssetPath(toDirectory, from.stem(), from.extension());

	if (!isDirectory)
	{
		filesystem::copy_file(from, target, filesystem::copy_options::none, ec);
	}
	else
	{
		filesystem::create_directories(target, ec);
		for (auto iterator = filesystem::recursive_directory_iterator(from, ec);
			!ec && iterator != filesystem::recursive_directory_iterator(); iterator.increment(ec))
		{
			const filesystem::path relative = iterator->path().lexically_relative(from);
			const filesystem::path destination = target / relative;
			error_code itemError;
			if (iterator->is_directory(itemError))
			{
				filesystem::create_directories(destination, itemError);
				continue;
			}
			const auto extension = iterator->path().extension();
			if (extension == ".meta" || extension == ".tmp") continue;
			filesystem::copy_file(iterator->path(), destination, filesystem::copy_options::skip_existing, itemError);
			if (itemError)
			{
				AddLog("コピー失敗: %s (%s)", PathToUtf8(iterator->path()).c_str(), itemError.message().c_str());
			}
		}
	}
	if (ec)
	{
		AddLog("コピー失敗: %s (%s)", PathToUtf8(from).c_str(), ec.message().c_str());
		return false;
	}
	if (outTarget) *outTarget = ToWorkingRelative(target);
	return true;
}

void ImGuiManager::PasteAssetClipboard(const filesystem::path& destinationDirectory)
{
	if (m_AssetClipboard.empty())
	{
		return;
	}
	const vector<filesystem::path> sources = m_AssetClipboard;
	vector<filesystem::path> created;
	int failed = 0;
	for (const auto& item : sources)
	{
		if (m_AssetClipboardCut)
		{
			if (MoveAssetPath(item, destinationDirectory)) created.push_back(m_LastRelocatedAssetPath);
			else ++failed;
		}
		else
		{
			filesystem::path target;
			if (CopyAssetPath(item, destinationDirectory, &target)) created.push_back(target);
			else ++failed;
		}
	}
	if (m_AssetClipboardCut)
	{
		// 切り取りは一度貼り付けたら終わり（エクスプローラーと同じ）
		m_AssetClipboard.clear();
		m_AssetClipboardCut = false;
	}
	MaterialManager::Refresh(); // コピーしたマテリアルに新しい GUID を振る
	m_AssetBrowserRescan = true;
	if (!created.empty() && CanonicalOf(destinationDirectory) == CanonicalOf(m_CurrentAssetDirectory))
	{
		ClearAssetSelection();
		for (const auto& item : created) AddAssetToSelection(item);
		m_SelectedAssetPath = created.back();
		m_AssetSelectionAnchor = created.front();
	}
	AddLog("貼り付け: %d 個%s", static_cast<int>(created.size()), failed > 0 ? "（一部失敗。ログを確認）" : "");
}

void ImGuiManager::DuplicateSelectedAssets()
{
	const vector<filesystem::path> sources = GetSelectedAssetPaths();
	if (sources.empty())
	{
		return;
	}
	vector<filesystem::path> created;
	for (const auto& item : sources)
	{
		filesystem::path target;
		if (CopyAssetPath(item, item.parent_path(), &target)) created.push_back(target);
	}
	MaterialManager::Refresh();
	m_AssetBrowserRescan = true;
	if (!created.empty())
	{
		ClearAssetSelection();
		for (const auto& item : created) AddAssetToSelection(item);
		m_SelectedAssetPath = created.back();
		m_AssetSelectionAnchor = created.front();
		AddLog("複製: %d 個", static_cast<int>(created.size()));
	}
}
