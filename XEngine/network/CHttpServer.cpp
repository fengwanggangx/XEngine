#include "CHttpServer.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/keyvalq_struct.h>
#include <utility>
#include "../common/defines.h"
#include "../common/utility.h"
#include "../common/container.h"

namespace
{
	std::string GetMethodName(net::HttpMethod method)
	{
		switch (method)
		{
		case net::HttpMethod::GET:
			return "GET";
		case net::HttpMethod::POST:
			return "POST";
		case net::HttpMethod::PUT:
			return "PUT";
		case net::HttpMethod::DELETE:
			return "DELETE";
		case net::HttpMethod::OPTIONS:
			return "OPTIONS";
		case net::HttpMethod::PATCH:
			return "PATCH";
		default:
			return "UNKNOWN";
		}
	}

	net::HttpMethod Cmd2Method(evhttp_cmd_type method)
	{
		switch (method)
		{
		case EVHTTP_REQ_GET:
			return net::HttpMethod::GET;
		case EVHTTP_REQ_POST:
			return net::HttpMethod::POST;
		case EVHTTP_REQ_PUT:
			return net::HttpMethod::PUT;
		case EVHTTP_REQ_DELETE:
			return net::HttpMethod::DELETE;
		case EVHTTP_REQ_OPTIONS:
			return net::HttpMethod::OPTIONS;
		case EVHTTP_REQ_PATCH:
			return net::HttpMethod::PATCH;
		default:
			return net::HttpMethod::UNKNOWN;
		}
	}

	std::string FmtRouteKey(net::HttpMethod method, const std::string& strPath)
	{
		return GetMethodName(method) + " " + strPath;
	}

	std::string DecodeURI(const std::string& strVal)
	{
		size_t sz = 0;
		char* p = evhttp_uridecode(strVal.c_str(), 1, &sz);
		if (nullptr == p)
		{
			return strVal;
		}
		std::string strRet(p, sz);
		free(p);
		return strRet;
	}

	void ParseQuery(const char* pQuery, std::unordered_map<std::string, std::string>& queries)
	{
		if ((nullptr == pQuery) || ('\0' == *pQuery))
		{
			return;
		}
		std::string strQuery(pQuery);
		std::size_t nBegin = 0;
		while (nBegin <= strQuery.size())
		{
			std::size_t nEnd = strQuery.find('&', nBegin);
			std::string strItem = strQuery.substr(nBegin, nEnd - nBegin);
			std::size_t nEqual = strItem.find('=');
			std::string strKey = DecodeURI(strItem.substr(0, nEqual));
			std::string strVal = (std::string::npos == nEqual) ? std::string() : DecodeURI(strItem.substr(nEqual + 1));
			if (!strKey.empty())
			{
				queries[strKey] = strVal;
			}
			if (std::string::npos == nEnd)
			{
				break;
			}
			nBegin = nEnd + 1;
		}
	}

	std::string EncodeSSE(const std::string& strEvent, const std::string& strData)
	{
		std::string strRet;
		if (!strEvent.empty())
		{
			strRet += "event: " + strEvent + "\n";
		}
		std::size_t nBegin = 0;
		do
		{
			std::size_t nEnd = strData.find('\n', nBegin);
			std::string strLine = strData.substr(nBegin, nEnd - nBegin);
			if (!strLine.empty() && ('\r' == strLine.back()))
			{
				strLine.pop_back();
			}
			strRet += "data: " + strLine + "\n";
			if (std::string::npos == nEnd)
			{
				break;
			}
			nBegin = nEnd + 1;
		} while (nBegin <= strData.size());
		strRet += "\n";
		return strRet;
	}

	struct AsyncResponseContext
	{
		evhttp_request* m_request{ nullptr };
		std::unique_ptr<net::CHttpResponseData> m_response;
	};

	void AsyncResponse_Callback(evutil_socket_t, short, void* pArg)
	{
		std::unique_ptr<AsyncResponseContext> context(static_cast<AsyncResponseContext*>(pArg));
		if ((nullptr == context) || (nullptr == context->m_request) || (nullptr == context->m_response))
		{
			return;
		}

		// evhttp_send_reply() 会在响应数据写完后通过 evhttp_send_done()
		// 自动释放 request。这里不能再用 unique_ptr/evhttp_request_free()，
		// 否则会在事件循环中形成二次释放。
		evhttp_request* pRequest = context->m_request;
		evkeyvalq* pHeaders = evhttp_request_get_output_headers(pRequest);
		for (const auto& item : context->m_response->m_headers)
		{
			evhttp_add_header(pHeaders, item.first.c_str(), item.second.c_str());
		}

		evbuffer* pBuffer = evbuffer_new();
		if (nullptr != pBuffer)
		{
			evbuffer_add(pBuffer, context->m_response->m_body.data(), context->m_response->m_body.size());
		}
		evhttp_send_reply(pRequest, context->m_response->m_status, nullptr, pBuffer);
		if (nullptr != pBuffer)
		{
			evbuffer_free(pBuffer);
		}
	}

