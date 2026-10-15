#include "PluginUpdates.h"
#include <juce_cryptography/juce_cryptography.h>
#include <array>

namespace mconnect::updates
{
namespace
{
std::optional<std::array<int, 3>> versionParts(const juce::String& version)
{
    const auto parts = juce::StringArray::fromTokens(version, ".", "");
    if (parts.size() != 3) return {};
    std::array<int, 3> values {};
    for (int i = 0; i < 3; ++i)
    {
        const auto& part = parts[i];
        if (part.isEmpty() || part.length() > 6 || !part.containsOnly("0123456789")
            || (part.length() > 1 && part.startsWithChar('0'))) return {};
        values[static_cast<size_t>(i)] = part.getIntValue();
    }
    return values;
}
bool boundedJson(const juce::String& json)
{
    if (json.getNumBytesAsUTF8() > static_cast<size_t>(maximumMetadataBytes)) return false;
    int depth = 0;
    bool quoted = false, escaped = false;
    for (auto c : json)
    {
        if (quoted)
        {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        }
        else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 32) return false; }
        else if (c == '}' || c == ']') { if (--depth < 0) return false; }
    }
    return !quoted && depth == 0;
}
juce::File defaultCache()
{
#if JUCE_MAC
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile("Library/Caches/MConnect/Updates");
#else
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("MConnect/Updates");
#endif
}
juce::String readBounded(const juce::File& file)
{
    if (!file.existsAsFile() || file.getSize() > maximumMetadataBytes * 2) return {};
    return file.loadFileAsString();
}
}

std::optional<int> compareVersions(const juce::String& left, const juce::String& right)
{
    const auto a = versionParts(left), b = versionParts(right);
    if (!a || !b) return {};
    return *a == *b ? 0 : (*a > *b ? 1 : -1);
}

juce::Result parseRelease(const juce::String& json, const juce::String& repository,
                          const juce::String& currentVersion, std::optional<Release>& result)
{
    result.reset();
    const auto invalid = [&result] { result.reset(); return juce::Result::fail("Invalid update information"); };
    if (!boundedJson(json)) return invalid();
    juce::var data;
    if (juce::JSON::parse(json, data).failed()) return invalid();
    auto* object = data.getDynamicObject();
    if (!object || !object->getProperty("draft").isBool() || !object->getProperty("prerelease").isBool()
        || static_cast<bool>(object->getProperty("draft"))
        || static_cast<bool>(object->getProperty("prerelease"))) return invalid();
    const auto tag = object->getProperty("tag_name").toString();
    if (!tag.startsWithChar('v')) return invalid();
    const auto version = tag.substring(1);
    const auto comparison = compareVersions(version, currentVersion);
    if (!comparison) return invalid();
    if (*comparison <= 0) return juce::Result::ok();
    const auto base = "https://github.com/" + repository + "/releases/";
    if (object->getProperty("html_url").toString() != base + "tag/" + tag) return invalid();
    auto* assets = object->getProperty("assets").getArray();
    if (!assets || assets->size() > 64) return invalid();
    const auto filename = "MConnect-" + version + "-macos-universal.pkg";
    for (const auto& item : *assets)
    {
        auto* asset = item.getDynamicObject();
        if (!asset || asset->getProperty("name").toString() != filename) continue;
        if (result) return invalid();
        const auto size = asset->getProperty("size");
        const auto digest = asset->getProperty("digest").toString();
        const auto url = asset->getProperty("browser_download_url").toString();
        if ((!size.isInt() && !size.isInt64()) || static_cast<juce::int64>(size) <= 0
            || static_cast<juce::int64>(size) > maximumInstallerBytes
            || asset->getProperty("state").toString() != "uploaded"
            || !digest.startsWith("sha256:") || digest.length() != 71
            || !digest.substring(7).containsOnly("0123456789abcdef")
            || url != base + "download/" + tag + "/" + filename) return invalid();
        result = Release { version, url, digest.substring(7), filename, static_cast<juce::int64>(size) };
    }
    return result ? juce::Result::ok() : juce::Result::fail("No compatible installer in this release");
}

bool verifyInstaller(const juce::File& file, const Release& release)
{
    return file.existsAsFile() && file.getSize() == release.size
        && file.getSize() > 0 && file.getSize() <= maximumInstallerBytes
        && juce::SHA256(file).toHexString() == release.sha256;
}

