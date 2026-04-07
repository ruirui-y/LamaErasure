#include "ThreadPool.h"
#include <QMutexLocker>
#include <QEventLoop>

ThreadPool::ThreadPool(QObject* parent)
	: QObject(parent)
{
}

ThreadPool::~ThreadPool()
{
	Stop();
}

void ThreadPool::Start(size_t threadNum)
{
	QMutexLocker locker(&mutex_);
	if (started_) return;
	if (threadNum == 0) threadNum = QThread::idealThreadCount();
	for (size_t i = 0; i < threadNum; ++i)
	{
		QSharedPointer<WorkerThread> thread = QSharedPointer<WorkerThread>(new WorkerThread());

		QEventLoop loop;
		QObject::connect(thread.get(), &WorkerThread::SigReady, &loop, &QEventLoop::quit, Qt::QueuedConnection);

		thread->start();
		thread->setObjectName(QString::number(i));
		loop.exec();																									// µÈµ½ worker run() Àï emit Ready()

		threads_.push_back(thread);
	}

	started_ = true;
}

void ThreadPool::Stop()
{
	QMutexLocker locker(&mutex_);
	if (!started_) return;
	for (auto& thread : threads_)
	{
		thread->quit();
		thread->wait();
	}
	threads_.clear();
	started_ = false;
}

QSharedPointer<WorkerThread> ThreadPool::GetThread()
{
	QMutexLocker locker(&mutex_);
	if (!started_) return nullptr;
	if (threads_.empty()) return nullptr;

	QSharedPointer<WorkerThread> thread = threads_[currentThreadIndex_];
	currentThreadIndex_ = (currentThreadIndex_ + 1) % threads_.size();

	return thread;
}