cmake_minimum_required(VERSION 3.22)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
project(MConnect VERSION 0.1.3 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()
option(MCONNECT_BUILD_TESTS "Build integration harness and core verification" ON)
set(MCONNECT_UPDATE_REPOSITORY "ibotpafos/MConnect" CACHE STRING "Public GitHub repository for stable releases")
if(NOT MCONNECT_UPDATE_REPOSITORY MATCHES "^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
    message(FATAL_ERROR "MCONNECT_UPDATE_REPOSITORY must be owner/repository")
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_SKIP_INSTALL_RULES ON CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(JUCE_COPY_PLUGIN_AFTER_BUILD OFF CACHE BOOL "" FORCE)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE/CMakeLists.txt")
    message(FATAL_ERROR "Fetch pinned sources first: python3 scripts/bootstrap.py")
endif()
add_subdirectory(external/JUCE)

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mbedtls_mconnect_config.h" CACHE FILEPATH "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory(external/mbedtls EXCLUDE_FROM_ALL)
add_library(MbedTLS::MbedTLS ALIAS mbedtls)
add_library(MbedTLS::X509 ALIAS mbedx509)
add_library(MbedTLS::Crypto ALIAS mbedcrypto)

set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(external/opus EXCLUDE_FROM_ALL)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_MEDIA OFF CACHE BOOL "" FORCE)
set(USE_GNUTLS OFF CACHE BOOL "" FORCE)
set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(USE_NICE OFF CACHE BOOL "" FORCE)
set(PREFER_SYSTEM_LIB OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_DEPS_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
set(ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(ENABLE_NSS OFF CACHE BOOL "" FORCE)
set(OPENSSL OFF CACHE BOOL "" FORCE)
set(MBEDTLS OFF CACHE BOOL "" FORCE)
set(NSS OFF CACHE BOOL "" FORCE)
set(GCM OFF CACHE BOOL "" FORCE)
set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)
set(ENABLE_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/libdatachannel EXCLUDE_FROM_ALL)
include(cmake/LibDataChannelCallbacks.cmake)
set(LIBSAMPLERATE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBSAMPLERATE_INSTALL OFF CACHE BOOL "" FORCE)
set(LIBSAMPLERATE_ENABLE_SINC_BEST_CONVERTER OFF CACHE BOOL "" FORCE)
set(LIBSAMPLERATE_ENABLE_SINC_FAST_CONVERTER OFF CACHE BOOL "" FORCE)
set(LIBSAMPLERATE_ENABLE_SINC_MEDIUM_CONVERTER ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(external/libsamplerate EXCLUDE_FROM_ALL)

# CivetWeb 1.16's atomic-stop option still reads *f non-atomically and retries
# after a successful store. Patch only that helper in a generated build copy;
# the pinned upstream checkout stays intact and the fix is reproducible.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/external/civetweb/src/civetweb.c" MCONNECT_CIVET_SOURCE)
set(MCONNECT_CIVET_STOP_OLD [=[
	stop_flag_t sf;
	do {
		sf = mg_atomic_compare_and_swap(f, *f, v);
	} while (sf != v);
]=])
set(MCONNECT_CIVET_STOP_NEW [=[
	stop_flag_t expected = mg_atomic_add(f, 0);
	for (;;) {
		stop_flag_t previous = mg_atomic_compare_and_swap(f, expected, v);
		if (previous == expected) {
			return;
		}
		expected = previous;
	}
]=])
string(FIND "${MCONNECT_CIVET_SOURCE}" "${MCONNECT_CIVET_STOP_OLD}" MCONNECT_CIVET_STOP_OFFSET)
if(MCONNECT_CIVET_STOP_OFFSET EQUAL -1)
    message(FATAL_ERROR "Pinned CivetWeb stop helper changed; review the atomic shutdown patch")
endif()
string(REPLACE "${MCONNECT_CIVET_STOP_OLD}" "${MCONNECT_CIVET_STOP_NEW}" MCONNECT_CIVET_SOURCE "${MCONNECT_CIVET_SOURCE}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/civetweb.c" CONTENT "${MCONNECT_CIVET_SOURCE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/external/civetweb/src/civetweb.c")
add_library(civetweb_static STATIC "${CMAKE_CURRENT_BINARY_DIR}/generated/civetweb.c")
target_include_directories(civetweb_static PUBLIC external/civetweb/include PRIVATE external/civetweb/src)
target_compile_definitions(civetweb_static PRIVATE USE_WEBSOCKET USE_IPV6 NO_CGI NO_FILES NO_CACHING USE_MBEDTLS USE_STACK_SIZE=102400 STOP_FLAG_NEEDS_LOCK)
target_link_libraries(civetweb_static PRIVATE MbedTLS::MbedTLS MbedTLS::X509 MbedTLS::Crypto)
if(WIN32)
    target_compile_definitions(civetweb_static PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(civetweb_static PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(civetweb_static PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()
add_library(qrcodegen STATIC external/qrcodegen/cpp/qrcodegen.cpp)
target_include_directories(qrcodegen PUBLIC external/qrcodegen/cpp)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(GLOB_RECURSE MCONNECT_SOURCE_FILES CONFIGURE_DEPENDS src/* assets/* docs/* scripts/* tests/* cmake/*)
list(FILTER MCONNECT_SOURCE_FILES EXCLUDE REGEX "/__pycache__/|\\.pyc$")
set(MCONNECT_SOURCE_ZIP "${CMAKE_CURRENT_BINARY_DIR}/generated/source.zip")
add_custom_command(OUTPUT "${MCONNECT_SOURCE_ZIP}"
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/scripts/source_archive.py" "${CMAKE_CURRENT_SOURCE_DIR}" "${MCONNECT_SOURCE_ZIP}"
    DEPENDS ${MCONNECT_SOURCE_FILES} CMakeLists.txt dependencies.lock.json LICENSE NOTICE.md README.md THIRD_PARTY_NOTICES.md LICENSES_THIRD_PARTY.md .gitignore .gitattributes CHANGELOG.md
    VERBATIM)
juce_add_binary_data(MConnectAssets HEADER_NAME BinaryData.h NAMESPACE MConnectBinaryData
    SOURCES assets/web/index.html assets/web/app.js assets/web/style.css LICENSE NOTICE.md "${MCONNECT_SOURCE_ZIP}")

set(MCONNECT_FORMATS VST3)
if(APPLE)
    list(APPEND MCONNECT_FORMATS AU)
elseif(UNIX)
    list(APPEND MCONNECT_FORMATS LV2)
endif()
juce_add_plugin(MConnect
    COMPANY_NAME "MConnect"
    BUNDLE_ID "audio.mconnect.plugin"
    IS_SYNTH FALSE NEEDS_MIDI_INPUT FALSE NEEDS_MIDI_OUTPUT FALSE IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE COPY_PLUGIN_AFTER_BUILD FALSE VST3_AUTO_MANIFEST TRUE
    VST3_CATEGORIES Fx Analyzer
    PLUGIN_MANUFACTURER_CODE Mcnn PLUGIN_CODE Mcnt
    FORMATS ${MCONNECT_FORMATS} PRODUCT_NAME "MConnect"
    LV2URI "https://mconnect.audio/plugin"
    LOCAL_NETWORK_PERMISSION_ENABLED TRUE
    LOCAL_NETWORK_PERMISSION_TEXT "MConnect streams your DAW audio to your phone on the local network."
    MICROPHONE_PERMISSION_ENABLED FALSE)
target_sources(MConnect PRIVATE src/PluginProcessor.cpp src/PluginEditor.cpp src/PluginState.cpp
    src/AudioTapFifo.cpp src/NetworkServer.cpp src/NetworkInterfaceUtils.cpp
    src/WebRtcAudioSender.cpp src/SelfSignedCertificate.cpp src/Resampler.cpp src/HtmlAssets.cpp
    src/PluginUpdates.cpp src/UpdateTransport.cpp)
if(APPLE)
    enable_language(OBJCXX)
    target_sources(MConnect PRIVATE src/UpdateTransport_mac.mm)
    set_source_files_properties(src/UpdateTransport_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()
target_include_directories(MConnect PUBLIC src external/JUCE/modules external/libsamplerate/include)
target_compile_definitions(MConnect PUBLIC MCONNECT_VERSION="${PROJECT_VERSION}" MCONNECT_UPDATE_REPOSITORY="${MCONNECT_UPDATE_REPOSITORY}" JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(MConnect PRIVATE MConnectAssets qrcodegen civetweb_static samplerate
    Opus::opus LibDataChannel::LibDataChannelStatic MbedTLS::MbedTLS MbedTLS::X509 MbedTLS::Crypto juce::juce_audio_processors juce::juce_cryptography
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
if(WIN32)
    target_compile_definitions(MConnect PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(MConnect PRIVATE ws2_32 iphlpapi crypt32 advapi32 user32 gdi32)
endif()
if(MCONNECT_BUILD_TESTS)
    enable_testing()
    juce_add_console_app(MConnectTests PRODUCT_NAME "MConnect integration tests")
    target_sources(MConnectTests PRIVATE tests/CoreTests.cpp)
    target_include_directories(MConnectTests PRIVATE src "${MCONNECT_RTC_INCLUDE_DIR}")
    target_link_libraries(MConnectTests PRIVATE MConnect)
    target_compile_definitions(MConnectTests PRIVATE JucePlugin_Build_Standalone=1)
    add_test(NAME core COMMAND MConnectTests)
    juce_add_console_app(MConnectUpdateTests PRODUCT_NAME "MConnect update tests")
    target_sources(MConnectUpdateTests PRIVATE tests/UpdateTests.cpp)
    target_include_directories(MConnectUpdateTests PRIVATE src)
    target_link_libraries(MConnectUpdateTests PRIVATE MConnect)
    target_compile_definitions(MConnectUpdateTests PRIVATE JucePlugin_Build_Standalone=1)
    add_test(NAME updates COMMAND MConnectUpdateTests)
    juce_add_gui_app(MConnectHarness PRODUCT_NAME "MConnect test host")
    target_sources(MConnectHarness PRIVATE tests/Harness.cpp)
    if(APPLE)
        enable_language(OBJCXX)
        set_source_files_properties(tests/Harness.cpp PROPERTIES LANGUAGE OBJCXX)
    endif()
    target_include_directories(MConnectHarness PRIVATE src)
    target_link_libraries(MConnectHarness PRIVATE MConnect)
    target_compile_definitions(MConnectHarness PRIVATE JucePlugin_Build_Standalone=1 JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)
    if(APPLE)
        add_library(MConnectBrowserTiming SHARED tests/MacosBrowserTiming.mm)
        target_link_libraries(MConnectBrowserTiming PRIVATE "-framework Foundation")
        add_custom_command(TARGET MConnectBrowserTiming POST_BUILD
            COMMAND codesign --force --sign - "$<TARGET_FILE:MConnectBrowserTiming>"
            VERBATIM)
    endif()
endif()