Service::Service() : Service(Configuration {}) {}
Service::Service(Configuration config, Fetch fetcher)
    : Thread("MConnect updates"), configuration(std::move(config)), transport(std::move(fetcher))
{
    if (configuration.cache == juce::File()) configuration.cache = defaultCache();
}
Service::~Service()
{
    signalThreadShouldExit(); notify();
    // Native transport cancels its task and drains completion callbacks first.
    // Never kill a worker or leave plugin code scheduled after module unload.
    waitForThreadToExit(-1);
}
Snapshot Service::snapshot() const { const std::lock_guard lock(mutex); return state; }
void Service::check(bool manual)
{
    const std::lock_guard lock(mutex);
    if (isThreadRunning() || state.phase == Phase::ready
        || (!manual && juce::Time::currentTimeMillis() < nextCheckMs)) return;
    manualCheck = manual; downloadJob = false;
    state.phase = Phase::checking;
    state.error.clear();
    if (!startThread(juce::Thread::Priority::low))
    {
        state.phase = Phase::failed; state.error = "Could not start update check";
    }
}
void Service::download()
{
    const std::lock_guard lock(mutex);
    if (isThreadRunning() || !state.release) return;
    downloadJob = true; state.phase = Phase::downloading; state.percent = 0; state.error.clear();
    if (!startThread(juce::Thread::Priority::low))
    {
        state.phase = Phase::available; state.error = "Could not start download";
    }
}
void Service::fail(const juce::String& error)
{
    const std::lock_guard lock(mutex);
    state.phase = state.release ? Phase::available : Phase::failed;
    state.error = error;
}
void Service::run()
{
    bool downloadNow;
    std::optional<Release> release;
    { const std::lock_guard lock(mutex); downloadNow = downloadJob; release = state.release; }
    if (configuration.cache.createDirectory().failed()) { fail("Update cache is unavailable"); return; }
    if (downloadNow && release) downloadOnWorker(*release);
    else checkOnWorker();
}
void Service::checkOnWorker()
{
    juce::InterProcessLock cacheLock("audio.mconnect.updates." + juce::String(configuration.cache.getFullPathName().hashCode64()));
    if (!cacheLock.enter(0))
    {
        const std::lock_guard lock(mutex);
        state.phase = state.release ? Phase::available : Phase::idle;
        nextCheckMs = juce::Time::currentTimeMillis() + 60000;
        return;
    }
    struct Unlock { juce::InterProcessLock& lock; ~Unlock() { lock.exit(); } } unlock {cacheLock};
    const auto cacheFile = configuration.cache.getChildFile("release.json");
    const auto cacheText = readBounded(cacheFile);
    const auto saved = boundedJson(cacheText) ? juce::JSON::parse(cacheText) : juce::var();
    const auto now = juce::Time::currentTimeMillis();
    juce::int64 attempted = 0, checked = 0;
    juce::String cachedBody;
    if (auto* object = saved.getDynamicObject(); object && object->getProperty("repository").toString() == configuration.repository)
    {
        const auto integer = [](const juce::var& value) -> juce::int64
        { return value.isInt() || value.isInt64() ? static_cast<juce::int64>(value) : 0; };
        attempted = integer(object->getProperty("attempted"));
        checked = integer(object->getProperty("checked"));
        if (attempted < 0 || attempted > now || checked < 0 || checked > now) attempted = checked = 0;
        cachedBody = object->getProperty("body").toString();
    }
    std::optional<Release> cachedRelease;
    if (parseRelease(cachedBody, configuration.repository, configuration.version, cachedRelease).failed())
    { cachedBody.clear(); checked = 0; }
    const auto due = attempted > checked ? attempted + retryIntervalMs : checked + checkIntervalMs;
    if (!manualCheck && now < due)
    {
        const std::lock_guard lock(mutex);
        state.release = cachedRelease;
        state.phase = cachedRelease ? Phase::available : (checked > 0 ? Phase::current : Phase::idle);
        nextCheckMs = due;
        return;
    }
    const auto saveCache = [&](juce::int64 successTime, const juce::String& body)
    {
        auto record = std::make_unique<juce::DynamicObject>();
        record->setProperty("repository", configuration.repository);
        record->setProperty("attempted", now);
        record->setProperty("checked", successTime);
        record->setProperty("body", body);
        juce::TemporaryFile temporary(cacheFile);
        if (temporary.getFile().replaceWithText(juce::JSON::toString(juce::var(record.release()))))
            temporary.overwriteTargetFileWithTemporary();
    };
    // Reserve the attempt before networking: repeated open/close or hosts in
    // other processes must not create a burst of cancelled API requests.
    saveCache(checked, cachedBody);
    const auto response = transport({"https://api.github.com/repos/" + configuration.repository + "/releases/latest", {}, maximumMetadataBytes},
                                    [this] { return threadShouldExit(); }, [](juce::int64) {});
    if (threadShouldExit()) return;
    std::optional<Release> latest;
    auto parsed = parseRelease(response.body, configuration.repository, configuration.version, latest);
    const bool valid = response.status == 200 && response.error.isEmpty() && parsed.wasOk();
    saveCache(valid ? now : checked, valid ? response.body : cachedBody);
    const std::lock_guard lock(mutex);
    state.release = valid ? latest : cachedRelease;
    state.phase = state.release ? Phase::available : (valid ? Phase::current : Phase::failed);
    state.error = valid ? juce::String() : "Updates unavailable. Click to retry.";
    nextCheckMs = now + (valid ? checkIntervalMs : retryIntervalMs);
}
void Service::downloadOnWorker(const Release& release)
{
    // Distinct partial names permit multiple DAW processes without collisions.
    const auto target = configuration.cache.getChildFile(release.filename);
    juce::TemporaryFile partial(target);
    if (!verifyInstaller(target, release))
    {
        const auto response = transport({release.url, partial.getFile(), release.size},
            [this] { return threadShouldExit(); }, [this, size = release.size](juce::int64 bytes)
            {
                const std::lock_guard lock(mutex);
                state.percent = static_cast<int>(juce::jlimit<juce::int64>(0, 99, bytes * 100 / size));
            });
        if (threadShouldExit()) return;
        if (response.status != 200 || response.error.isNotEmpty()) { fail("Download failed. Click to retry."); return; }
        if (!verifyInstaller(partial.getFile(), release)) { fail("Installer verification failed. Click to retry."); return; }
        if (!partial.overwriteTargetFileWithTemporary()) { fail("Could not save installer. Click to retry."); return; }
    }
    if (threadShouldExit()) return;
    const std::lock_guard lock(mutex);
    state.phase = Phase::ready; state.installer = target; state.percent = 100;
}
}