	bool PostAsyncResponse(event_base* pBase, evhttp_request* pRequest, std::unique_ptr<net::CHttpResponseData>&& response)
	{
		auto* pContext = new AsyncResponseContext{ pRequest, std::move(response) };
		timeval timeout{ 0, 0 };
		if (0 != event_base_once(pBase, -1, EV_TIMEOUT, AsyncResponse_Callback, pContext, &timeout))
		{
			delete pContext;
			return false;
		}
		return true;
	}
} // namespace

namespace net
{
	struct CHttpStream::State
	{
		struct CloseContext
		{
			std::shared_ptr<State> m_state;
		};

		struct DrainContext
		{
			std::shared_ptr<State> m_state;
		};

		event_base* m_base{ nullptr };
		evhttp_request* m_request{ nullptr };
		evhttp_connection* m_connection{ nullptr };
		std::mutex m_state_mutex;
		std::deque<std::string> m_messages;
		std::function<void()> m_close_handler;
		CloseContext* m_close_context{ nullptr };
		std::weak_ptr<State> m_self;
		bool m_scheduled{ false };
		bool m_close_requested{ false };
		bool m_closed{ false };

		static void ConnectionClose_Callback(evhttp_connection*, void* pArg)
		{
			std::unique_ptr<CloseContext> context(static_cast<CloseContext*>(pArg));
			if ((nullptr != context) && (nullptr != context->m_state))
			{
				context->m_state->OnConnectionClosed(context.get());
			}
		}

		static void Drain_Callback(evutil_socket_t, short, void* pArg)
		{
			std::unique_ptr<DrainContext> context(static_cast<DrainContext*>(pArg));
			if ((nullptr != context) && (nullptr != context->m_state))
			{
				context->m_state->Drain();
			}
		}

		bool Schedule()
		{
			std::shared_ptr<State> state = m_self.lock();
			if (nullptr == state)
			{
				return false;
			}
			DrainContext* pContext = new DrainContext{ std::move(state) };
			timeval timeout{ 0, 0 };
			if (0 != event_base_once(m_base, -1, EV_TIMEOUT, Drain_Callback, pContext, &timeout))
			{
				delete pContext;
				std::lock_guard<std::mutex> lock(m_state_mutex);
				m_scheduled = false;
				return false;
			}
			return true;
		}

		bool Queue(std::string&& strMessage)
		{
			bool bSchedule = false;
			{
				std::lock_guard<std::mutex> lock(m_state_mutex);
				if (m_closed || m_close_requested)
				{
					return false;
				}
				m_messages.emplace_back(std::move(strMessage));
				if (!m_scheduled)
				{
					m_scheduled = true;
					bSchedule = true;
				}
			}
			return !bSchedule || Schedule();
		}

		void RequestClose()
		{
			bool bSchedule = false;
			{
				std::lock_guard<std::mutex> lock(m_state_mutex);
				if (m_closed || m_close_requested)
				{
					return;
				}
				m_close_requested = true;
				if (!m_scheduled)
				{
					m_scheduled = true;
					bSchedule = true;
				}
			}
			if (bSchedule)
			{
				Schedule();
			}
		}

		void Drain()
		{
			std::deque<std::string> pending;
			bool bClose = false;
			{
				std::lock_guard<std::mutex> lock(m_state_mutex);
				if (m_closed)
				{
					m_scheduled = false;
					return;
				}
				pending.swap(m_messages);
				bClose = m_close_requested;
				m_scheduled = false;
			}
			for (const auto& strMessage : pending)
			{
				evbuffer* pBuffer = evbuffer_new();
				if (nullptr != pBuffer)
				{
					evbuffer_add(pBuffer, strMessage.data(), strMessage.size());
					evhttp_send_reply_chunk(m_request, pBuffer);
					evbuffer_free(pBuffer);
				}
			}
			if (bClose)
			{
				CloseOnEventThread();
			}
		}

