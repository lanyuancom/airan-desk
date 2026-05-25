#ifndef QT_CALLBACK_UTIL_H
#define QT_CALLBACK_UTIL_H

#include <QPointer>
#include <utility>

template <typename T, typename... Args>
class WeakMemberCallback
{
public:
    using Method = void (T::*)(Args...);

    WeakMemberCallback(T *target, Method method)
        : m_target(target), m_method(method)
    {
    }

    void operator()(Args... args) const
    {
        T *target = m_target.data();
        if (target)
            (target->*m_method)(std::forward<Args>(args)...);
    }

private:
    QPointer<T> m_target;
    Method m_method = nullptr;
};

template <typename T, typename... Args>
WeakMemberCallback<T, Args...> makeWeakCallback(T *target, void (T::*method)(Args...))
{
    return WeakMemberCallback<T, Args...>(target, method);
}

#endif /* QT_CALLBACK_UTIL_H */
