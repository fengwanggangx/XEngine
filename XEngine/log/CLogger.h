#pragma once

#include <memory>
#include <sstream>
#include <string>

namespace spdlog
{
	class async_logger;

	namespace details
	{
		class thread_pool;
	}
} // namespace spdlog

enum class Level
{
	Debug,
	Info,
	Warning,
	Error
};

class CLogger final
{
public:
	static void Initialize(const std::string& project);

	template <typename... _TyPart>
	static void Write(Level level, const char* module, const _TyPart&... parts)
	{
		std::ostringstream message;
		(message << ... << parts);
		Instance()->WriteMessage(level, module, message.str());
	}

private:
	CLogger() = default;
	~CLogger();
	CLogger(const CLogger& arg) = delete;
	CLogger& operator=(const CLogger& arg) = delete;

	static CLogger* Instance();

	void WriteMessage(Level level, const char* module, const std::string& message);

	std::shared_ptr<spdlog::details::thread_pool> m_pool;
	std::shared_ptr<spdlog::async_logger> m_logger;
};
