#pragma once
#include <juce_core/juce_core.h>
#include <functional>
#include <mutex>
#include <optional>

namespace mconnect
{
namespace updates
{
constexpr juce::int64 maximumMetadataBytes = 256 * 1024;
constexpr juce::int64 maximumInstallerBytes = 128 * 1024 * 1024;
constexpr juce::int64 checkIntervalMs = 24 * 60 * 60 * 1000;
constexpr juce::int64 retryIntervalMs = 30 * 60 * 1000;

struct Release
{
    juce::String version, url, sha256, filename;
    juce::int64 size = 0;
};

// Only stable three-component versions are accepted. No lexical comparison.
std::optional<int> compareVersions(const juce::String&, const juce::String&);
juce::Result parseRelease(const juce::String& json, const juce::String& repository,
                          const juce::String& currentVersion, std::optional<Release>&);
bool verifyInstaller(const juce::File&, const Release&);

struct Request
{
    juce::String url;
    juce::File destination; // Empty for bounded JSON, otherwise a private partial file.
    juce::int64 limit = maximumMetadataBytes;
};
struct Reply
{
    int status = 0;
    juce::String body, error;
};
using Cancelled = std::function<bool()>;
using Progress = std::function<void(juce::int64)>;
using Fetch = std::function<Reply(const Request&, const Cancelled&, const Progress&)>;
Reply fetch(const Request&, const Cancelled&, const Progress&);

enum class Phase { idle, checking, current, available, downloading, ready, failed };
struct Snapshot
{
    Phase phase = Phase::idle;
    std::optional<Release> release;
    juce::String error;
    juce::File installer;
    int percent = 0;
};
struct Configuration
{
    juce::String repository = MCONNECT_UPDATE_REPOSITORY;
    juce::String version = MCONNECT_VERSION;
    juce::File cache;
};

// Owned only by visible editors, shared between instances in this process.
// No processor, host parameter, audio callback, or message-thread network work.
class Service final : private juce::Thread
{
public:
    Service();
    explicit Service(Configuration, Fetch = fetch);
    ~Service() override;
    void check(bool manual = false);
    void download();
    Snapshot snapshot() const;
private:
    void run() override;
    void checkOnWorker();
    void downloadOnWorker(const Release&);
    void fail(const juce::String&);
    Configuration configuration;
    Fetch transport;
    mutable std::mutex mutex;
    Snapshot state;
    bool manualCheck = false, downloadJob = false;
    juce::int64 nextCheckMs = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Service)
};
}
}
