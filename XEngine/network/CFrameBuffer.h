#ifndef HQMARKET_NETWORK_CFRAMEBUFFER_H
#define HQMARKET_NETWORK_CFRAMEBUFFER_H
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace net
{
	/*
	* 协议帧
	┌─────────────┬────────────────┐
	 4字节长度字段 │ payload
	 辅助协议信息  │ 真正的业务消息
	└─────────────┴────────────────┘
	*/

	class CFrameBuffer final
	{
	public:
		CFrameBuffer();

		void Reset();
		bool HasError() const;
		static std::optional<std::string> Encode(const std::string& strPayload);
		std::optional<std::vector<std::string>> Decode(const void* pData, std::size_t nLength);

	private:
		static const std::size_t m_n_max_frame_size{ 8 * 1024 * 1024 };
		std::vector<std::uint8_t> m_recv_buffer;
		bool m_bError{ false };
	};
} // namespace net
#endif
