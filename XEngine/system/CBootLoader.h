#ifndef HQMARKET_SYSTEM_CBOOTLOADER_H
#define HQMARKET_SYSTEM_CBOOTLOADER_H

#include "../indicator/CIndicator.h"

#include <string>
#include <thread>
#include <filesystem>

namespace net
{
	class CHttpServer;
	class CTcpServer;
} // namespace net

class CSession;
class CTradeService;
class CAccountService;
class CEngineCenter;
class CHQService;
class CStrategyService;

class CBootLoader final
{
public:
	CBootLoader();
	~CBootLoader();
	CBootLoader(const CBootLoader&) = delete;
	CBootLoader& operator=(const CBootLoader&) = delete;

	bool Initialize();
	bool Run();
	void Finalize();
	const std::filesystem::path& GetRoot() const;
	net::CTcpServer& GetTcpServer();
	net::CHttpServer& GetHttpServer();
	const std::string& GetLastError() const;
	int GetErrorCode() const;

private:
	std::filesystem::path m_exec;
	std::filesystem::path m_path_py_runtime;
	std::filesystem::path m_path_py_scripts;
	std::string m_last_error;
	int m_error_code{ 0 };
	bool m_initialized{ false };

private:
	net::CTcpServer* m_tcp_server{ nullptr };
	net::CHttpServer* m_http_server{ nullptr };
	CSession* m_hq_market{ nullptr };
	CSession* m_broker{ nullptr };
	CAccountService* m_account_service{ nullptr };
	CTradeService* m_trade_service{ nullptr };
	CStrategyService* m_strategy_service{ nullptr };
	CHQService* m_hq_service{ nullptr };
	CEngineCenter* m_engine_center{ nullptr };
	CIndicator m_indicator;
};

#endif