		void CloseOnEventThread()
		{
			CloseContext* pCloseContext = nullptr;
			std::function<void()> func;
			evhttp_request* pRequest = nullptr;
			{
				std::lock_guard<std::mutex> lock(m_state_mutex);
				if (m_closed)
				{
					return;
				}
				m_closed = true;
				pCloseContext = m_close_context;
				m_close_context = nullptr;
				pRequest = m_request;
				m_request = nullptr;
				func = std::move(m_close_handler);
			}
			if (nullptr != m_connection)
			{
				evhttp_connection_set_closecb(m_connection, nullptr, nullptr);
			}
			delete pCloseContext;
			if (nullptr != pRequest)
			{
				evhttp_send_reply_end(pRequest);
				// evhttp_send_reply_end() 完成发送后由 libevent 释放 request。
			}
			if (func)
			{
				func();
			}
		}

		void OnConnectionClosed(CloseContext* pContext)
		{
			std::function<void()> func;
			evhttp_request* pRequest = nullptr;
			{
				std::lock_guard<std::mutex> lock(m_state_mutex);
				if (m_close_context == pContext)
				{
					m_close_context = nullptr;
				}
				if (m_closed)
				{
					return;
				}
				m_closed = true;
				m_messages.clear();
				pRequest = m_request;
				m_request = nullptr;
				func = std::move(m_close_handler);
			}
			if (nullptr != pRequest)
			{
				evhttp_request_free(pRequest);
			}
			if (func)
			{
				func();
			}
		}
	};

	struct CHttpResponse::State
	{
		event_base* m_base{ nullptr };
		evhttp_request* m_request{ nullptr };
		CHttpServer* m_server{ nullptr };
		int m_status{ HTTP_OK };
		std::unordered_map<std::string, std::string> m_headers;
		std::atomic_int m_mode{ 0 };
	};

	HttpMethod CHttpRequest::GetMethod() const
	{
		return m_method;
	}

	const std::string& CHttpRequest::GetPath() const
	{
		return m_path;
	}

	const std::string& CHttpRequest::GetUri() const
	{
		return m_uri;
	}

	const std::string& CHttpRequest::GetBody() const
	{
		return m_body;
	}

	std::string CHttpRequest::GetHeader(const std::string& strName) const
	{
		return container::vfind(m_headers, utility::lower(strName));
	}

	std::string CHttpRequest::GetQuery(const std::string& strName) const
	{
		return container::vfind(m_queries, strName);
	}

	CHttpStream::CHttpStream(std::shared_ptr<State> state) : m_state(std::move(state))
	{
	}

	bool CHttpStream::Send(const std::string& strData)
	{
		return Send(std::string(), strData);
	}

	bool CHttpStream::Send(const std::string& strEvent, const std::string& strData)
	{
		return (nullptr != m_state) && m_state->Queue(EncodeSSE(strEvent, strData));
	}

	void CHttpStream::Close()
	{
		if (nullptr != m_state)
		{
			m_state->RequestClose();
		}
	}

	void CHttpStream::SetCloseHandler(std::function<void()>&& func)
	{
		if (nullptr != m_state)
		{
			std::lock_guard<std::mutex> lock(m_state->m_state_mutex);
			if (!m_state->m_closed)
			{
				m_state->m_close_handler = std::move(func);
			}
		}
	}

	CHttpResponse::CHttpResponse(std::shared_ptr<State> state) : m_state(std::move(state))
	{
	}

	void CHttpResponse::SetStatus(int nStatus)
	{
		if ((nullptr != m_state) && (0 == m_state->m_mode.load()))
		{
			m_state->m_status = nStatus;
		}
	}

	void CHttpResponse::SetHeader(const std::string& strName, const std::string& strValue)
	{
		if ((nullptr != m_state) && (0 == m_state->m_mode.load()) && !strName.empty())
		{
			m_state->m_headers[strName] = strValue;
		}
	}

	bool CHttpResponse::Send(const std::string& strBody)
	{
		if (nullptr == m_state)
		{
			return false;
		}
		int nExpected = 0;
		if (!m_state->m_mode.compare_exchange_strong(nExpected, 1))
		{
			return false;
		}
		evkeyvalq* pHeaders = evhttp_request_get_output_headers(m_state->m_request);
		for (const auto& item : m_state->m_headers)
		{
			evhttp_add_header(pHeaders, item.first.c_str(), item.second.c_str());
		}
		evbuffer* pBuffer = evbuffer_new();
		if (nullptr != pBuffer)
		{
			evbuffer_add(pBuffer, strBody.data(), strBody.size());
		}
		evhttp_send_reply(m_state->m_request, m_state->m_status, nullptr, pBuffer);
		if (nullptr != pBuffer)
		{
			evbuffer_free(pBuffer);
		}
		return true;
	}

