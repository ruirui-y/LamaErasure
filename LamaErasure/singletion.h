#ifndef SINGLETON_H
#define SINGLETON_H

#include <mutex>
#include <memory>
#include <iostream>
#include <type_traits>

// C++17 环境嗅探魔法：自动检测当前工程是否有 Qt 环境
#if defined(__has_include)
#if __has_include(<QObject>) && __has_include(<QThread>)
#define ENV_HAS_QT
#include <QObject>
#include <QThread>
#endif
#endif

template<typename T>
class Singleton
{
protected:
    Singleton<T>() = default;
    Singleton(const Singleton<T>&) = delete;
    Singleton<T>& operator=(const Singleton<T>&) = delete;

    static std::shared_ptr<T> m_pInstance;

public:
    static std::shared_ptr<T> Instance()
    {
        static std::once_flag flag;
        std::call_once(flag, []()
            {
                try {
                    // ==============================================================================
                    // 只有在侦察到 Qt 环境时，编译器才会看这段代码
#ifdef ENV_HAS_QT
                // 编译期魔法：如果在 Qt 环境下，且 T 继承自 QObject
                    if constexpr (std::is_base_of_v<QObject, T>) {
                        // 为 QObject 定制的跨线程删除器
                        auto safeDeleter = [](T* obj) {
                            if (!obj) return;
                            QThread* objThread = obj->thread();
                            // 线程死了就常规 delete，线程活着就安全投递 deleteLater
                            if (!objThread || !objThread->isRunning() || objThread->isFinished()) {
                                delete obj;
                            }
                            else {
                                obj->deleteLater();
                            }
                            };

                        m_pInstance = std::shared_ptr<T>(new T, safeDeleter);
                    }
                    else
#endif
                        // ==============================================================================
                    {
                        // 纯 C++ 类的默认处理路径（无论有没有 Qt，普通类都走这里）
                        m_pInstance = std::shared_ptr<T>(new T);
                    }
                }
                catch (...) {
                    std::cerr << "Failed to create singleton instance." << std::endl;
                }
            });
        return m_pInstance;
    }

    void PrintAddress()
    {
        std::cout << "Singleton<T> address: " << m_pInstance.get() << std::endl;
    }

    virtual ~Singleton<T>()
    {
        std::cout << "Singleton<T> is destroyed" << std::endl;
    }
};

template<typename T>
std::shared_ptr<T> Singleton<T>::m_pInstance = nullptr;

#endif // SINGLETON_H