#ifndef SINGLETON_H
#define SINGLETON_H

#include <mutex>
#include <memory>
#include <iostream>

template<typename T>
class Singleton
{
protected:
	Singleton<T>() = default;
	Singleton(const Singleton<T>& ) = delete;
	Singleton<T>& operator=(const Singleton<T>& ) = delete;

	static std::shared_ptr<T> m_pInstance;

public:
	static std::shared_ptr<T> Instance()
	{
		static std::once_flag flag;
		std::call_once(flag, []() {
			try {
				m_pInstance = std::shared_ptr<T>(new T);
			}
			catch (...) {
				// 处理异常，这里可以记录日志等
				std::cerr << "Failed to create singleton instance." << std::endl;
			}
			});
		return m_pInstance;
	}

	void PrintAddress()
	{
		std::cout << "Singleton<T> address: " << m_pInstance.get() << std::endl;
	}

	~Singleton<T>()
	{
		std::cout << "Singleton<T> is destroyed" << std::endl;
	}
};

template<typename T>
std::shared_ptr<T> Singleton<T>::m_pInstance = nullptr;

#endif // SINGLETON_H