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
	BuildNewThread(m_cores);
}

CThreadPool::CThreadPool(std::size_t cores, std::size_t assistants)
{
	SetWorkersCount(cores, assistants);
	BuildNewThread(m_cores);
}

CThreadPool::CThreadPool(pool_type ty)
{
	SetWorkersCount(CalcBestPerformanceThreads(ty), (m_cores - 1) / 2);
	BuildNewThread(m_cores);
}

CThreadPool::~CThreadPool()
{
	ShutDown();
}

void CThreadPool::SetWorkersCount(std::size_t nCores, std::size_t nAssistants)
{
	m_cores = nCores;
	m_assistants = nAssistants;
	m_max_workers = m_cores + m_assistants;
}

void CThreadPool::ShutDown()
{
	{
		std::unique_lock<std::mutex> lck(m_tasks_mutex);
		if (m_stop)
		{
			return;
		}
		m_stop = true;
		m_thread_task_queues.clear();
	}
	m_task_ready.notify_all();

	{
		std::unique_lock<std::mutex> lck(m_workers_mutex);
		for (auto& w : m_workers)
		{
			if (w.joinable())
			{
				w.join();
			}
		}
		m_workers.clear();
	}
	m_worker_count.store(0);
	m_tasks.store(0);
	m_busy_workers.store(0);
}

void CThreadPool::ThreadProc()
{
	thread_local size_t tid = m_thread_id.fetch_add(1, std::memory_order_relaxed);
	while (true)
	{
		std::function<void()> task{ nullptr };
		{
			std::unique_lock<std::mutex> lck(m_tasks_mutex);
			m_task_ready.wait(lck, [this]
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
				--m_tasks;
			}
			else if (!m_task_queue.empty())
			{
				task = std::move(m_task_queue.top().m_task);
				m_task_queue.pop();
				--m_tasks;
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
	if (m_assistants <= 0)
	{
		return false;
	}
	if (m_busy_workers < m_cores) // 忙碌线程未到最大
	{
		return false;
	}
	if (m_worker_count >= m_max_workers)
	{
		return false;
	}
	std::size_t nTasks = m_tasks.load();
	std::size_t nWorkers = m_worker_count.load();
	return (nTasks > (nWorkers * 2.5)) && (nTasks >= 20);
}

void CThreadPool::BuildNewThread(std::size_t threads)
{
	std::unique_lock<std::mutex> lck(m_workers_mutex);
	if (m_worker_count >= m_max_workers)
	{
		return;
	}
	for (std::size_t i = 0; i < threads; ++i)
	{
		m_workers.emplace_back(&CThreadPool::ThreadProc, this);
		if (m_workers.size() >= m_max_workers)
		{
			break;
		}
	}
	std::size_t sz = m_workers.size();
	m_worker_count.store(sz);
}