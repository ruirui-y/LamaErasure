#ifndef WORKERTHREAD_H
#define WORKERTHREAD_H

#include <QThread>
#include <QSharedPointer>
#include <QSemaphore>
#include <QMetaObject>
#include <atomic>

class WorkerThread : public QThread
{
    Q_OBJECT
public:
    explicit WorkerThread(QObject* parent = nullptr) : QThread(parent) {}
    ~WorkerThread() override {}

    void WaitReady() { ready_.acquire(); }

    QObject* Dispatcher() const { return dispatcher_.load(std::memory_order_acquire); }

    // 在该 Worker 线程创建 QObject，并返回 QSharedPointer（析构走 deleteLater）
    template<class T, class... Args>
    QSharedPointer<T> CreateQObject(Args&&... args)
    {
        static_assert(std::is_base_of_v<QObject, T>, "T must derive from QObject");

        QObject* d = Dispatcher();
        if (!d) return {};

        // 线程内调用：直接 new，避免 Blocking 自死锁
        if (QThread::currentThread() == d->thread())
        {
            T* raw = new T(std::forward<Args>(args)...);
            return QSharedPointer<T>(raw, &QObject::deleteLater);
        }

        QSharedPointer<T> out;
        QMetaObject::invokeMethod(d, [&]()
            {
                T* raw = new T(std::forward<Args>(args)...);
                out = QSharedPointer<T>(raw, &QObject::deleteLater);
            }, Qt::BlockingQueuedConnection);

        return out;
    }

protected:
    void run() override;

signals:
    void SigReady();

private:
    std::atomic<QObject*> dispatcher_{ nullptr };
    QSemaphore ready_{ 0 };
};

#endif // WORKERTHREAD_H