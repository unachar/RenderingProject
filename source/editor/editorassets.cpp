#include "pch.h"
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
	ImGui::SetNextWindowSize(ImVec2(720.0f, 260.0f), ImGuiCond_FirstUseEver);
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
	if (ImGui::Button("ルート"))
	{
		m_CurrentAssetDirectory = m_AssetRoot;
		m_SelectedAssetPath.clear();
	}
	ImGui::SameLine();
	if (ImGui::Button("+ 作成")) ImGui::OpenPopup("CreateAssetPopup");
	ImGui::TextWrapped("%s", m_CurrentAssetDirectory.generic_string().c_str());

	static char folderName[128] = "NewFolder";
	static char fileName[128] = "NewMaterial.txt";
	if (ImGui::BeginPopup("CreateAssetPopup"))
	{
		ImGui::InputText("フォルダ名", folderName, IM_ARRAYSIZE(folderName));
		if (ImGui::Button("フォルダ作成") && folderName[0] != '\0')
		{
			filesystem::create_directories(m_CurrentAssetDirectory / folderName, ec);
		}
		ImGui::Separator();
		ImGui::InputText("ファイル名", fileName, IM_ARRAYSIZE(fileName));
		if (ImGui::Button("ファイル作成") && fileName[0] != '\0')
		{
			const filesystem::path newPath = m_CurrentAssetDirectory / fileName;
			CreateAssetFile(newPath);
		}
		ImGui::EndPopup();
	}

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

	vector<filesystem::directory_entry> entries;
	for (const auto& entry : filesystem::directory_iterator(m_CurrentAssetDirectory, ec))
	{
		if (assetFilter.PassFilter(entry.path().filename().generic_string().c_str())) entries.push_back(entry);
	}
	if (ec) ImGui::TextWrapped("フォルダを読み込めません: %s", ec.message().c_str());
	else if (entries.empty()) ImGui::TextWrapped("表示するアセットがありません。検索条件を変更するか、ファイルをドロップして取り込んでください。");
	sort(entries.begin(), entries.end(), [](const auto& a, const auto& b)
		{
			if (a.is_directory() != b.is_directory())
			{
				return a.is_directory() > b.is_directory();
			}
			return a.path().filename().generic_string() < b.path().filename().generic_string();
		});

	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(entries.size()));
	while (clipper.Step())
	{
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
		{
			const filesystem::path path = entries[i].path();
			const bool isDir = entries[i].is_directory();
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

			ImVec2 cursorPos = ImGui::GetCursorScreenPos();
			ImVec2 iconSize(16, 16);

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
				else if (ext == ".hlsl" || ext == ".slang")
					fileColor = ImVec4(0.8f, 0.4f, 1.0f, 1.0f);

				ImDrawList* drawList = ImGui::GetWindowDrawList();
				drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + iconSize.x - 2, cursorPos.y + iconSize.y), ImGui::ColorConvertFloat4ToU32(fileColor), 2.0f);
				drawList->AddLine(ImVec2(cursorPos.x + 6, cursorPos.y + 2), ImVec2(cursorPos.x + 12, cursorPos.y + 2), ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.6f)), 1.0f);
				drawList->AddLine(ImVec2(cursorPos.x + 6, cursorPos.y + 6), ImVec2(cursorPos.x + 12, cursorPos.y + 6), ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.6f)), 1.0f);
				drawList->AddLine(ImVec2(cursorPos.x + 6, cursorPos.y + 10), ImVec2(cursorPos.x + 10, cursorPos.y + 10), ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.6f)), 1.0f);
			}

			ImGui::Dummy(iconSize);
			ImGui::SameLine(0.0f, 6.0f);

			if (ImGui::Selectable(displayName.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
			{
				m_SelectedAssetPath = path;
				if (isDir && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				{
					m_CurrentAssetDirectory = path;
					m_SelectedAssetPath.clear();
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
				ImGui::SetDragDropPayload("ASSET_PATH", relative.c_str(), relative.size() + 1);
				ImGui::TextUnformatted(relative.c_str());
				ImGui::EndDragDropSource();
			}

			ImGui::PopID();
		}
	}
	ImGui::EndChild();
	if (deleteRequested) ImGui::OpenPopup("アセットを削除");
	if (ImGui::BeginPopupModal("アセットを削除", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextWrapped("%s", pendingDeletePath.generic_string().c_str());
		ImGui::TextUnformatted("フォルダ内のファイルも削除されます。この操作は元に戻せません。");
		ImGui::Separator();
		if (ImGui::Button("削除する"))
		{
			DeleteAssetPath(pendingDeletePath);
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

	ofstream file(path);
	if (!file)
	{
		AddLog("作成失敗: %s", path.generic_string().c_str());
		return;
	}

	string ext = path.extension().generic_string();
	transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
	if (ext == ".material")
	{
		file << "{\n  \"type\": \"material\",\n  \"texture\": \"\"\n}\n";
	}
	else
	{
		file << "# 新規アセット\n";
	}
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
