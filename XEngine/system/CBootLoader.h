#ifndef HQMARKET_SYSTEM_CBOOTLOADER_H
#define HQMARKET_SYSTEM_CBOOTLOADER_H

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
class CEngineService;
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
	std::string m_strLastError;
	int m_nErrorCode{ 0 };
	bool m_bInitialized{ false };

private:
	net::CTcpServer* m_pTcpServer{ nullptr };
	net::CHttpServer* m_pHttpServer{ nullptr };
	CSession* m_pHQMarket{ nullptr };
	CSession* m_pBroker{ nullptr };
	CAccountService* m_pAccountService{ nullptr };
	CTradeService* m_pTradeService{ nullptr };
	CStrategyService* m_pStrategyService{ nullptr };
	CEngineService* m_pEngineService{ nullptr };
};

#endif
