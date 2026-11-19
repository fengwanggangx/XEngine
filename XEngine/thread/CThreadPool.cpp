#include "CThreadPool.h"
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)
#include <windows.h>
#else
#include <unistd.h>
#include <sys/sysinfo.h>
#endif

std::size_t GetSystemCPUCount()
{
	std::size_t s_cores = std::thread::hardware_concurrency();
	if (s_cores <= 0)
	{
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)
		SYSTEM_INFO systemInfo;
		GetSystemInfo(&systemInfo);
		s_cores = systemInfo.dwNumberOfProcessors;
#else
		s_cores = get_nprocs_conf();
#endif
	}
	return s_cores;
}

std::size_t CalcBestPerformanceThreads(pool_type mode)
{
	// CPU密集型:CPU数 + 1
	// IO密集型:CPU数 * 2
	// CPU数 * (1 + 线程等待时间/线程运行时间)
	std::size_t nCpus = GetSystemCPUCount();

	if (mode == pool_type::em_more_calc)
	{
		return (nCpus + 1);
	}
	else if (mode == pool_type::em_more_io)
	{
		return (2 * nCpus);
	}
	else if (mode == pool_type::em_more_complex)
	{
		return int(1.5 * nCpus + 1);
	}
	return nCpus;
}

CThreadPool::CThreadPool(std::size_t cores)
{
	SetWorkersCount(cores, 0);
	BuildNewThread(m_nCores);
}

CThreadPool::CThreadPool(std::size_t cores, std::size_t assistants)
{
	SetWorkersCount(cores, assistants);
	BuildNewThread(m_nCores);
}

CThreadPool::CThreadPool(pool_type ty)
{
	SetWorkersCount(CalcBestPerformanceThreads(ty), (m_nCores - 1) / 2);
	BuildNewThread(m_nCores);
}

CThreadPool::~CThreadPool()
{
	ShutDown();
}

void CThreadPool::SetWorkersCount(std::size_t nCores, std::size_t nAssistants)
{
	m_nCores = nCores;
	m_nAssistants = nAssistants;
	m_n_max_workers = m_nCores + m_nAssistants;
}

void CThreadPool::ShutDown()
{
	{
		std::unique_lock<std::mutex> lck(m_mtx_tasks);
		if (m_stop)
		{
			return;
		}
		m_stop = true;
		m_thread_task_queues.clear();
	}
	m_cond.notify_all();

	{
		std::unique_lock<std::mutex> lck(m_mtx_workers);
		for (auto& w : m_workers)
		{
			if (w.joinable())
			{
				w.join();
			}
		}
		m_workers.clear();
	}
	m_nWorkers.store(0);
	m_nTasks.store(0);
	m_busy_workers.store(0);
}

void CThreadPool::ThreadProc()
{
	thread_local size_t tid = m_thread_id.fetch_add(1, std::memory_order_relaxed);
	while (true)
	{
		std::function<void()> task{ nullptr };
		{
			std::unique_lock<std::mutex> lck(m_mtx_tasks);
			m_cond.wait(lck, [this]
			{
				return m_stop || !m_task_queue.empty() || ((tid < m_thread_task_queues.size()) && !m_thread_task_queues.at(tid).empty());
			});

			if (m_stop && (m_task_queue.empty() && ((tid >= m_thread_task_queues.size()) || m_thread_task_queues.at(tid).empty())))
			{
				return;
			}

			if (tid < m_thread_task_queues.size() && !m_thread_task_queues.at(tid).empty())
			{
				auto& queue = m_thread_task_queues.at(tid);
				task = std::move(queue.top().m_task);
				queue.pop();
				--m_nTasks;
			}
			else if (!m_task_queue.empty())
			{
				task = std::move(m_task_queue.top().m_task);
				m_task_queue.pop();
				--m_nTasks;
			}
		}
		if (nullptr != task)
		{
			++m_busy_workers;
			try
			{
				task();
			}
			catch (const std::exception&)
			{
			}
			--m_busy_workers;
		}
	}
}

bool CThreadPool::IsNeedAssistant()
{
	if (m_nAssistants <= 0)
	{
		return false;
	}
	if (m_busy_workers < m_nCores) // 忙碌线程未到最大
	{
		return false;
	}
	if (m_nWorkers >= m_n_max_workers)
	{
		return false;
	}
	std::size_t nTasks = m_nTasks.load();
	std::size_t nWorkers = m_nWorkers.load();
	return (nTasks > (nWorkers * 2.5)) && (nTasks >= 20);
}

void CThreadPool::BuildNewThread(std::size_t threads)
{
	std::unique_lock<std::mutex> lck(m_mtx_workers);
	if (m_nWorkers >= m_n_max_workers)
	{
		return;
	}
	for (std::size_t i = 0; i < threads; ++i)
	{
		m_workers.emplace_back(&CThreadPool::ThreadProc, this);
		if (m_workers.size() >= m_n_max_workers)
		{
			break;
		}
	}
	std::size_t sz = m_workers.size();
	m_nWorkers.store(sz);
}