	bool CHttpResponse::SendJson(const std::string& strBody)
	{
		SetHeader("Content-Type", "application/json; charset=utf-8");
		return Send(strBody);
	}

	std::shared_ptr<CHttpStream> CHttpResponse::BeginSSE()
	{
		if (nullptr == m_state)
		{
			return nullptr;
		}
		int nExpected = 0;
		if (!m_state->m_mode.compare_exchange_strong(nExpected, 2))
		{
			return nullptr;
		}
		evkeyvalq* pHeaders = evhttp_request_get_output_headers(m_state->m_request);
		for (const auto& item : m_state->m_headers)
		{
			evhttp_add_header(pHeaders, item.first.c_str(), item.second.c_str());
		}
		evhttp_add_header(pHeaders, "Content-Type", "text/event-stream; charset=utf-8");
		evhttp_add_header(pHeaders, "Cache-Control", "no-cache");
		evhttp_add_header(pHeaders, "Connection", "keep-alive");
		evhttp_add_header(pHeaders, "X-Accel-Buffering", "no");
		evhttp_request_own(m_state->m_request);
		evhttp_send_reply_start(m_state->m_request, m_state->m_status, nullptr);

		std::shared_ptr<CHttpStream::State> state = std::make_shared<CHttpStream::State>();
		state->m_self = state;
		state->m_base = m_state->m_base;
		state->m_request = m_state->m_request;
		state->m_connection = evhttp_request_get_connection(m_state->m_request);
		state->m_close_context = new CHttpStream::State::CloseContext{ state };
		if (nullptr != state->m_connection)
		{
			evhttp_connection_set_closecb(state->m_connection, CHttpStream::State::ConnectionClose_Callback, state->m_close_context);
		}
		if (nullptr != m_state->m_server)
		{
			m_state->m_server->RegisterStream(state);
		}
		return std::shared_ptr<CHttpStream>(new CHttpStream(std::move(state)));
	}

	bool CHttpResponse::IsHandled() const
	{
		return (nullptr != m_state) && (0 != m_state->m_mode.load());
	}

	CHttpServer::CHttpServer(int nPort) : m_port(nPort)
	{
	}

	CHttpServer::~CHttpServer()
	{
		Release();
	}

	int CHttpServer::Initialize()
	{
		if ((nullptr == GetNet()) || (nullptr != m_http) || (m_port < 0) || (m_port > 65535))
		{
			return -1;
		}
		m_http = evhttp_new(GetNet());
		if (nullptr == m_http)
		{
			return -2;
		}
		evhttp_set_max_body_size(m_http, 4 * 1024 * 1024);
		evhttp_set_gencb(m_http, CHttpServer::Request_Callback, this);
		m_listener = evhttp_bind_socket_with_handle(m_http, "0.0.0.0", static_cast<ev_uint16_t>(m_port));
		if (nullptr == m_listener)
		{
			evhttp_free(m_http);
			m_http = nullptr;
			return -3;
		}
		return 0;
	}

	void CHttpServer::RegisterHandler(HttpMethod method, const std::string& strPath, _TyHandler&& func)
	{
		if ((HttpMethod::UNKNOWN != method) && !strPath.empty() && func)
		{
			m_handlers[FmtRouteKey(method, strPath)] = std::move(func);
		}
	}

	void CHttpServer::Request_Callback(struct evhttp_request* pRequest, void* pArg)
	{
		CHttpServer* pInstance = static_cast<CHttpServer*>(pArg);
		if ((nullptr != pInstance) && (nullptr != pRequest))
		{
			pInstance->OnRequest(pRequest);
		}
	}

