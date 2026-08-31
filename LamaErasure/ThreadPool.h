#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <QObject>
#include <QMutex>
#include <QVector>
#include <memory>
#include "singletion.h"
#include "Global.h"
#include "WorkerThread.h"
#include <type_traits>

class ThreadPool : public QObject, public Singleton<ThreadPool>
{
	Q_OBJECT

		friend class Singleton<ThreadPool>;
public:
	ThreadPool(QObject* parent = 0);
	~ThreadPool();

	void Start(size_t threadNum);
	void Stop();
	QSharedPointer<WorkerThread> GetThread();

public:
	// 终极泛型分发引擎：自动处理线程获取、生命周期保持与跨线程跳跃
	template<typename TaskFunc>
	void DispatchToWorker(TaskFunc&& task)
	{
		// 1. 获取线程智能指针（必须按值捕获以延长生命周期）
		auto thread = GetThread();
		if (!thread) return;

		// 2. 获取目标子线程的锚点
		QObject* dispatcher = thread->Dispatcher();
		if (!dispatcher) return;

		// 3. 跨线程时空跳跃，并在子线程中解包执行真正的任务
		QMetaObject::invokeMethod(dispatcher, [thread, func = std::forward<TaskFunc>(task)]() mutable
			{
				// 编译期分支判断
				// 判断 func 是否可以接受 WorkerThread* 类型的参数
				if constexpr (std::is_invocable_v<TaskFunc, WorkerThread*>)
				{
					func(thread.data());																	// 如果可以，就把线程指针传进去（适合需要获取线程上下文的任务）
				}
				else
				{
					func();																					// 如果不可以，就直接执行（适合像 TCPMgr::Instance() 这种任务）
				}
			}, Qt::QueuedConnection);
	}

private:
	size_t threadNum_ = 0;
	int currentThreadIndex_ = 0;
	QMutex mutex_;
	bool started_ = false;
	QVector<QSharedPointer<WorkerThread>> threads_;
};

#endif // THREADPOOL_H