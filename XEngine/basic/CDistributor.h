#ifndef __CDISTRIBUTOR_H__
#define __CDISTRIBUTOR_H__
#include <algorithm>
#include <chrono>
#include <shared_mutex>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include "../common/defines.h"
#include "../common/utility.h"

template <bool bAsyn, class _Ty, class _TyHandler>
class CDistributor
{
	using _TyData = Typer<_Ty>::type;
	using _TyDataContainer = std::vector<_TyData>;

public:
	CDistributor() = default;

	~CDistributor()
	{
		StopAndWait();
	}

public:
	void Dispatch(_Ty&& data)
	{
		if constexpr (bAsyn)
		{
			std::lock_guard<std::mutex> taskLock(m_tasks_mutex);
			if (m_stopping)
			{
				return;
			}
			if constexpr (IsContainer<_Ty>)
			{
				m_cache.insert(m_cache.end(), std::make_move_iterator(data.begin()), std::make_move_iterator(data.end()));
			}
			else
			{
				m_cache.emplace_back(std::move(data));
			}
			if (!m_scheduled)
			{
				m_task = ThreadPoolPtr->PushTask(task_priority::em_normal, 0, [this]()
				{
					Drain();
				});
				m_scheduled = true;
			}
		}
		else
		{
			Execute(std::forward<_Ty>(data));
		}
	}

	void RegisterHandler(_TyHandler&& fun)
	{
		std::unique_lock<std::shared_mutex> lock(m_handler_mutex);
		m_handler.emplace_back(std::forward<_TyHandler>(fun));
	}

	void ClearHandlers()
	{
		std::unique_lock<std::shared_mutex> lock(m_handler_mutex);
		m_handler.clear();
	}

	void StopAndWait()
	{
		// 等待所有任务结束，因为异步任务如果捕获的资源可能在断线等情况下释放重置导致失效，因此释放前要等异步任务执行完成后释放
		if constexpr (bAsyn)
		{
			std::shared_future<void> task;
			{
				std::lock_guard<std::mutex> lock(m_tasks_mutex);
				m_stopping = true;
				task = m_task;
			}
			if (task.valid())
			{
				task.wait();
			}
		}
	}

private:
	void Drain()
	{
		while (true)
		{
			_TyDataContainer data;
			{
				std::lock_guard<std::mutex> lock(m_tasks_mutex);
				if (m_cache.empty())
				{
					m_scheduled = false;
					return;
				}
				data.swap(m_cache);
			}
			for (const auto& item : data)
			{
				ExecuteA(item);
			}
		}
	}

	void Execute(_Ty&& data)
	{
		if constexpr (IsContainer<_Ty>)
		{
			for (const auto& item : data)
			{
				ExecuteA(item);
			}
		}
		else
		{
			ExecuteA(data);
		}
	}

	void ExecuteA(const _TyData& data)
	{
		std::shared_lock<std::shared_mutex> lock(m_handler_mutex);
		for (const auto& handler : m_handler)
		{
			if (nullptr != handler)
			{
				if constexpr (bAsyn)
				{
					try
					{
						handler(data);
					}
					catch (...)
					{
						// Keep draining subsequent events after a failed callback.
					}
				}
				else
				{
					handler(data);
				}
			}
		}
	}

private:
	_TyDataContainer m_cache;

	std::shared_mutex m_handler_mutex;
	std::vector<_TyHandler> m_handler;

	std::mutex m_tasks_mutex;
	std::shared_future<void> m_task;
	bool m_scheduled{ false };
	bool m_stopping{ false };
};

#endif
