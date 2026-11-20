#include "CFrameBuffer.h"
#include <limits>

namespace net
{
	CFrameBuffer::CFrameBuffer()
	{
		m_recv_buffer.reserve(4096);
	}

	std::optional<std::vector<std::string>> CFrameBuffer::Decode(const void* pData, std::size_t nLength)
	{
		if (m_error || ((nullptr == pData) && (0 != nLength)))
		{
			return std::nullopt;
		}
		if (nLength > (std::numeric_limits<std::size_t>::max)() - m_recv_buffer.size())
		{
			m_error = true;
			return std::nullopt;
		}
		if (0 != nLength)
		{
			const auto* pBytes = static_cast<const std::uint8_t*>(pData);
			m_recv_buffer.insert(m_recv_buffer.end(), pBytes, pBytes + nLength);
		}

		std::vector<std::string> frames;
		std::size_t nOffset = 0;
		while ((m_recv_buffer.size() - nOffset) >= sizeof(std::uint32_t))
		{
			// 读取 4 字节长度头
			std::uint32_t nFrameLength = (static_cast<std::uint32_t>(m_recv_buffer[nOffset]) << 24) |
										 (static_cast<std::uint32_t>(m_recv_buffer[nOffset + 1]) << 16) |
										 (static_cast<std::uint32_t>(m_recv_buffer[nOffset + 2]) << 8) |
										 static_cast<std::uint32_t>(m_recv_buffer[nOffset + 3]);
			// 帧长度为 0 或超过允许的最大帧长度
			if ((0 == nFrameLength) || (nFrameLength > m_max_frame_size))
			{
				m_error = true;
				return std::nullopt;
			}
			// 保留半包，等待下次数据
			if ((m_recv_buffer.size() - nOffset - sizeof(std::uint32_t)) < nFrameLength)
			{
				break;
			}
			const char* pPayload = reinterpret_cast<const char*>(m_recv_buffer.data() + nOffset + sizeof(std::uint32_t));
			frames.emplace_back(pPayload, nFrameLength);
			nOffset += sizeof(std::uint32_t) + nFrameLength;
		}
		if (0 != nOffset)
		{
			// 删除所有已完成帧的数据，尾部半包继续保留
			m_recv_buffer.erase(m_recv_buffer.begin(), m_recv_buffer.begin() + static_cast<std::ptrdiff_t>(nOffset));
		}
		return frames;
	}

	void CFrameBuffer::Reset()
	{
		m_recv_buffer.clear();
		m_error = false;
	}

	bool CFrameBuffer::HasError() const
	{
		return m_error;
	}

	std::optional<std::string> CFrameBuffer::Encode(const std::string& strPayload)
	{
		std::size_t sz = strPayload.size();
		if (strPayload.empty() || (sz > m_max_frame_size))
		{
			return std::nullopt;
		}

		std::uint32_t nLength = static_cast<std::uint32_t>(strPayload.size());
		std::string frame(sizeof(std::uint32_t), '\0');
		frame[0] = static_cast<char>((nLength >> 24) & 0xFF);
		frame[1] = static_cast<char>((nLength >> 16) & 0xFF);
		frame[2] = static_cast<char>((nLength >> 8) & 0xFF);
		frame[3] = static_cast<char>(nLength & 0xFF);
		frame.append(strPayload);
		return frame;
	}
} // namespace net
