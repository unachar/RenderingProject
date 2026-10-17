#pragma once
// ============================================================================
// PerformanceGovernor
//  目標フレームレート（既定120fps）を一定に保つため、GPU時間を毎フレーム監視して
//  描画品質を段階（Tier）で自動調整する。
//
//  ・可変レイトレーシング : サンプル数 / AO / 反射 / シャドウ距離 / レイトレ自体のON/OFF
//  ・可変GI              : Realtime → ScreenSpace → Off、GI距離・強度
//  ・ライト              : 画面内ライト数 / シャドウ付きライト数 / ボリューメトリック数
//  ・シャドウ            : 仮想シャドウのクリップマップ段数、コンタクトシャドウ段数
//  ・自作Nanite          : 許容誤差（ピクセル）を段階的に大きくする
//  ・雲                  : レイマーチ段数
//  ・解像度              : 最終手段。最下位段で長く予算超過した時だけ下げる（再確保のため頻度を抑える）
//
//  ユーザーがUIで設定した値を「基準値」として保持し、段階0では必ず基準値に戻す。
//  段階が上がっている間にユーザーが値を変更した場合は、その項目の基準値を更新する。
//  ヘッダーのみで完結（.vcxprojへの追加不要）。
// ============================================================================
#include "renderersettings.h"
#include "renderconfiguration.h"
#include "raytracing.h"
#include "cloud/cloudrenderer.h"
#include "virtualgeometry/virtualgeometrytraversal.h"
#include "world.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>

class PerformanceGovernor final
{
public:
	static constexpr int kTierCount = 6; // 0 = 最高品質, 5 = 最軽量

	struct Options
	{
		bool Enabled = true;
		bool AllowResolutionScaling = true;
		float BudgetHeadroom = 0.90f;   // 1フレーム時間のうちGPUに使ってよい割合
		float UpgradeMargin = 0.72f;    // 予算のこの割合を下回り続けたら品質を戻す
		float DowngradeDelay = 0.20f;   // 予算超過がこの秒数続いたら段階を下げる
		float UpgradeDelay = 2.0f;      // 余裕がこの秒数続いたら段階を戻す
		float ResolutionDelay = 3.0f;   // 最下位段でこの秒数超過したら解像度を下げる
		// false（既定）: ユーザーが ON にした機能（Realtime GI / RT反射 / RT AO / SSAO）は切り替えず、
		// サンプル数・距離・ライト数などのコストだけを下げる。
		// 以前は段階2で Realtime GI → SSGI、段階5で GI OFF に勝手に切り替えていたため、
		// 目標FPSに届かない環境では「Realtime GI を ON にしても OFF と同じ」状態になっていた。
		bool AllowFeatureToggles = false;
	};

	static Options& GetOptions() { return s_Options; }
	static int GetTier() { return s_Tier; }
	static float GetSmoothedGpuMs() { return s_SmoothedMs; }
	static float GetBudgetMs() { return s_BudgetMs; }

	static const char* GetTierName(int tier)
	{
		static const char* names[kTierCount] = { "最高", "高", "中", "低", "最低", "最軽量" };
		return names[std::clamp(tier, 0, kTierCount - 1)];
	}

