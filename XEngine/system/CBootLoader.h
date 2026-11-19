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
	std::string m_str_last_error;
	int m_n_error_code{ 0 };
	bool m_bInitialized{ false };

private:
	net::CTcpServer* m_p_tcp_server{ nullptr };
	net::CHttpServer* m_p_http_server{ nullptr };
	CSession* m_p_hq_market{ nullptr };
	CSession* m_pBroker{ nullptr };
	CAccountService* m_p_account_service{ nullptr };
	CTradeService* m_p_trade_service{ nullptr };
	CStrategyService* m_p_strategy_service{ nullptr };
	CHQService* m_p_hq_service{ nullptr };
	CEngineCenter* m_p_engine_center{ nullptr };
	CIndicator m_indicator;
};

#endif
