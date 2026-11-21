#include "runtime/resource/resource_read.hpp"
#include "runtime/performance/profiling.hpp"

#include "runtime/resource/resource_read_state.hpp"

#include <utility>

namespace engine
{

ResourceRead::ResourceRead(std::shared_ptr<State> state) noexcept : state_(std::move(state))
{
    ENGINE_PROFILE_FUNCTION();
}

ResourceRead::~ResourceRead() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    (void)cancel();
}

ResourceReadStatus ResourceRead::status() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return state_->status;
}

bool ResourceRead::take(std::unique_ptr<Resource>& output)
{
    ENGINE_PROFILE_FUNCTION();
    if (output || state_->status != ResourceReadStatus::ready)
    {
        return false;
    }
    output = std::move(state_->resource);
    state_->status = ResourceReadStatus::delivered;
    return true;
}

bool ResourceRead::cancel() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return state_->cancel();
}

ResourceError ResourceRead::error() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return state_->error;
}

std::string_view ResourceRead::last_error() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (state_->error == ResourceError::cancelled)
    {
        return "Resource read cancelled";
    }
    return state_->message;
}

} // namespace engine