	// Game::Run の先頭で毎フレーム呼ぶ。
	static void Update()
	{
		if (!s_BaselineCaptured)
		{
			CaptureBaseline();
		}
		else
		{
			AbsorbUserChanges();
		}

		if (!s_Options.Enabled)
		{
			if (s_Tier != 0 || s_ResolutionReduced)
			{
				s_Tier = 0;
				RestoreResolution();
				Apply();
			}
			return;
		}

		const float dt = std::clamp(World::GetRawDeltaTime(), 0.0f, 0.1f);
		const int targetFps = (std::max)(World::GetTargetFrameRate(), 30);
		s_BudgetMs = 1000.0f / static_cast<float>(targetFps) * s_Options.BudgetHeadroom;

		float costMs = 0.0f;
		if (RenderProfiler::HasLatest())
		{
			const auto& latest = RenderProfiler::GetLatest();
			if (latest.Serial != s_LastSerial)
			{
				s_LastSerial = latest.Serial;
				// GPU時間で判定する（品質段階はGPU負荷にしか効かないため）。GPU計測がない時だけCPU記録時間を使う。
				costMs = static_cast<float>(latest.GpuTimingValid ? latest.GpuRenderMs : latest.CpuRenderMs);
				s_SmoothedMs = s_SmoothedMs <= 0.0f ? costMs : s_SmoothedMs + (costMs - s_SmoothedMs) * 0.15f;
			}
		}
		if (s_SmoothedMs <= 0.0f)
		{
			return; // 計測値がまだない
		}

		if (s_SmoothedMs > s_BudgetMs)
		{
			s_OverTime += dt;
			s_UnderTime = 0.0f;
		}
		else if (s_SmoothedMs < s_BudgetMs * s_Options.UpgradeMargin)
		{
			s_UnderTime += dt;
			s_OverTime = 0.0f;
		}
		else
		{
			s_OverTime = (std::max)(0.0f, s_OverTime - dt);
			s_UnderTime = 0.0f;
		}

		// 大きく超過している時は待たずに下げる（スパイク対策で2フレーム分は様子を見る）。
		const bool severe = s_SmoothedMs > s_BudgetMs * 1.5f && s_OverTime > dt * 2.0f;
		if ((s_OverTime >= s_Options.DowngradeDelay || severe) && s_Tier < kTierCount - 1)
		{
			++s_Tier;
			s_OverTime = 0.0f;
			s_SmoothedMs = s_BudgetMs; // 変更直後の判定を落ち着かせる
			Apply();
		}
		else if (s_UnderTime >= s_Options.UpgradeDelay)
		{
			s_UnderTime = 0.0f;
			if (s_ResolutionReduced)
			{
				RestoreResolution();
			}
			else if (s_Tier > 0)
			{
				--s_Tier;
				Apply();
			}
		}

		// 最終手段：解像度。
		if (s_Tier == kTierCount - 1 && s_Options.AllowResolutionScaling && !s_ResolutionReduced)
		{
			s_ResolutionOverTime = s_SmoothedMs > s_BudgetMs ? s_ResolutionOverTime + dt : 0.0f;
			if (s_ResolutionOverTime >= s_Options.ResolutionDelay)
			{
				s_ResolutionOverTime = 0.0f;
				ReduceResolution();
			}
		}
		else
		{
			s_ResolutionOverTime = 0.0f;
		}
	}

	static void DrawImGui()
	{
		ImGui::Checkbox("自動品質調整（目標FPS維持）", &s_Options.Enabled);
		ImGui::Text("段階: %d (%s)   GPU: %.2f ms / 予算 %.2f ms", s_Tier, GetTierName(s_Tier), s_SmoothedMs, s_BudgetMs);
		ImGui::Checkbox("解像度も下げてよい", &s_Options.AllowResolutionScaling);
		ImGui::SliderFloat("GPU予算の割合", &s_Options.BudgetHeadroom, 0.6f, 1.0f, "%.2f");
		if (ImGui::Checkbox("機能の自動OFFを許可（GI / RT反射 / AO）", &s_Options.AllowFeatureToggles))
		{
			Apply();
		}
		if (s_Tier > 0)
		{
			const Baseline now = ReadCurrent();
			if (now.GiMode != s_Base.GiMode || now.RtReflections != s_Base.RtReflections ||
				now.RtAmbientOcclusion != s_Base.RtAmbientOcclusion)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "自動調整で GI / RT 反射 / AO の一部を一時的に OFF にしています");
			}
		}
		if (s_ResolutionReduced)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "解像度を一時的に下げています (%.2f)", RenderConfiguration::GetResolutionScale());
		}
	}

	// ---- 設定保存 ----
	// 自動調整で一時的に下げた値を cfg に保存すると、次回起動時にそれが「ユーザー設定」になり
	// 品質が下がったまま戻らなくなる。保存中だけユーザーの基準値に戻し、保存後に適用値へ戻す。
	// 解像度は再確保を伴うため一時的にも書き換えず、保存値だけ基準値にする。
	static float GetPersistentResolutionScale()
	{
		return s_ResolutionReduced ? s_Base.ResolutionScale : RenderConfiguration::GetResolutionScale();
	}

private:
	struct Baseline
	{
		bool RtEnabled = true;
		int RtSamples = 2;
		bool RtAmbientOcclusion = true;
		bool RtReflections = true;
		float RtShadowDistance = 500.0f;
		GlobalIlluminationMode GiMode = GlobalIlluminationMode::ScreenSpace;
		float GiDistance = 40.0f;
		bool Ssao = true;
		int ContactSteps = 12;
		int VirtualLevels = 4;
		int ScreenLights = 32;
		int ShadowLights = 4;
		int VolumetricLights = 5;
		float NaniteError = 1.0f;
		int CloudSteps = 64;
		float ResolutionScale = 1.0f;
	};

public:
	class ScopedPersistBaseline
	{
	public:
		ScopedPersistBaseline()
		{
			if (!s_BaselineCaptured) return;
			AbsorbUserChanges(); // 直前にUIで変えた値を先に基準値へ取り込む
			if (s_Tier == 0) return;
			m_Saved = s_Applied;
			Write(s_Base);
			m_Active = true;
		}
		~ScopedPersistBaseline()
		{
			if (m_Active) Write(m_Saved);
		}
		ScopedPersistBaseline(const ScopedPersistBaseline&) = delete;
		ScopedPersistBaseline& operator=(const ScopedPersistBaseline&) = delete;
	private:
		Baseline m_Saved{};
		bool m_Active = false;
	};

