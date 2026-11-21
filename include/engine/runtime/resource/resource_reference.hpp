#pragma once

#include "runtime/performance/function_profile.hpp"

#include "runtime/resource/resource_loader.hpp"

#include <utility>

namespace engine
{
// 独占一次资源获取。只能显式 retain，禁止复制；清理在协调线程执行。
// 加载器借用必须覆盖此对象及其保活的数据使用。
class ResourceReference final
{
public:
    ResourceReference() noexcept = default;
    ~ResourceReference() noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        reset();
    }
    ResourceReference(const ResourceReference&) = delete;
    ResourceReference& operator=(const ResourceReference&) = delete;
    ResourceReference(ResourceReference&& other) noexcept
        : loader_(std::exchange(other.loader_, nullptr)), handle_(std::exchange(other.handle_, {}))
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
    }
    ResourceReference& operator=(ResourceReference&& other) noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        if (this != &other)
        {
            reset();
            loader_ = std::exchange(other.loader_, nullptr);
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }
    [[nodiscard]] bool acquire(ResourceLoader* loader, std::string_view path_or_guid)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        if (loader_ || !loader || !loader->acquire_loaded(path_or_guid, handle_))
        {
            return false;
        }
        loader_ = loader;
        return true;
    }
    [[nodiscard]] bool retain(ResourceLoader* loader, const ResourceHandle& source)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        if (loader_ || !loader || !loader->retain(source, handle_))
        {
            return false;
        }
        loader_ = loader;
        return true;
    }
    [[nodiscard]] bool retain(const ResourceReference& source)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return retain(source.loader_, source.handle_);
    }
    [[nodiscard]] bool dependency(ResourceLoader* loader, const ResourceHandle& owner,
                                  std::string_view path_or_guid)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        if (loader_ || !loader || !loader->retain_dependency(owner, path_or_guid, handle_))
        {
            return false;
        }
        loader_ = loader;
        return true;
    }
    [[nodiscard]] bool adopt(ResourceLoader* loader, ResourceHandle& handle)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        const Resource* resource = nullptr;
        if (loader_ || !loader || loader->get(handle, resource) != ResourceError::none)
        {
            return false;
        }
        loader_ = loader;
        handle_ = std::exchange(handle, {});
        return true;
    }
    void reset() noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        if (loader_)
        {
            (void)loader_->release(handle_);
        }
        handle_ = {};
        loader_ = nullptr;
    }
    [[nodiscard]] ResourceHandle detach() noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        loader_ = nullptr;
        return std::exchange(handle_, {});
    }
    [[nodiscard]] ResourceHandle handle() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return handle_;
    }
    [[nodiscard]] ResourceLoader* loader() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return loader_;
    }
    [[nodiscard]] const Resource* get() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        const Resource* value = nullptr;
        if (loader_)
        {
            (void)loader_->get(handle_, value);
        }
        return value;
    }
    template <typename T> [[nodiscard]] const typename T::Data* data() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        const T* value = nullptr;
        return loader_ && loader_->get(handle_, value) == ResourceError::none ? &value->data()
                                                                              : nullptr;
    }
    explicit operator bool() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return get() != nullptr;
    }
    [[nodiscard]] const Resource* operator->() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return get();
    }
    [[nodiscard]] const Resource& operator*() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return *get();
    }

private:
    ResourceLoader* loader_ = nullptr;
    ResourceHandle handle_;
};
} // namespace engine
