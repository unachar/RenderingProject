#pragma once
// =============================================================================================
// RenderQuality
//   画質向上機能の設定をまとめたヘッダー（ヘッダーのみ。.vcxproj への追加不要）。
//   ・IBL DFG LUT / 多重散乱 GGX エネルギー補償
//   ・エリアライト（LTC）
//   ・トーンマップ / カラーグレーディング
//   ・レンズ効果（ビネット / 色収差 / フィルムグレイン）
//   ・ブルーム（デュアルフィルタ）/ レンズフレア
//   ・天候（雨 / 濡れ / 水たまり / 雪）
//   ・半透明（OIT / RT 屈折）
//   ・ReSTIR GI
//   値は PostProcessParams（b0 / forward b6）末尾の拡張定数で GPU に渡す。
// =============================================================================================
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

enum class ToneMapper : int { AcesApprox = 0, AgX = 1, AcesFitted = 2, PbrNeutral = 3 };

struct RenderQualitySettings
{
		// --- PBR ---
		bool EnergyCompensation = true;     // 多重散乱 GGX のエネルギー補償（DFG LUT）
		bool AreaLights = true;             // 矩形ライト（LTC）
		// --- トーンマップ / グレーディング ---
		ToneMapper Tonemapper = ToneMapper::AgX;
		bool AgXPunchy = false;
		float Contrast = 1.0f;
		float Saturation = 1.0f;
		float Vibrance = 0.0f;
		float Temperature = 0.0f;           // -1(青) .. +1(黄)
		float Tint = 0.0f;                  // -1(緑) .. +1(マゼンタ)
		DirectX::XMFLOAT3 Lift = { 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT3 Gamma = { 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT3 Gain = { 1.0f, 1.0f, 1.0f };
		// --- レンズ ---
		float Vignette = 0.0f;
		float VignetteSmoothness = 0.45f;
		float ChromaticAberration = 0.0f;   // 画面端でのずれ（ピクセル）
		float FilmGrain = 0.0f;
		// --- ブルーム / フレア ---
		bool DualBloom = true;              // 有効時は従来の Bloom ポストプロセスを置き換える
		float BloomIntensity = 0.06f;
		float BloomThreshold = 1.0f;        // 0 で物理ベース（しきい値なし）
		float BloomRadius = 0.85f;          // アップサンプル時のフィルタ半径
		int BloomLevels = 6;
		bool LensFlare = false;
		float LensFlareIntensity = 0.35f;
		float LensFlareHalo = 0.25f;
		// --- 天候 ---
		bool WeatherEnabled = false;
		float RainIntensity = 0.0f;         // 雨粒（画面）と濡れの進行速度
		float Wetness = 0.0f;               // 0..1 濡れ（暗く・ツヤ）
		float Puddles = 0.0f;               // 0..1 水たまり（上向きの面）
		float Snow = 0.0f;                  // 0..1 積雪（上向きの面）
		float SnowFall = 0.0f;              // 降雪（画面）
		float WindX = 0.2f, WindZ = 0.0f;
		bool AutoWetness = true;            // 雨量に応じて濡れ・水たまりを時間変化させる
		// --- 半透明 ---
		bool OrderIndependentTransparency = true;
		bool RaytracedRefraction = true;
		// --- ReSTIR GI ---
		bool ReSTIRGI = true;
		int ReSTIRTemporalMaxM = 20;        // 時間方向の履歴上限
		int ReSTIRSpatialSamples = 4;
		float ReSTIRSpatialRadius = 24.0f;  // ピクセル
		bool ReSTIRSpatialVisibility = true;
};

class RenderQuality final
{
public:
	using Settings = RenderQualitySettings;

	static Settings& Get() { return s_Settings; }

	// Packs the settings into the extended PostProcessParams block (11 x float4).
	static void Pack(DirectX::XMFLOAT4* out, float time, unsigned frame, bool oitActive, bool refractionActive)
	{
		Settings& s = s_Settings;
		UpdateWeather();
		out[0] = { static_cast<float>(s.Tonemapper), (std::max)(0.0f, s.Contrast), (std::max)(0.0f, s.Saturation), s.Vibrance };
		out[1] = { std::clamp(s.Temperature, -1.0f, 1.0f), std::clamp(s.Tint, -1.0f, 1.0f), s.AgXPunchy ? 1.0f : 0.0f, 0.0f };
		out[2] = { s.Lift.x, s.Lift.y, s.Lift.z, 0.0f };
		out[3] = { (std::max)(0.05f, s.Gamma.x), (std::max)(0.05f, s.Gamma.y), (std::max)(0.05f, s.Gamma.z), 0.0f };
		out[4] = { (std::max)(0.0f, s.Gain.x), (std::max)(0.0f, s.Gain.y), (std::max)(0.0f, s.Gain.z), 0.0f };
		out[5] = { std::clamp(s.Vignette, 0.0f, 1.0f), std::clamp(s.VignetteSmoothness, 0.01f, 1.0f),
			std::clamp(s.ChromaticAberration, 0.0f, 16.0f), std::clamp(s.FilmGrain, 0.0f, 1.0f) };
		out[6] = { time, static_cast<float>(frame & 0xffff), 0.0f, 0.0f };
		const bool weather = s.WeatherEnabled;
		out[7] = { weather ? std::clamp(s_EffectiveWetness, 0.0f, 1.0f) : 0.0f, weather ? std::clamp(s_EffectivePuddles, 0.0f, 1.0f) : 0.0f,
			weather ? std::clamp(s.Snow, 0.0f, 1.0f) : 0.0f, weather ? std::clamp(s.RainIntensity, 0.0f, 1.0f) : 0.0f };
		out[8] = { weather ? std::clamp(s.SnowFall, 0.0f, 1.0f) : 0.0f, s.WindX, s.WindZ, 0.0f };
		out[9] = { s.EnergyCompensation ? 1.0f : 0.0f, s.AreaLights ? 1.0f : 0.0f, oitActive ? 1.0f : 0.0f, refractionActive ? 1.0f : 0.0f };
		out[10] = { 0.0f, 0.0f, 0.0f, 0.0f };
	}

	static float GetEffectiveWetness() { return s_EffectiveWetness; }
	static float GetEffectivePuddles() { return s_EffectivePuddles; }

	// Rain gradually soaks surfaces and forms puddles; they dry slowly when it stops.
	static void Tick(float deltaTime)
	{
		Settings& s = s_Settings;
		deltaTime = std::clamp(deltaTime, 0.0f, 0.25f);
		if (!s.WeatherEnabled || !s.AutoWetness)
		{
			s_EffectiveWetness = s.Wetness;
			s_EffectivePuddles = s.Puddles;
			return;
		}
		const float rain = std::clamp(s.RainIntensity, 0.0f, 1.0f);
		const float wetTarget = (std::max)(s.Wetness, rain > 0.01f ? 1.0f : 0.0f);
		const float puddleTarget = (std::max)(s.Puddles, rain * 0.85f);
		const float wetRate = rain > 0.01f ? 0.15f + rain * 0.6f : 0.04f;
		const float puddleRate = rain > 0.01f ? 0.03f + rain * 0.12f : 0.015f;
		s_EffectiveWetness += std::clamp(wetTarget - s_EffectiveWetness, -wetRate * deltaTime, wetRate * deltaTime);
		s_EffectivePuddles += std::clamp(puddleTarget - s_EffectivePuddles, -puddleRate * deltaTime, puddleRate * deltaTime);
	}

private:
	static void UpdateWeather() {}
	static inline Settings s_Settings{};
	static inline float s_EffectiveWetness = 0.0f;
	static inline float s_EffectivePuddles = 0.0f;
};