private:

	static inline Options s_Options{};
	static inline Baseline s_Base{};
	static inline Baseline s_Applied{};
	static inline bool s_BaselineCaptured = false;
	static inline int s_Tier = 0;
	static inline float s_SmoothedMs = 0.0f;
	static inline float s_BudgetMs = 1000.0f / 120.0f;
	static inline float s_OverTime = 0.0f;
	static inline float s_UnderTime = 0.0f;
	static inline float s_ResolutionOverTime = 0.0f;
	static inline bool s_ResolutionReduced = false;
	static inline uint64_t s_LastSerial = 0;

	static Baseline ReadCurrent()
	{
		Baseline b{};
		const auto& rt = Raytracing::GetSettings();
		b.RtEnabled = rt.Enabled;
		b.RtSamples = rt.Samples;
		b.RtAmbientOcclusion = rt.AmbientOcclusion;
		b.RtReflections = rt.Reflections;
		b.RtShadowDistance = rt.ShadowDistance;
		b.GiMode = RendererSettings::GetGiMode();
		b.GiDistance = RendererSettings::GetGiDistance();
		b.Ssao = RendererSettings::GetSsaoEnabled();
		b.ContactSteps = RendererSettings::GetContactShadowSteps();
		b.VirtualLevels = RendererSettings::GetVirtualClipmapLevels();
		b.ScreenLights = RendererSettings::GetScreenLightBudget();
		b.ShadowLights = RendererSettings::GetShadowLightBudget();
		b.VolumetricLights = RendererSettings::GetVolumetricLightBudget();
		b.NaniteError = VirtualGeometryTraversal::GetSettings().TargetErrorPixels;
		b.CloudSteps = CloudRenderer::GetSettings().ViewSteps;
		b.ResolutionScale = RenderConfiguration::GetResolutionScale();
		return b;
	}

	static void CaptureBaseline()
	{
		s_Base = ReadCurrent();
		s_Applied = s_Base;
		s_BaselineCaptured = true;
	}

	// 自分が最後に適用した値と現在値が違う項目 = ユーザー（またはプロジェクト設定読み込み）が変えた項目。
	static void AbsorbUserChanges()
	{
		const Baseline now = ReadCurrent();
		auto absorb = [](auto& base, const auto& current, const auto& applied)
		{
			if (current != applied) base = current;
		};
		absorb(s_Base.RtEnabled, now.RtEnabled, s_Applied.RtEnabled);
		absorb(s_Base.RtSamples, now.RtSamples, s_Applied.RtSamples);
		absorb(s_Base.RtAmbientOcclusion, now.RtAmbientOcclusion, s_Applied.RtAmbientOcclusion);
		absorb(s_Base.RtReflections, now.RtReflections, s_Applied.RtReflections);
		absorb(s_Base.RtShadowDistance, now.RtShadowDistance, s_Applied.RtShadowDistance);
		absorb(s_Base.GiMode, now.GiMode, s_Applied.GiMode);
		absorb(s_Base.GiDistance, now.GiDistance, s_Applied.GiDistance);
		absorb(s_Base.Ssao, now.Ssao, s_Applied.Ssao);
		absorb(s_Base.ContactSteps, now.ContactSteps, s_Applied.ContactSteps);
		absorb(s_Base.VirtualLevels, now.VirtualLevels, s_Applied.VirtualLevels);
		absorb(s_Base.ScreenLights, now.ScreenLights, s_Applied.ScreenLights);
		absorb(s_Base.ShadowLights, now.ShadowLights, s_Applied.ShadowLights);
		absorb(s_Base.VolumetricLights, now.VolumetricLights, s_Applied.VolumetricLights);
		absorb(s_Base.NaniteError, now.NaniteError, s_Applied.NaniteError);
		absorb(s_Base.CloudSteps, now.CloudSteps, s_Applied.CloudSteps);
		if (!s_ResolutionReduced)
		{
			absorb(s_Base.ResolutionScale, now.ResolutionScale, s_Applied.ResolutionScale);
		}
		s_Applied = now;
	}

	static void Apply()
	{
		const int t = s_Tier;
		Baseline v = s_Base;

		// ---- 可変レイトレーシング ----
		static const int   kSamples[kTierCount]      = { 8, 2, 1, 1, 1, 1 };
		static const float kShadowDist[kTierCount]   = { 1.0f, 0.75f, 0.5f, 0.35f, 0.25f, 0.2f };
		v.RtSamples = (std::min)(s_Base.RtSamples, kSamples[t]);
		v.RtShadowDistance = s_Base.RtShadowDistance * kShadowDist[t];
		const bool toggles = s_Options.AllowFeatureToggles;
		v.RtAmbientOcclusion = s_Base.RtAmbientOcclusion && (!toggles || t <= 2);
		v.RtReflections = s_Base.RtReflections && (!toggles || t <= 3);
		// レイトレ自体は切らない（RTシャドウ使用時に切るとラスターシャドウが走り、逆に重くなるため）。
		v.RtEnabled = s_Base.RtEnabled;

		// ---- 可変GI ----
		static const float kGiDist[kTierCount] = { 1.0f, 0.8f, 0.6f, 0.45f, 0.35f, 0.25f };
		v.GiDistance = (std::max)(1.0f, s_Base.GiDistance * kGiDist[t]);
		if (!toggles)
		{
			v.GiMode = s_Base.GiMode;
		}
		else if (s_Base.GiMode == GlobalIlluminationMode::Realtime)
		{
			v.GiMode = t <= 1 ? GlobalIlluminationMode::Realtime :
				(t <= 4 ? GlobalIlluminationMode::ScreenSpace : GlobalIlluminationMode::Off);
		}
		else if (s_Base.GiMode == GlobalIlluminationMode::ScreenSpace)
		{
			v.GiMode = t <= 4 ? GlobalIlluminationMode::ScreenSpace : GlobalIlluminationMode::Off;
		}
		v.Ssao = s_Base.Ssao && (!toggles || t <= 4);

		// ---- シャドウ ----
		static const int kContact[kTierCount] = { 24, 12, 10, 8, 6, 4 };
		static const int kLevels[kTierCount] = { 4, 4, 4, 3, 3, 2 };
		v.ContactSteps = (std::min)(s_Base.ContactSteps, kContact[t]);
		v.VirtualLevels = (std::min)(s_Base.VirtualLevels, kLevels[t]);

		// ---- ライト ----
		static const int kScreen[kTierCount] = { 32, 24, 16, 12, 8, 6 };
		static const int kShadow[kTierCount] = { 8, 4, 3, 2, 2, 1 };
		static const int kVolume[kTierCount] = { 5, 4, 3, 2, 1, 0 };
		v.ScreenLights = (std::min)(s_Base.ScreenLights, kScreen[t]);
		v.ShadowLights = (std::max)(1, (std::min)(s_Base.ShadowLights, kShadow[t]));
		v.VolumetricLights = (std::min)(s_Base.VolumetricLights, kVolume[t]);

		// ---- 自作Nanite ----
		static const float kError[kTierCount] = { 1.0f, 1.25f, 1.5f, 2.0f, 2.75f, 3.5f };
		v.NaniteError = s_Base.NaniteError * kError[t];

		// ---- 雲 ----
		static const float kCloud[kTierCount] = { 1.0f, 0.85f, 0.7f, 0.55f, 0.45f, 0.35f };
		v.CloudSteps = (std::max)(16, static_cast<int>(static_cast<float>(s_Base.CloudSteps) * kCloud[t]));

		Write(v);
	}

	static void Write(const Baseline& v)
	{
		auto& rt = Raytracing::GetSettings();
		rt.Enabled = v.RtEnabled;
		rt.Samples = v.RtSamples;
		rt.AmbientOcclusion = v.RtAmbientOcclusion;
		rt.Reflections = v.RtReflections;
		rt.ShadowDistance = v.RtShadowDistance;
		RendererSettings::SetGiMode(v.GiMode);
		RendererSettings::SetGiDistance(v.GiDistance);
		RendererSettings::SetSsaoEnabled(v.Ssao);
		RendererSettings::SetContactShadowSteps(v.ContactSteps);
		RendererSettings::SetVirtualClipmapLevels(v.VirtualLevels);
		RendererSettings::SetScreenLightBudget(v.ScreenLights);
		RendererSettings::SetShadowLightBudget(v.ShadowLights);
		RendererSettings::SetVolumetricLightBudget(v.VolumetricLights);
		VirtualGeometryTraversal::GetSettings().TargetErrorPixels = v.NaniteError;
		CloudRenderer::GetSettings().ViewSteps = v.CloudSteps;
		// Setter内のclampで値が変わる可能性があるため、実際の値を読み直して記録する。
		s_Applied = ReadCurrent();
	}

	static void ReduceResolution()
	{
		const float target = (std::max)(0.35f, s_Base.ResolutionScale * 0.8f); // NIS も多段で 50% 未満に対応したため
		if (target < RenderConfiguration::GetResolutionScale() - 0.01f)
		{
			RenderConfiguration::SetResolutionScale(target);
			s_ResolutionReduced = true;
			s_Applied.ResolutionScale = target;
		}
	}

	static void RestoreResolution()
	{
		if (!s_ResolutionReduced) return;
		RenderConfiguration::SetResolutionScale(s_Base.ResolutionScale);
		s_Applied.ResolutionScale = s_Base.ResolutionScale;
		s_ResolutionReduced = false;
	}
};



