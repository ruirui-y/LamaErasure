#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <QObject>
#include <QMutex>
#include <QVector>
#include <memory>
#include "singletion.h"

class WorkerThread;
class ThreadPool  : public QObject, public Singleton<ThreadPool>
{
	Q_OBJECT

	friend class Singleton<ThreadPool>;
public:
	ThreadPool(QObject *parent = 0);
	~ThreadPool();

	void Start(size_t threadNum);
	void Stop();
	QSharedPointer<WorkerThread> GetThread();

private:
	size_t threadNum_ = 0;
	size_t currentThreadIndex_ = 0;
	QMutex mutex_;
	bool started_ = false;
	QVector<QSharedPointer<WorkerThread>> threads_;
};

#endif // THREADPOOL_H