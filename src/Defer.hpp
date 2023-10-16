/*
 * (C) 2021 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */
#ifndef __WARABI_DEFER_HPP
#define __WARABI_DEFER_HPP

namespace warabi {

template<typename F>
class deferred {

    public:

    explicit deferred(F&& function)
    : m_function(std::forward<F>(function)) {}

    ~deferred() {
        m_function();
    }

    private:

    F m_function;
};

template<typename F>
inline auto defer(F&& f) {
    return deferred<F>(std::forward<F>(f));
}

#define _UTILITY_DEFERRED_LINENAME_CAT(name, line) name##line
#define _UTILITY_DEFERRED_LINENAME(name, line) _UTILITY_DEFERRED_LINENAME_CAT(name, line)
#define DEFER(f) \
    const auto& _UTILITY_DEFERRED_LINENAME(EXIT, __LINE__) = ::warabi::defer([&]() { f; }); (void)_UTILITY_DEFERRED_LINENAME(EXIT, __LINE__)

}

#endif
