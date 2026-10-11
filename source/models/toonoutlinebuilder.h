#pragma once

#include "main.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>


	static constexpr int kToonOutlineModeCount = 4;

	enum class ToonOutlineMeshMode : int
	{
		Balanced = 0,
		Boundary = 1,
		HardEdge = 2,
		Clean = 3,
	};

	struct ToonOutlineOptions
	{
		bool IncludeBoundaryEdges = true;
		bool IncludeHardEdges = true;
		float HardEdgeAngleDegrees = 55.0f;
		float WeldTolerance = 0.0005f;
		float MinEdgeLength = 0.0f;
	};

	inline ToonOutlineOptions GetToonOutlinePreset(ToonOutlineMeshMode mode)
	{
		switch (mode)
		{
		case ToonOutlineMeshMode::Boundary:
			return { true, false, 180.0f, 0.0010f, 0.0020f };
		case ToonOutlineMeshMode::HardEdge:
			return { false, true, 50.0f, 0.0010f, 0.0010f };
		case ToonOutlineMeshMode::Clean:
			return { true, true, 70.0f, 0.0030f, 0.0100f };
		case ToonOutlineMeshMode::Balanced:
		default:
			return { true, true, 55.0f, 0.0010f, 0.0020f };
		}
	}

	struct ToonOutlineEdgeKey
	{
		uint32_t A = 0;
		uint32_t B = 0;

		ToonOutlineEdgeKey() = default;
		ToonOutlineEdgeKey(uint32_t a, uint32_t b)
		{
			A = std::min(a, b);
			B = std::max(a, b);
		}

		bool operator==(const ToonOutlineEdgeKey& rhs) const
		{
			return A == rhs.A && B == rhs.B;
		}
	};

	struct ToonOutlineEdgeKeyHash
	{
		size_t operator()(const ToonOutlineEdgeKey& key) const
		{
			return (static_cast<size_t>(key.A) << 32) ^ static_cast<size_t>(key.B);
		}
	};

	struct ToonOutlineEdgeInfo
	{
		uint32_t Count = 0;
		XMFLOAT3 Normal[2] = {};
		uint32_t OriginalA = 0;
		uint32_t OriginalB = 0;
	};

	struct ToonOutlineQuantizedPosition
	{
		int X = 0;
		int Y = 0;
		int Z = 0;

		bool operator==(const ToonOutlineQuantizedPosition& rhs) const
		{
			return X == rhs.X && Y == rhs.Y && Z == rhs.Z;
		}
	};

	struct ToonOutlineQuantizedPositionHash
	{
		size_t operator()(const ToonOutlineQuantizedPosition& key) const
		{
			size_t h = static_cast<size_t>(key.X) * 73856093u;
			h ^= static_cast<size_t>(key.Y) * 19349663u;
			h ^= static_cast<size_t>(key.Z) * 83492791u;
			return h;
		}
	};

	inline XMFLOAT3 ToonOutlineAdd(const XMFLOAT3& a, const XMFLOAT3& b)
	{
		return { a.x + b.x, a.y + b.y, a.z + b.z };
	}

	inline XMFLOAT3 ToonOutlineSubtract(const XMFLOAT3& a, const XMFLOAT3& b)
	{
		return { a.x - b.x, a.y - b.y, a.z - b.z };
	}

	inline XMFLOAT3 ToonOutlineScale(const XMFLOAT3& v, float s)
	{
		return { v.x * s, v.y * s, v.z * s };
	}

	inline float ToonOutlineDot(const XMFLOAT3& a, const XMFLOAT3& b)
	{
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}

	inline XMFLOAT3 ToonOutlineCross(const XMFLOAT3& a, const XMFLOAT3& b)
	{
		return
		{
			a.y * b.z - a.z * b.y,
			a.z * b.x - a.x * b.z,
			a.x * b.y - a.y * b.x
		};
	}

	inline XMFLOAT3 NormalizeToonOutlineOr(const XMFLOAT3& v, const XMFLOAT3& fallback)
	{
		const float lenSq = ToonOutlineDot(v, v);
		if (lenSq <= 1.0e-8f)
		{
			return fallback;
		}
		const float invLen = 1.0f / std::sqrt(lenSq);
		return ToonOutlineScale(v, invLen);
	}

	inline float ToonOutlineLength(const XMFLOAT3& v)
	{
		return std::sqrt(ToonOutlineDot(v, v));
	}

	inline ToonOutlineQuantizedPosition QuantizeToonOutlinePosition(const XMFLOAT3& p, float tolerance)
	{
		if (tolerance <= 0.0f)
		{
			return
			{
				static_cast<int>(std::round(p.x * 1000000.0f)),
				static_cast<int>(std::round(p.y * 1000000.0f)),
				static_cast<int>(std::round(p.z * 1000000.0f))
			};
		}
		const float invTolerance = 1.0f / tolerance;
		return
		{
			static_cast<int>(std::round(p.x * invTolerance)),
			static_cast<int>(std::round(p.y * invTolerance)),
			static_cast<int>(std::round(p.z * invTolerance))
		};
	}

	template <typename TVertex>
	void BuildTeoMesh(
		const std::vector<TVertex>& sourceVertices,
		const std::vector<unsigned int>& sourceIndices,
		std::vector<TVertex>& outVertices,
		std::vector<unsigned int>& outIndices,
		const ToonOutlineOptions& options)
	{
		outVertices.clear();
		outIndices.clear();

		if (sourceVertices.empty() || sourceIndices.size() < 3)
		{
			return;
		}

		const float clampedAngle = std::clamp(options.HardEdgeAngleDegrees, 0.0f, 180.0f);
		const float hardEdgeCos = std::cos(clampedAngle * 3.1415926535f / 180.0f);
		const float minEdgeLength = std::max(options.MinEdgeLength, 0.0f);

		std::vector<uint32_t> canonicalIndices(sourceVertices.size());
		std::unordered_map<ToonOutlineQuantizedPosition, uint32_t, ToonOutlineQuantizedPositionHash> canonicalByPosition;
		canonicalByPosition.reserve(sourceVertices.size());
		for (uint32_t i = 0; i < sourceVertices.size(); ++i)
		{
			const ToonOutlineQuantizedPosition key = QuantizeToonOutlinePosition(sourceVertices[i].Position, options.WeldTolerance);
			auto it = canonicalByPosition.find(key);
			if (it == canonicalByPosition.end())
			{
				canonicalByPosition[key] = i;
				canonicalIndices[i] = i;
			}
			else
			{
				canonicalIndices[i] = it->second;
			}
		}

		std::unordered_map<ToonOutlineEdgeKey, ToonOutlineEdgeInfo, ToonOutlineEdgeKeyHash> edges;
		edges.reserve(sourceIndices.size());

		auto addEdge = [&](uint32_t a, uint32_t b, const XMFLOAT3& faceNormal)
			{
				const uint32_t ca = canonicalIndices[a];
				const uint32_t cb = canonicalIndices[b];
				if (ca == cb)
				{
					return;
				}
				ToonOutlineEdgeInfo& info = edges[ToonOutlineEdgeKey(ca, cb)];
				if (info.Count == 0)
				{
					info.OriginalA = a;
					info.OriginalB = b;
				}
				if (info.Count < 2)
				{
					info.Normal[info.Count] = faceNormal;
				}
				++info.Count;
			};

		for (size_t i = 0; i + 2 < sourceIndices.size(); i += 3)
		{
			const uint32_t i0 = sourceIndices[i + 0];
			const uint32_t i1 = sourceIndices[i + 1];
			const uint32_t i2 = sourceIndices[i + 2];
			if (i0 >= sourceVertices.size() || i1 >= sourceVertices.size() || i2 >= sourceVertices.size())
			{
				continue;
			}

			const XMFLOAT3& p0 = sourceVertices[i0].Position;
			const XMFLOAT3& p1 = sourceVertices[i1].Position;
			const XMFLOAT3& p2 = sourceVertices[i2].Position;
			const XMFLOAT3 faceNormal = NormalizeToonOutlineOr(ToonOutlineCross(ToonOutlineSubtract(p1, p0), ToonOutlineSubtract(p2, p0)), { 0.0f, 1.0f, 0.0f });

			addEdge(i0, i1, faceNormal);
			addEdge(i1, i2, faceNormal);
			addEdge(i2, i0, faceNormal);
		}

		std::vector<ToonOutlineEdgeKey> selectedEdges;
		selectedEdges.reserve(edges.size());
		std::vector<XMFLOAT3> neighborSum(sourceVertices.size(), { 0.0f, 0.0f, 0.0f });

		for (const auto& kv : edges)
		{
			const ToonOutlineEdgeInfo& info = kv.second;
			const bool boundaryEdge = options.IncludeBoundaryEdges && info.Count == 1;
			const bool hardEdge = options.IncludeHardEdges && info.Count == 2 && ToonOutlineDot(info.Normal[0], info.Normal[1]) < hardEdgeCos;
			if (!boundaryEdge && !hardEdge)
			{
				continue;
			}

			ToonOutlineEdgeKey edge(info.OriginalA, info.OriginalB);
			const XMFLOAT3& pa = sourceVertices[edge.A].Position;
			const XMFLOAT3& pb = sourceVertices[edge.B].Position;
			if (minEdgeLength > 0.0f && ToonOutlineLength(ToonOutlineSubtract(pb, pa)) < minEdgeLength)
			{
				continue;
			}

			selectedEdges.push_back(edge);
			neighborSum[edge.A] = ToonOutlineAdd(neighborSum[edge.A], ToonOutlineSubtract(pb, pa));
			neighborSum[edge.B] = ToonOutlineAdd(neighborSum[edge.B], ToonOutlineSubtract(pa, pb));
		}

		outVertices.reserve(selectedEdges.size() * 4);
		outIndices.reserve(selectedEdges.size() * 12);

		for (const ToonOutlineEdgeKey& edge : selectedEdges)
		{
			const TVertex& srcA = sourceVertices[edge.A];
			const TVertex& srcB = sourceVertices[edge.B];

			TVertex aBase = srcA;
			TVertex bBase = srcB;
			TVertex bOuter = srcB;
			TVertex aOuter = srcA;

			aBase.Normal = { 0.0f, 0.0f, 0.0f };
			bBase.Normal = { 0.0f, 0.0f, 0.0f };
			aOuter.Normal = NormalizeToonOutlineOr(ToonOutlineScale(neighborSum[edge.A], -1.0f), srcA.Normal);
			bOuter.Normal = NormalizeToonOutlineOr(ToonOutlineScale(neighborSum[edge.B], -1.0f), srcB.Normal);

			const unsigned int base = static_cast<unsigned int>(outVertices.size());
			outVertices.push_back(aBase);
			outVertices.push_back(bBase);
			outVertices.push_back(bOuter);
			outVertices.push_back(aOuter);

			outIndices.push_back(base + 0);
			outIndices.push_back(base + 1);
			outIndices.push_back(base + 2);
			outIndices.push_back(base + 0);
			outIndices.push_back(base + 2);
			outIndices.push_back(base + 3);
			outIndices.push_back(base + 2);
			outIndices.push_back(base + 1);
			outIndices.push_back(base + 0);
			outIndices.push_back(base + 3);
			outIndices.push_back(base + 2);
			outIndices.push_back(base + 0);
		}
	}

	template <typename TVertex>
	void BuildTeoMesh(
		const std::vector<TVertex>& sourceVertices,
		const std::vector<unsigned int>& sourceIndices,
		std::vector<TVertex>& outVertices,
		std::vector<unsigned int>& outIndices,
		ToonOutlineMeshMode mode)
	{
		BuildTeoMesh(sourceVertices, sourceIndices, outVertices, outIndices, GetToonOutlinePreset(mode));
	}
