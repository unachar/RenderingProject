#include "pch.h"
#include "materialmanager.h"
#include "assetthumbnailmanager.h"
#include "thumbnailvalidation.h"
#include "imguimanager.h"
#include "editorwidgets.h"
#include "materialsystem.h"
#include "modelmanager.h"
#include "texturemanager.h"
#include "world.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <DirectXCollision.h>
#include <ImGuizmo.h>



using namespace EditorWidgets;

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
		ImGui::End();
		return;
	}

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
	ImGui::TextWrapped("%s", m_CurrentAssetDirectory.generic_string().c_str());

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
			filesystem::create_directories(m_CurrentAssetDirectory / folderName, ec);
		}
		ImGui::Separator();
		ImGui::InputText("ファイル名", fileName, IM_ARRAYSIZE(fileName));
        if (ImGui::Button("Material作成"))
        {
            filesystem::path materialPath = m_CurrentAssetDirectory / fileName;
            if (materialPath.extension() != ".material")
            {
                materialPath += ".material";
            }
            CreateAssetFile(materialPath);
        }

		if (ImGui::Button("ファイル作成") && fileName[0] != '\0')
		{
			const filesystem::path newPath = m_CurrentAssetDirectory / fileName;
			CreateAssetFile(newPath);
		}
		ImGui::EndPopup();
	}

    int thumbnailSize = static_cast<int>(AssetThumbnailManager::Settings().Size);
    ImGui::SetNextItemWidth(150);
    if (ImGui::SliderInt("サムネイルサイズ", &thumbnailSize, 32, 256)) AssetThumbnailManager::Settings().Size = static_cast<UINT>(thumbnailSize);
    ImGui::SameLine();
    if (ImGui::Button("サムネイル更新")) AssetThumbnailManager::Refresh();
    const float cellSize = static_cast<float>(thumbnailSize);
    const float rowHeight = cellSize + ImGui::GetTextLineHeightWithSpacing() + 12.0f;
    static ImGuiTextFilter assetFilter;
	static filesystem::path pendingDeletePath;
	bool deleteRequested = false;
	DrawSearchField("AssetSearch", "ファイル名で検索...", assetFilter);
	ImGui::BeginChild("AssetList", ImVec2(0, -ImGui::GetTextLineHeightWithSpacing()), true, ImGuiWindowFlags_HorizontalScrollbar);
	if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
		ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !m_SelectedAssetPath.empty())
	{
		pendingDeletePath = m_SelectedAssetPath;
		deleteRequested = true;
	}
	if (ImGui::BeginPopupContextWindow("AssetBackgroundContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
	{
		if (ImGui::MenuItem("フォルダ作成"))
		{
			filesystem::path path = m_CurrentAssetDirectory / folderName;
			filesystem::create_directories(path, ec);
			AddLog("フォルダ作成: %s", path.generic_string().c_str());
		}
		if (ImGui::MenuItem("空ファイル作成"))
		{
			CreateAssetFile(m_CurrentAssetDirectory / fileName);
		}
		if (ImGui::MenuItem("マテリアルファイル作成"))
		{
			CreateAssetFile(m_CurrentAssetDirectory / "NewMaterial.material");
		}
		ImGui::EndPopup();
	}
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
		{
			const char* src = static_cast<const char*>(payload->Data);
			if (src && src[0] != '\0')
			{
				ImportAssetFile(src);
			}
		}
		ImGui::EndDragDropTarget();
	}

    static vector<filesystem::directory_entry> entries;
    static unordered_map<string, AssetHandle> browserHandles;
    static filesystem::path listedDirectory;
    static string listedFilter;
    static double nextScan = 0;
    const string currentFilter = assetFilter.InputBuf;
    if (listedDirectory != m_CurrentAssetDirectory || listedFilter != currentFilter || ImGui::GetTime() >= nextScan)
    {
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

			bool selected = false;
			if (!m_SelectedAssetPath.empty())
			{
				selected = filesystem::equivalent(path, m_SelectedAssetPath, ec);
				ec.clear();
			}

			ImGui::PushID(itemId.c_str());

            const ImVec2 cellPosition = ImGui::GetCursorScreenPos();
            ImVec2 cursorPos(cellPosition.x + (cellSize - 16.0f) * 0.5f, cellPosition.y + (cellSize - 16.0f) * 0.5f);
			ImVec2 iconSize(16, 16);
            const bool clicked = ImGui::Selectable("##AssetCell", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(cellSize, rowHeight));
            ThumbnailValidation::CaptureAssetCell(fileName.c_str(), cellPosition.x, cellPosition.y, selected);

			if (isDir)
			{
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				ImVec4 yellow(1.0f, 0.85f, 0.2f, 1.0f);
				drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + iconSize.x, cursorPos.y + iconSize.y), ImGui::ColorConvertFloat4ToU32(yellow), 2.0f);
				drawList->AddRect(ImVec2(cursorPos.x + 3, cursorPos.y + 2), ImVec2(cursorPos.x + 11, cursorPos.y + 8), ImGui::ColorConvertFloat4ToU32(ImVec4(0.8f, 0.65f, 0.1f, 1.0f)), 1.0f);
				drawList->AddRectFilled(ImVec2(cursorPos.x + 3, cursorPos.y + 7), ImVec2(cursorPos.x + 13, cursorPos.y + 14), ImGui::ColorConvertFloat4ToU32(yellow), 2.0f);
			}
			else
			{
				const string ext = path.extension().generic_string();
				ImVec4 fileColor(0.6f, 0.8f, 1.0f, 1.0f);
				if (ext == ".png" || ext == ".jpg" || ext == ".bmp" || ext == ".tga")
					fileColor = ImVec4(0.4f, 0.9f, 0.5f, 1.0f);
				else if (ext == ".fbx" || ext == ".obj")
					fileColor = ImVec4(1.0f, 0.6f, 0.3f, 1.0f);
				else if (ext == ".material")
                    fileColor = ImVec4(0.2f, 0.8f, 0.95f, 1.0f);
				else if (ext == ".hlsl" || ext == ".slang")
					fileColor = ImVec4(0.8f, 0.4f, 1.0f, 1.0f);

                ImDrawList* drawList = ImGui::GetWindowDrawList();
                if (ext == ".material")
                {
                    drawList->AddCircleFilled(ImVec2(cursorPos.x + 8, cursorPos.y + 8), 7,
                        ImGui::ColorConvertFloat4ToU32(fileColor));
                    drawList->AddCircleFilled(ImVec2(cursorPos.x + 5, cursorPos.y + 5), 2,
                        IM_COL32(215, 245, 255, 220));
                }
                else
                {
				drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + iconSize.x - 2, cursorPos.y + iconSize.y), ImGui::ColorConvertFloat4ToU32(fileColor), 2.0f);
				drawList->AddLine(ImVec2(cursorPos.x + 6, cursorPos.y + 2), ImVec2(cursorPos.x + 12, cursorPos.y + 2), ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.6f)), 1.0f);
				drawList->AddLine(ImVec2(cursorPos.x + 6, cursorPos.y + 6), ImVec2(cursorPos.x + 12, cursorPos.y + 6), ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.6f)), 1.0f);
				drawList->AddLine(ImVec2(cursorPos.x + 6, cursorPos.y + 10), ImVec2(cursorPos.x + 10, cursorPos.y + 10), ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.6f)), 1.0f);
                }
			}

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
            else if (thumbnail.State == ThumbnailState::Failed)
                ImGui::GetWindowDrawList()->AddText(ImVec2(cellPosition.x + cellSize - 12, cellPosition.y), IM_COL32(255, 110, 90, 255), "!");
            const ImVec2 labelPosition(cellPosition.x, cellPosition.y + cellSize + 4.0f);
            ImGui::GetWindowDrawList()->PushClipRect(labelPosition, ImVec2(cellPosition.x + cellSize, labelPosition.y + ImGui::GetTextLineHeight()), true);
            ImGui::GetWindowDrawList()->AddText(labelPosition, ImGui::GetColorU32(ImGuiCol_Text), displayName.c_str());
            ImGui::GetWindowDrawList()->PopClipRect();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", displayName.c_str());
            if (clicked)
			{
				m_SelectedAssetPath = path;
				if (isDir && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				{
					m_CurrentAssetDirectory = path;
					m_SelectedAssetPath.clear();
				}
				else if (AssetRegistry::Classify(path) == AssetType::Material && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    string error;
                    const auto handle = MaterialManager::Registry().Register(path, AssetType::Material, error);
                    OpenMaterialAsset(handle);
                }
                else if (IsTextureFile(path) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				{
					const string relative = MakeRelativeAssetPath(path);
					TextureManager::LoadTexture(relative.c_str());
					if (m_SelectedEntity != g_kINVALID_ENTITY && Registry::IsAlive(m_SelectedEntity))
					{
						MaterialSystem::SetTexture(m_SelectedEntity, relative.c_str());
					}
				}
			}

			if (ImGui::BeginPopupContextItem("AssetItemContext"))
			{
				m_SelectedAssetPath = path;
				ImGui::TextUnformatted(path.filename().generic_string().c_str());
				ImGui::Separator();
				if (isDir && ImGui::MenuItem("開く"))
				{
					m_CurrentAssetDirectory = path;
					m_SelectedAssetPath.clear();
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
                if (ImGui::MenuItem("削除"))
                {
					pendingDeletePath = path;
					deleteRequested = true;
				}
				ImGui::EndPopup();
			}

			if (!isDir && ImGui::BeginDragDropSource())
			{
				const string relative = MakeRelativeAssetPath(path);
				const auto type = AssetRegistry::Classify(path);
                if (type == AssetType::Material || type == AssetType::Texture)
                {
                    string error;
                    const AssetDragPayload payload{MaterialManager::Registry().Register(path, type, error), type};
                    if (payload.Handle != 0)
                    {
                        ImGui::SetDragDropPayload(AssetPayloadType, &payload, sizeof(payload));
                    }
                }
                else
                {
                    ImGui::SetDragDropPayload("ASSET_PATH", relative.c_str(), relative.size() + 1);
                }
				ImGui::TextUnformatted(relative.c_str());
				ImGui::EndDragDropSource();
			}

            ImGui::PopID();
            }
        }
    }
    ImGui::EndTable();
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
		ImGui::TextWrapped("%s", pendingDeletePath.generic_string().c_str());
		ImGui::TextUnformatted("フォルダ内のファイルも削除されます。この操作は元に戻せません。");
		ImGui::Separator();
		if (ImGui::Button("削除する"))
		{
			DeleteAssetPath(pendingDeletePath);
            filesystem::remove(pendingDeletePath.wstring() + L".meta", ec);
            MaterialManager::Refresh();
			m_SelectedAssetPath.clear();
			pendingDeletePath.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("キャンセル") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			pendingDeletePath.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	ImGui::TextDisabled("%d 件 / ダブルクリック: 開く・適用 / ドラッグ: 配置", static_cast<int>(entries.size()));
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
	return ext == ".fbx" || ext == ".obj" || ext == ".vrm" || ext == ".pmx";
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
