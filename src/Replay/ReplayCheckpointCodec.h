/**
*  yrpp-spawner
*
*  Copyright(C) 2026-present CnCNet
*
*  This program is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation, either version 3 of the License, or
*  (at your option) any later version.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program.If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace Replay::CheckpointCodec
{
	constexpr size_t MaxBytes = 32u * 1024u * 1024u;
	// Field-wise encoding excludes STL internals and structure padding.
	// Scalars use the game's little-endian representation; bools are checked bytes.
	class Writer
	{
	public:
		std::vector<unsigned char> Bytes;
		bool Good = true;
		void Raw(const void* data, size_t size)
		{
			if (!Good || size > MaxBytes - Bytes.size()) { Good = false; return; }
			if (size == 0) return;
			const auto* p = static_cast<const unsigned char*>(data);
			Bytes.insert(Bytes.end(), p, p + size);
		}
		template<typename T> void Scalar(const T& value)
		{
			static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
			Raw(&value, sizeof(T));
		}
	};
	class Reader
	{
	public:
		const std::vector<unsigned char>& Bytes;
		size_t Position = 0;
		size_t AllocationLeft = MaxBytes * 4;
		bool Good = true;
		void Raw(void* data, size_t size)
		{
			if (!Good || size > Bytes.size() - Position) { Good = false; return; }
			if (size > 0) std::memcpy(data, Bytes.data() + Position, size);
			Position += size;
		}
		template<typename T> void Scalar(T& value)
		{
			static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
			if (!Good || sizeof(T) > Bytes.size() - Position) { Good = false; return; }
			if constexpr (std::is_same_v<T, bool>)
			{
				if (Bytes[Position] > 1) { Good = false; return; }
			}
			std::memcpy(&value, Bytes.data() + Position, sizeof(T));
			Position += sizeof(T);
		}
	};
	template<typename Archive, typename T> void Value(Archive& archive, T& value);
	template<typename Archive, typename T, size_t N> void Value(Archive&, std::array<T, N>&);
	template<typename Archive, typename T, size_t N> void Value(Archive&, T (&)[N]);
	template<typename Archive, typename T> void Value(Archive&, std::vector<T>&);
	template<typename Archive, typename T, size_t N>
	void Value(Archive& archive, std::array<T, N>& values)
	{
		if constexpr (std::is_same_v<T, unsigned char>) archive.Raw(values.data(), N);
		else for (auto& value : values) Value(archive, value);
	}
	template<typename Archive, typename T, size_t N>
	void Value(Archive& archive, T (&values)[N])
	{
		for (auto& value : values) Value(archive, value);
	}
	template<typename Archive, typename T>
	void Value(Archive& archive, std::vector<T>& values)
	{
		uint32_t count = static_cast<uint32_t>(values.size());
		archive.Scalar(count);
		if constexpr (std::is_same_v<Archive, Reader>)
		{
			// Bound both wire size and cumulative allocations across nested vectors.
			if (!archive.Good || count > archive.Bytes.size() - archive.Position
				|| count > archive.AllocationLeft / sizeof(T))
			{ archive.Good = false; return; }
			archive.AllocationLeft -= static_cast<size_t>(count) * sizeof(T);
			values.resize(count);
		}
		if constexpr (std::is_same_v<T, unsigned char>) archive.Raw(values.data(), values.size());
		else for (auto& value : values)
		{
			if (!archive.Good) break;
			Value(archive, value);
		}
	}
	template<typename Archive, typename... T> void Fields(Archive& archive, T&... values)
	{
		(Value(archive, values), ...);
	}
	template<typename Archive, typename T> void Value(Archive& archive, T& value)
	{
		if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) archive.Scalar(value);
		else Visit(archive, value);
	}
}
