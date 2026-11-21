#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <wasi/api.h>

#include <cmath>

extern "C" __wasi_errno_t __real___wasi_clock_time_get(__wasi_clockid_t clock,
                                                       __wasi_timestamp_t precision,
                                                       __wasi_timestamp_t* timestamp);

extern "C" __wasi_errno_t __wrap___wasi_clock_time_get(__wasi_clockid_t clock,
                                                       __wasi_timestamp_t precision,
                                                       __wasi_timestamp_t* timestamp)
{
    if (clock > __WASI_CLOCKID_THREAD_CPUTIME_ID)
    {
        return __WASI_ERRNO_INVAL;
    }

    // Use the pinned Emscripten time sources and keep the i64 timestamp inside Wasm.
    const double milliseconds =
        clock == __WASI_CLOCKID_REALTIME ? emscripten_date_now() : emscripten_get_now();
    constexpr double nanoseconds_per_millisecond = 1'000'000.0;
    constexpr double timestamp_limit = 0x1p64;
    const double nanoseconds = std::round(milliseconds * nanoseconds_per_millisecond);
    if (!(nanoseconds >= 0 && nanoseconds < timestamp_limit))
    {
        return __real___wasi_clock_time_get(clock, precision, timestamp);
    }
    *timestamp = static_cast<__wasi_timestamp_t>(nanoseconds);
    return __WASI_ERRNO_SUCCESS;
}
