#ifndef HQMARKET_SYSTEM_CBOOTLOADER_H
#define HQMARKET_SYSTEM_CBOOTLOADER_H

#include <memory>
#include <string>
#include <thread>
#include <filesystem>

namespace net
{
	class CHttpServer;
	class CTcpServer;
} // namespace net

class CHostMgr;
class CSession;
class CTradeService;
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
	const std::string& GetAccount() const;
	const std::string& GetPassword() const;
	CHostMgr& GetHostMgr();
	const std::string& GetLastError() const;
	int GetErrorCode() const;

private:
	std::filesystem::path m_exec;
	std::filesystem::path m_path_py_runtime;
	std::filesystem::path m_path_py_scripts;
	std::string m_strAccount;
	std::string m_strPassword;
	std::string m_strLastError;
	int m_nErrorCode{ 0 };
	bool m_bInitialized{ false };

private:
	std::unique_ptr<net::CTcpServer> m_pTcpServer;
	std::unique_ptr<net::CHttpServer> m_pHttpServer;
	std::unique_ptr<CHostMgr> m_pHostMgr;
	std::unique_ptr<CSession> m_pSession;
	std::unique_ptr<CTradeService> m_pTradeService;
	std::unique_ptr<CStrategyService> m_pStrategyService;
	std::unique_ptr<CEngineService> m_pEngineService;
};

#endif
