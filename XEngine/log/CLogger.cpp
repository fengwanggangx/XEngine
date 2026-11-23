#include "CLogger.h"

#include <filesystem>
#include <iostream>
#include <utility>
#include <spdlog/async.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace
{
	struct DailyFilename
	{
		static spdlog::filename_t calc_filename(const spdlog::filename_t& base, const std::tm& date)
		{
			auto [stem, extension] = spdlog::details::file_helper::split_by_extension(base);
			return spdlog::fmt_lib::format("{}_{:04}{:02}{:02}{}", stem,
				date.tm_year + 1900, date.tm_mon + 1, date.tm_mday, extension);
		}
	};

	using _TyDailySink = spdlog::sinks::daily_file_sink<std::mutex, DailyFilename>;
} // namespace

CLogger* CLogger::Instance()
{
	static CLogger* p = new CLogger();
	return p;
}

void CLogger::Initialize(const std::string& project)
{
	CLogger* instance = Instance();
	if (nullptr != instance->m_logger)
	{
		return;
	}

	std::vector<spdlog::sink_ptr> sinks{ std::make_shared<spdlog::sinks::stderr_sink_mt>() };
	std::filesystem::path path;
	try
	{
		path = std::filesystem::read_symlink("/proc/self/exe").parent_path() / "logs" /
			   (project + ".log");
		sinks.emplace_back(std::make_shared<_TyDailySink>(path.string(), 0, 0));
	}
	catch (const std::exception& error)
	{
		std::cerr << "File logging initialization failed (" << path << "): " << error.what() << '\n';
	}

	std::shared_ptr<spdlog::details::thread_pool> pool = std::make_shared<spdlog::details::thread_pool>(200, 1);
	std::shared_ptr<spdlog::async_logger> logger = std::make_shared<spdlog::async_logger>(project, sinks.begin(), sinks.end(),
		pool, spdlog::async_overflow_policy::block);
	logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
	logger->set_level(spdlog::level::trace);
	logger->flush_on(spdlog::level::info);
	instance->m_pool = std::move(pool);
	instance->m_logger = std::move(logger);
}

CLogger::~CLogger()
{
	if (nullptr != m_logger)
	{
		m_logger->flush();
		m_pool.reset();
	}
}

void CLogger::WriteMessage(Level level, const char* module, const std::string& message)
{
	if (nullptr == m_logger)
	{
		std::ostream& stream = Level::Error == level ? std::cerr : std::cout;
		stream << '[' << module << "] " << message << '\n';
		return;
	}

	spdlog::level::level_enum severity = spdlog::level::info;
	switch (level)
	{
	case Level::Debug:
		severity = spdlog::level::debug;
		break;
	case Level::Warning:
		severity = spdlog::level::warn;
		break;
	case Level::Error:
		severity = spdlog::level::err;
		break;
	case Level::Info:
		break;
	}
	m_logger->log(severity, "[{}] {}", module, message);
}
