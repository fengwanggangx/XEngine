#ifndef __CHTTPSERVER_H__
#define __CHTTPSERVER_H__

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "CNet.h"

/*
 * HTTP 请求格式示例：
 *
 * POST /api/users/100?source=pc&debug=1 HTTP/1.1
 * Host: example.com:8080
 * Content-Type: application/json; charset=utf-8
 * Authorization: Bearer abc123
 * User-Agent: WindClient/1.0
 * Accept: application/json
 * Content-Length: 23
 * Connection: keep-alive
 *
 * {"name":"Tom","age":20}
 *
 * HTTP 请求由以下部分组成：
 *
 * 1. 请求行（Request Line）
 *
 *    POST /api/users/100?source=pc&debug=1 HTTP/1.1
 *
 *    Method  = POST
 *    URI     = /api/users/100?source=pc&debug=1
 *    Path    = /api/users/100
 *    Query   = source=pc&debug=1
 *    Version = HTTP/1.1
 *
 * 2. 请求头（Request Headers）
 *
 *    Host          = example.com:8080
 *    Content-Type  = application/json; charset=utf-8
 *    Authorization = Bearer abc123
 *    User-Agent    = WindClient/1.0
 *    Accept        = application/json
 *    Content-Length= 23
 *    Connection    = keep-alive
 *
 * 3. 空行
 *
 *    请求头与请求体之间必须使用空行分隔。
 *    HTTP 协议中实际使用 "\r\n\r\n" 表示请求头结束。
 *
 * 4. 请求体（Request Body）
 *
 *    {"name":"Tom","age":20}
 *
 * libevent 接口与字段的对应关系：
 *
 *    evhttp_request_get_command(request)
 *        -> EVHTTP_REQ_POST
 *
 *    evhttp_request_get_uri(request)
 *        -> "/api/users/100?source=pc&debug=1"
 *
 *    evhttp_uri_get_path(parsedUri)
 *        -> "/api/users/100"
 *
 *    evhttp_uri_get_query(parsedUri)
 *        -> "source=pc&debug=1"
 *
 *    evhttp_request_get_input_headers(request)
 *        -> 请求头集合
 *
 *    evhttp_request_get_input_buffer(request)
 *        -> 请求体数据
 *
 * 解析为 CHttpRequest 后：
 *
 *    m_method  = HttpMethod::POST
 *    m_uri  = "/api/users/100?source=pc&debug=1"
 *    m_path = "/api/users/100"
 *
 *    m_queries["source"] = "pc"
 *    m_queries["debug"]  = "1"
 *
 *    m_headers["host"]          = "example.com:8080"
 *    m_headers["content-type"]  = "application/json; charset=utf-8"
 *    m_headers["authorization"] = "Bearer abc123"
 *
 *    m_body = "{\"name\":\"Tom\",\"age\":20}"
 */

struct evhttp;
struct evhttp_bound_socket;
struct evhttp_request;

namespace net
{
	enum class HttpMethod
	{
		GET,
		POST,
		PUT,
		DELETE,
		OPTIONS,
		PATCH,
		UNKNOWN
	};

	class CHttpRequest final
	{
	public:
		HttpMethod GetMethod() const;
		const std::string& GetPath() const;
		const std::string& GetUri() const;
		const std::string& GetBody() const;
		std::string GetHeader(const std::string& strName) const;
		std::string GetQuery(const std::string& strName) const;

	private:
		friend class CHttpServer;
		CHttpRequest() = default;

	private:
		HttpMethod m_method{ HttpMethod::UNKNOWN };
		std::string m_path;
		std::string m_uri;
		std::string m_body;
		std::unordered_map<std::string, std::string> m_headers;
		std::unordered_map<std::string, std::string> m_queries;
	};

	class CHttpStream final
	{
	public:
		~CHttpStream() = default;
		bool Send(const std::string& strData);
		bool Send(const std::string& strEvent, const std::string& strData);
		void Close();
		void SetCloseHandler(std::function<void()>&& func);

	private:
		friend class CHttpResponse;
		friend class CHttpServer;
		struct State;
		explicit CHttpStream(std::shared_ptr<State> state);

	private:
		std::shared_ptr<State> m_state;
	};

	class CHttpResponse final
	{
	public:
		~CHttpResponse() = default;
		void SetStatus(int nStatus);
		void SetHeader(const std::string& strName, const std::string& strValue);
		bool Send(const std::string& strBody);
		bool SendJson(const std::string& strBody);
		std::shared_ptr<CHttpStream> BeginSSE();

	private:
		friend class CHttpServer;
		struct State;
		explicit CHttpResponse(std::shared_ptr<State> state);
		bool IsHandled() const;

	private:
		std::shared_ptr<State> m_state;
	};

	struct CHttpResponseData
	{
		int m_status{ 200 };
		std::unordered_map<std::string, std::string> m_headers;
		std::string m_body;
	};

	class CHttpServer final : public CNet
	{
		friend class CHttpResponse;
		using _TyHandler = std::function<std::unique_ptr<CHttpResponseData>(const CHttpRequest&)>;

	public:
		explicit CHttpServer(int nPort);
		~CHttpServer() override;
		int Initialize();
		void RegisterHandler(HttpMethod method, const std::string& strPath, _TyHandler&& func);

	private:
		static void Request_Callback(struct evhttp_request* pRequest, void* pArg);
		static int ParseRequest(struct evhttp_request* pRequest, CHttpRequest& request);
		void OnRequest(struct evhttp_request* pRequest);
		void Release();
		void RegisterStream(const std::shared_ptr<CHttpStream::State>& state);

	private:
		int m_port{ -1 };
		struct evhttp* m_http{ nullptr };
		struct evhttp_bound_socket* m_listener{ nullptr };
		std::unordered_map<std::string, _TyHandler> m_handlers;
		std::mutex m_streams_mutex;
		std::vector<std::weak_ptr<CHttpStream::State>> m_streams;
	};
} // namespace net

#endif