	int CHttpServer::ParseRequest(struct evhttp_request* pRequest, CHttpRequest& request)
	{
		if (nullptr == pRequest)
		{
			return HTTP_BADREQUEST;
		}
		request.m_method = Cmd2Method(evhttp_request_get_command(pRequest));

		// 统一资源标识符 Uniform Resource Identifier
		const char* pURI = evhttp_request_get_uri(pRequest);
		if (nullptr == pURI)
		{
			return HTTP_BADREQUEST;
		}
		request.m_uri = pURI;

		// 结构化Uri
		evhttp_uri* pFmtURI = evhttp_uri_parse(request.m_uri.c_str());
		if (nullptr != pFmtURI)
		{
			const char* pPath = evhttp_uri_get_path(pFmtURI);
			request.m_path = ((nullptr == pPath) || ('\0' == *pPath)) ? "/" : DecodeURI(pPath);
			ParseQuery(evhttp_uri_get_query(pFmtURI), request.m_queries);
			evhttp_uri_free(pFmtURI);
		}
		else
		{
			return HTTP_BADREQUEST;
		}

		evkeyvalq* pHeaders = evhttp_request_get_input_headers(pRequest);
		if (nullptr != pHeaders)
		{
			for (evkeyval* pHeader = pHeaders->tqh_first; nullptr != pHeader; pHeader = pHeader->next.tqe_next)
			{
				if ((nullptr != pHeader->key) && (nullptr != pHeader->value))
				{
					request.m_headers[utility::lower(pHeader->key)] = pHeader->value;
				}
			}
		}
		evbuffer* pInput = evhttp_request_get_input_buffer(pRequest);
		if (nullptr == pInput)
		{
			return HTTP_BADREQUEST;
		}
		std::size_t nLength = evbuffer_get_length(pInput);
		request.m_body.resize(nLength);
		if (nLength > 0)
		{
			const ev_ssize_t nCopied = evbuffer_copyout(pInput, request.m_body.data(), nLength);
			if ((nCopied < 0) || (static_cast<std::size_t>(nCopied) != nLength))
			{
				request.m_body.clear();
				return HTTP_BADREQUEST;
			}
		}
		return 0;
	}

	void CHttpServer::OnRequest(struct evhttp_request* pRequest)
	{
		CHttpRequest request;
		const int nRet = ParseRequest(pRequest, request);
		if (0 != nRet)
		{
			evhttp_send_reply(pRequest, nRet, nullptr, nullptr);
			return;
		}

		const std::string strRouteKey = FmtRouteKey(request.m_method, request.m_path);
		const auto mIter = m_handlers.find(strRouteKey);
		if (m_handlers.end() != mIter)
		{
			// 确定要异步处理后才取得所有权。
			evhttp_request_own(pRequest);
			event_base* pBase = GetNet();
			_TyHandler handler = mIter->second;
			ThreadPoolPtr->PushTask(task_priority::em_normal, 0, [pBase, pRequest, handler = std::move(handler), request = std::move(request)]() mutable
			{
				std::unique_ptr<CHttpResponseData> response;
				try
				{
					response = handler(request);
					if (nullptr == response)
					{
						response = std::make_unique<CHttpResponseData>();
						response->m_status = HTTP_INTERNAL;
						response->m_body = "Internal Server Error";
					}
				}
				catch (...)
				{
					response = std::make_unique<CHttpResponseData>();
					response->m_status = HTTP_INTERNAL;
					response->m_body = "Internal Server Error";
				}

				if (!PostAsyncResponse(pBase, pRequest, std::move(response)))
				{
					evhttp_request_free(pRequest);
				}
			});
			return;
		}

		bool bPathExists = false;
		const std::string strSuffix = " " + request.m_path;
		for (const auto& item : m_handlers)
		{
			if ((item.first.size() >= strSuffix.size()) && (0 == item.first.compare(item.first.size() - strSuffix.size(), strSuffix.size(), strSuffix)))
			{
				bPathExists = true;
				break;
			}
		}
		evhttp_send_reply(pRequest, bPathExists ? HTTP_BADMETHOD : HTTP_NOTFOUND, nullptr, nullptr);
	}

	void CHttpServer::RegisterStream(const std::shared_ptr<CHttpStream::State>& state)
	{
		std::lock_guard<std::mutex> lock(m_streams_mutex);
		m_streams.erase(std::remove_if(m_streams.begin(), m_streams.end(), [](const auto& item)
		{
			return item.expired();
		}),
			m_streams.end());
		m_streams.emplace_back(state);
	}

	void CHttpServer::Release()
	{
		std::vector<std::shared_ptr<CHttpStream::State>> streams;
		{
			std::lock_guard<std::mutex> lock(m_streams_mutex);
			for (const auto& item : m_streams)
			{
				if (auto state = item.lock())
				{
					streams.emplace_back(std::move(state));
				}
			}
			m_streams.clear();
		}
		for (const auto& state : streams)
		{
			state->CloseOnEventThread();
		}
		m_listener = nullptr;
		if (nullptr != m_http)
		{
			evhttp_free(m_http);
			m_http = nullptr;
		}
	}
} // namespace net
