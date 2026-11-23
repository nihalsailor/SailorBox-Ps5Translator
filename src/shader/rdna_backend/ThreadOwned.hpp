#ifndef CORE_SHADER_RECOMPILER_THREADOWNED_HPP
#define CORE_SHADER_RECOMPILER_THREADOWNED_HPP

#include <memory>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace ShaderRecompiler {

namespace ThreadOwnedDetail {

struct Owned {
    void* object;
    void (*destroy)(void*);
};

inline void DestroyAll(void* value) {
    auto* owned = static_cast<std::vector<Owned>*>(value);
    for (auto it = owned->rbegin(); it != owned->rend(); ++it) it->destroy(it->object);
    delete owned;
}

#ifdef _WIN32
inline DWORD Key() {
    static const DWORD slot = FlsAlloc(+[](void* value) {
        if (value) DestroyAll(value);
    });
    if (slot == FLS_OUT_OF_INDEXES) throw std::runtime_error("ThreadOwned: FlsAlloc failed");
    return slot;
}

inline void DestroyAtThreadExit(void* object, void (*destroy)(void*)) {
    auto* owned = static_cast<std::vector<Owned>*>(FlsGetValue(Key()));
    if (owned == nullptr) {
        owned = new std::vector<Owned>();
        if (!FlsSetValue(Key(), owned)) {
            delete owned;
            throw std::runtime_error("ThreadOwned: FlsSetValue failed");
        }
    }
    owned->push_back({object, destroy});
}
#else
inline pthread_key_t Key() {
    static const pthread_key_t key = [] {
        pthread_key_t created{};
        if (pthread_key_create(&created, DestroyAll) != 0) throw std::runtime_error("ThreadOwned: pthread_key_create failed");
        return created;
    }();
    return key;
}

inline void DestroyAtThreadExit(void* object, void (*destroy)(void*)) {
    auto* owned = static_cast<std::vector<Owned>*>(pthread_getspecific(Key()));
    if (owned == nullptr) {
        owned = new std::vector<Owned>();
        if (pthread_setspecific(Key(), owned) != 0) {
            delete owned;
            throw std::runtime_error("ThreadOwned: pthread_setspecific failed");
        }
    }
    owned->push_back({object, destroy});
}
#endif

}

template<typename T>
T& ThreadOwned(T*& slot) {
    if (slot == nullptr) {
        auto object = std::make_unique<T>();
        ThreadOwnedDetail::DestroyAtThreadExit(object.get(), [](void* value) { delete static_cast<T*>(value); });
        slot = object.release();
    }
    return *slot;
}

}

#endif
