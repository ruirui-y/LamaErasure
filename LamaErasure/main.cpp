#include "MainWindow.h"
#include "ThreadPool.h"
#include "Global.h"
#include <QThread>
#include <QtWidgets/QApplication>

void RegisterMetaTypes()
{
    qRegisterMetaType<ImageCallback>("ImageCallback");
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.thread()->setObjectName("main");
    RegisterMetaTypes();
    ThreadPool::Instance()->Start(4);
    MainWindow window;
    window.show();
    return app.exec();
}