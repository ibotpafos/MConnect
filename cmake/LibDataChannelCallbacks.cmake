# Keep the pinned checkout pristine. The implicit derived assignment used by
# callback=nullptr copied its optional replay state outside the base mutex.
set(MCONNECT_RTC_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/libdatachannel/include")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/external/libdatachannel/include/"
    DESTINATION "${MCONNECT_RTC_INCLUDE_DIR}" PATTERN "utils.hpp" EXCLUDE)
set(MCONNECT_RTC_UTILS_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/libdatachannel/include/rtc/utils.hpp")
file(READ "${MCONNECT_RTC_UTILS_PATH}" MCONNECT_RTC_UTILS)
set(MCONNECT_RTC_CALLBACK_OLD [=[
	~synchronized_stored_callback() {}

private:
]=])
set(MCONNECT_RTC_CALLBACK_NEW [=[
	~synchronized_stored_callback() {}

	synchronized_stored_callback &operator=(decltype(nullptr)) {
		std::lock_guard lock(this->mutex);
		this->callback = nullptr;
		stored.reset();
		return *this;
	}

	template <typename F, std::enable_if_t<!std::is_same_v<std::decay_t<F>, synchronized_stored_callback>, int> = 0>
	synchronized_stored_callback &operator=(F &&func) {
		synchronized_callback<Args...>::operator=(std::function<void(Args...)>(std::forward<F>(func)));
		return *this;
	}

private:
]=])
string(FIND "${MCONNECT_RTC_UTILS}" "${MCONNECT_RTC_CALLBACK_OLD}" MCONNECT_RTC_CALLBACK_OFFSET)
if(MCONNECT_RTC_CALLBACK_OFFSET EQUAL -1)
    message(FATAL_ERROR "Pinned libdatachannel callback helper changed; review the synchronization patch")
endif()
string(REPLACE "${MCONNECT_RTC_CALLBACK_OLD}" "${MCONNECT_RTC_CALLBACK_NEW}" MCONNECT_RTC_UTILS "${MCONNECT_RTC_UTILS}")
string(REPLACE "#include <tuple>" "#include <tuple>\n#include <type_traits>" MCONNECT_RTC_UTILS "${MCONNECT_RTC_UTILS}")
file(GENERATE OUTPUT "${MCONNECT_RTC_INCLUDE_DIR}/rtc/utils.hpp" CONTENT "${MCONNECT_RTC_UTILS}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MCONNECT_RTC_UTILS_PATH}")
target_include_directories(datachannel-static BEFORE PUBLIC "${MCONNECT_RTC_INCLUDE_DIR}")
target_include_directories(datachannel-static BEFORE PRIVATE "${MCONNECT_RTC_INCLUDE_DIR}/rtc")
