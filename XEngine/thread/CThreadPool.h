#pragma once
#include <cstdint>
#include <iostream>
#include <queue>
#include <thread>
#include <functional>
#include <future>
#include <mutex>

enum class task_priority
{
	em_normal = 0, // 一般任务
	em_middle,	   // 居中
	em_high		   // 高优先级任务
};

enum class pool_type
{
	em_more_calc = 0, // CPU密集型，大量数据处理计算
	em_more_io,		  // IO密集型，IO等待
	em_more_complex	  // 混合任务
};

enum class abort_policy
{
	em_callerrunspolicy = 0, // 调用者线程执行任务
	em_discardoldestpolicy,	 // 丢弃原队列中队尾任务，换成新的
	em_discardpolicy		 // 丢弃任务
};

class Task
{
public:
	friend struct TaskComparator;
	friend class CThreadPool;

public:
	Task() = default;

	template <typename _Fx>
	Task(task_priority level, size_t priority, std::uint64_t sequence, _Fx&& func) : m_level(level), m_priority(priority), m_sequence(sequence)
	{
		m_task = [f = std::forward<_Fx>(func)]() mutable
		{
			(*f)();
		};
	}

	~Task()
	{
		m_task = nullptr;
	}

private:
	task_priority m_level{ task_priority::em_normal };
	size_t m_priority{ 0 };
	std::uint64_t m_sequence{ 0 };
	std::function<void()> m_task{ nullptr };
};

struct TaskComparator
{
	// 优先级高，先出栈
	bool operator()(const Task& p1, const Task& p2)
	{
		if (p1.m_level != p2.m_level)
		{
			return p1.m_level < p2.m_level;
		}
		if (p1.m_priority != p2.m_priority)
		{
			return p1.m_priority < p2.m_priority;
		}
		return p1.m_sequence > p2.m_sequence;
	}
};

class CThreadPool final
{
public:
	explicit CThreadPool(std::size_t cores);
	explicit CThreadPool(pool_type ty);
	explicit CThreadPool(std::size_t cores, std::size_t assistants);
	~CThreadPool();

public:
	template <class _Fx, class... _Args>
	auto PushTask(task_priority level, std::size_t priority, _Fx&& f, _Args&&... args) // ->  std::future<typename std::result_of<_Fx(_Args...)>::type>
	{
		using _TyReturn = typename std::invoke_result_t<_Fx, _Args...>;
		std::shared_ptr<std::packaged_task<_TyReturn()>> task = std::make_shared<std::packaged_task<_TyReturn()>>(
			[f = std::forward<_Fx>(f), args = std::make_tuple(std::forward<_Args>(args)...)]() mutable
		{
			return std::apply(std::move(f), std::move(args));
		});
		std::future<_TyReturn> ret = task->get_future();
		RegisterTask(level, priority, std::move(task));
		return ret;
	}

	template <class _Fx, class... _Args>
	auto PushTask(std::size_t idx, task_priority level, std::size_t priority, _Fx&& f, _Args&&... args)
	{
		using _TyReturn = typename std::invoke_result_t<_Fx, _Args...>;
		std::shared_ptr<std::packaged_task<_TyReturn()>> task = std::make_shared<std::packaged_task<_TyReturn()>>(
			[f = std::forward<_Fx>(f), args = std::make_tuple(std::forward<_Args>(args)...)]() mutable
		{
			return std::apply(std::move(f), std::move(args));
		});
		std::future<_TyReturn> ret = task->get_future();
		RegisterTask(idx, level, priority, std::move(task));
		return ret;
	}

	void ShutDown();

private:
	template <typename _Task>
	bool RegisterTask(task_priority level, size_t priority, _Task&& task)
	{
		{
			std::unique_lock<std::mutex> lck(m_tasks_mutex);
			if (m_stop)
			{
				return false;
			}
			m_task_queue.emplace(level, priority, m_nextSequence++, std::forward<_Task>(task));
			++m_tasks;
		}

		m_task_ready.notify_one();
		if ((m_assistants > 0) && IsNeedAssistant()) // 增加辅助线程
		{
			BuildNewThread(1);
		}
		return true;
	}

	template <typename _Task>
	bool RegisterTask(std::size_t idx, task_priority level, size_t priority, _Task&& task)
	{
		if (idx >= m_worker_count)
		{
			return RegisterTask(level, priority, std::forward<_Task>(task));
		}
		{
			std::unique_lock<std::mutex> lck(m_tasks_mutex);
			if (m_stop)
			{
				return false;
			}
			while (idx >= m_thread_task_queues.size())
			{
				m_thread_task_queues.emplace_back();
			}
			m_thread_task_queues.at(idx).emplace(level, priority, m_nextSequence++, std::forward<_Task>(task));
			++m_tasks;
		}

		m_task_ready.notify_all(); // 会有小规模惊群唤醒问题，线程数不多影响不大
		return true;
	}

private:
	void ThreadProc();
	bool IsNeedAssistant();
	void BuildNewThread(std::size_t threads);
	void SetWorkersCount(std::size_t nCores, std::size_t nAssistants);

private:
	using _TyTaskQueue = std::priority_queue<Task, std::vector<Task>, TaskComparator>;

	// 公共任务队列
	_TyTaskQueue m_task_queue;

	// 指定线程任务队列,主要用于时序任务
	std::vector<_TyTaskQueue> m_thread_task_queues;

	std::uint64_t m_nextSequence{ 0 };
	std::mutex m_tasks_mutex;
	std::condition_variable m_task_ready;

	std::atomic_size_t m_thread_id{ 0 };

	std::mutex m_workers_mutex;
	std::vector<std::thread> m_workers;

private:
	std::atomic_size_t m_worker_count{ 0 }; // 当前线程数
	std::atomic_size_t m_tasks{ 0 };		// 当前任务数
	std::atomic_size_t m_busy_workers{ 0 }; // 忙碌线程数
	std::atomic_bool m_stop{ false };					// 线程退出

private:
	std::size_t m_cores{ 0 };		// 核心线程数
	std::size_t m_assistants{ 0 };	// 辅助线程数
	std::size_t m_max_workers{ 0 }; // 最大线程数
};
