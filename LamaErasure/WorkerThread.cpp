#include "WorkerThread.h"
#include <QDebug>

void WorkerThread::run()
{
    QObject dispatcher;
    dispatcher_.store(&dispatcher, std::memory_order_release);

    ready_.release();
    emit SigReady();

    exec();

    // 退出事件循环后 dispatcher 即将销毁
    dispatcher_.store(nullptr, std::memory_order_release);
}