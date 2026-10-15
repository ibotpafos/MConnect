#include "PluginUpdates.h"
#include "PluginProcessor.h"
#include <juce_cryptography/juce_cryptography.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace mconnect::updates;
namespace
{
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
const juce::String repository = "ibotpafos/MConnect", payload = "test installer bytes";
juce::var releaseData(const juce::String& version = "0.1.4")
{
    const auto tag = "v" + version;
    const auto name = "MConnect-" + version + "-macos-universal.pkg";
    auto asset = std::make_unique<juce::DynamicObject>();
    asset->setProperty("name", name);
    asset->setProperty("state", "uploaded");
    asset->setProperty("size", static_cast<juce::int64>(payload.getNumBytesAsUTF8()));
    asset->setProperty("digest", "sha256:" + juce::SHA256(payload.toRawUTF8(), payload.getNumBytesAsUTF8()).toHexString());
    asset->setProperty("browser_download_url", "https://github.com/" + repository + "/releases/download/" + tag + "/" + name);
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("tag_name", tag);
    object->setProperty("html_url", "https://github.com/" + repository + "/releases/tag/" + tag);
    object->setProperty("draft", false);
    object->setProperty("prerelease", false);
    object->setProperty("assets", juce::Array<juce::var> {juce::var(asset.release())});
    return juce::var(object.release());
}
juce::String json(const juce::var& data) { return juce::JSON::toString(data); }
void waitFor(Service& service)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 4000;
    for (;;)
    {
        const auto status = service.snapshot().phase;
        if (status != Phase::checking && status != Phase::downloading) break;
        require(juce::Time::getMillisecondCounterHiRes() < deadline, "worker did not finish");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Allow Thread::run to return after publishing the result.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
struct Fixture
{
    juce::File directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("mconnect-update-test", "", false);
    std::atomic<int> requests {0}, downloads {0};
    bool offline = false, corrupt = false;
    juce::String body = json(releaseData());
    Configuration config() const { return {repository, "0.1.3", directory}; }
    Fetch transport()
    {
        return [this](const Request& request, const Cancelled& cancelled, const Progress& progress)
        {
            if (cancelled()) return Reply {0, {}, "cancelled"};
            if (request.destination == juce::File())
            {
                ++requests;
                if (offline) return Reply {0, {}, "offline"};
                require(request.url == "https://api.github.com/repos/" + repository + "/releases/latest", "wrong metadata endpoint");
                return Reply {200, body, {}};
            }
            ++downloads;
            if (offline) return Reply {503, {}, "offline"};
            require(request.destination.replaceWithText(corrupt ? payload.toUpperCase() : payload), "fixture write failed");
            progress(request.destination.getSize());
            return Reply {200, {}, {}};
        };
    }
    ~Fixture() { directory.deleteRecursively(); }
};
void versionsAndMetadata()
{
    require(compareVersions("0.1.10", "0.1.9") == 1, "lexical version comparison");
    require(compareVersions("1.0.0", "0.99.999") == 1, "major comparison");
    require(compareVersions("0.1.3", "0.1.3") == 0, "equality");
    for (const auto* version : {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.-2.3", "1.2.3-beta", "9999999.0.0", "1..3", "1.2.3/evil"})
        require(!compareVersions(version, "0.1.3"), "invalid version accepted");
    std::optional<Release> release;
    require(parseRelease(json(releaseData()), repository, "0.1.3", release).wasOk() && release, "new release missing");
    require(parseRelease(json(releaseData("0.1.3")), repository, "0.1.3", release).wasOk() && !release, "same version offered");
    require(parseRelease(json(releaseData("0.1.2")), repository, "0.1.3", release).wasOk() && !release, "downgrade offered");
    for (const auto* property : {"draft", "prerelease"})
    {
        auto data = releaseData(); data.getDynamicObject()->setProperty(property, true);
        require(parseRelease(json(data), repository, "0.1.3", release).failed(), "unstable release accepted");
        data.getDynamicObject()->setProperty(property, "false");
        require(parseRelease(json(data), repository, "0.1.3", release).failed(), "wrong boolean type accepted");
    }
    for (const auto* url : {"http://github.com/ibotpafos/MConnect/releases/download/v0.1.4/evil.pkg", "https://github.com/attacker/MConnect/releases/download/v0.1.4/evil.pkg", "https://example.org/update.pkg", "file:///tmp/evil.pkg"})
    {
        auto data = releaseData();
        data["assets"][0].getDynamicObject()->setProperty("browser_download_url", url);
        require(parseRelease(json(data), repository, "0.1.3", release).failed(), "untrusted installer URL accepted");
    }
    for (const auto* digest : {"", "md5:abcd", "sha256:abcd", "sha256:ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ"})
    {
        auto data = releaseData(); data["assets"][0].getDynamicObject()->setProperty("digest", digest);
        require(parseRelease(json(data), repository, "0.1.3", release).failed(), "bad digest accepted");
    }
    for (const juce::var& size : {juce::var(0), juce::var(-1), juce::var(maximumInstallerBytes + 1), juce::var("19"), juce::var(19.0)})
    {
        auto data = releaseData(); data["assets"][0].getDynamicObject()->setProperty("size", size);
        require(parseRelease(json(data), repository, "0.1.3", release).failed(), "bad size accepted");
    }
    auto duplicate = releaseData();
    duplicate["assets"].getArray()->add(duplicate["assets"][0]);
    require(parseRelease(json(duplicate), repository, "0.1.3", release).failed(), "duplicate installer accepted");
    for (const auto& malformed : {juce::String("{"), juce::String("null"), juce::String("[]"), juce::String::repeatedString("[", 2000) + juce::String::repeatedString("]", 2000), juce::String::repeatedString(" ", static_cast<int>(maximumMetadataBytes + 1))})
        require(parseRelease(malformed, repository, "0.1.3", release).failed(), "malformed metadata accepted");
}
void checkDownloadAndCache()
{
    Fixture fixture;
    {
        Service service(fixture.config(), fixture.transport());
        service.check(); waitFor(service);
        require(service.snapshot().phase == Phase::available && fixture.requests == 1, "automatic check failed");
        service.check(); waitFor(service);
        require(fixture.requests == 1, "automatic check was not throttled");
        service.download(); waitFor(service);
        const auto ready = service.snapshot();
        require(ready.phase == Phase::ready && ready.release && verifyInstaller(ready.installer, *ready.release), "verified download missing");
        require(fixture.downloads == 1, "duplicate download");
        require(ready.installer.deleteFile(), "could not purge cached installer");
        service.download(); waitFor(service);
        require(service.snapshot().phase == Phase::ready && fixture.downloads == 2, "purged ready installer was not restored");
        require(service.snapshot().installer.replaceWithText(payload.toUpperCase()), "could not alter ready installer");
        service.download(); waitFor(service);
        require(service.snapshot().phase == Phase::ready && fixture.downloads == 3, "changed ready installer was not verified again");
    }
    {
        Service reopened(fixture.config(), fixture.transport());
        reopened.check(); waitFor(reopened);
        require(reopened.snapshot().phase == Phase::available && fixture.requests == 1, "persistent check cache missing");
        reopened.download(); waitFor(reopened);
        require(reopened.snapshot().phase == Phase::ready && fixture.downloads == 3, "verified installer was not reused");
    }
    {
        Service offline(fixture.config(), fixture.transport());
        fixture.offline = true;
        offline.check(true); waitFor(offline);
        require(offline.snapshot().phase == Phase::available, "offline check lost a known update");
        require(fixture.requests == 2, "manual check did not bypass the cache");
    }
}
void failedDownloadsAndRetry()
{
    Fixture fixture;
    Service service(fixture.config(), fixture.transport());
    service.check(); waitFor(service);
    fixture.offline = true;
    service.download(); waitFor(service);
    require(service.snapshot().phase == Phase::available && service.snapshot().error.isNotEmpty(), "failed download could not retry");
    fixture.offline = false;
    fixture.corrupt = true;
    service.download(); waitFor(service);
    auto status = service.snapshot();
    require(status.phase == Phase::available && status.installer == juce::File() && status.error.isNotEmpty(), "corrupt installer offered");
    require(fixture.directory.findChildFiles(juce::File::findFiles, false, "*.pkg").isEmpty(), "corrupt package published to cache");
    fixture.corrupt = false;
    service.download(); waitFor(service);
    require(service.snapshot().phase == Phase::ready, "download retry failed");
    // A previously downloaded but changed package must be downloaded and checked again.
    require(service.snapshot().installer.replaceWithText("local corruption"), "fixture corrupt failed");
    {
        Service reopened(fixture.config(), fixture.transport());
        reopened.check(); waitFor(reopened); reopened.download(); waitFor(reopened);
        require(reopened.snapshot().phase == Phase::ready && fixture.downloads == 4, "changed cached installer reused");
    }
}
void offlineAndShutdown()
{
    Fixture fixture; fixture.offline = true;
    {
        Service service(fixture.config(), fixture.transport());
        service.check(); waitFor(service);
        require(service.snapshot().phase == Phase::failed, "offline check not reported");
    }
    {
        Service service(fixture.config(), fixture.transport());
        service.check(); waitFor(service);
        require(fixture.requests == 1, "failed checks were not persistently throttled");
    }
    std::atomic<bool> entered {false}, cancelled {false};
    const auto start = juce::Time::getMillisecondCounterHiRes();
    {
        Service service(fixture.config(), [&](const Request&, const Cancelled& stop, const Progress&)
        {
            entered = true;
            while (!stop()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            cancelled = true;
            return Reply {0, {}, "cancelled"};
        });
        service.check(true);
        while (!entered)
        {
            require(juce::Time::getMillisecondCounterHiRes() - start < 1000, "shutdown fixture did not start");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    require(cancelled && juce::Time::getMillisecondCounterHiRes() - start < 500, "editor close blocked on network");
    {
        Service reopened(fixture.config(), fixture.transport());
        reopened.check(); waitFor(reopened);
        require(fixture.requests == 1, "cancelled check caused an API request burst on reopen");
    }
    require(fetch({"http://localhost/not-allowed", {}, 1024}, [] { return false; }, [](juce::int64) {}).error.isNotEmpty(), "HTTP transport accepted");
}
void corruptedCaches()
{
    for (int scenario = 0; scenario < 3; ++scenario)
    {
        Fixture fixture;
        fixture.directory.createDirectory();
        const auto now = juce::Time::currentTimeMillis();
        auto record = std::make_unique<juce::DynamicObject>();
        record->setProperty("repository", repository);
        record->setProperty("attempted", scenario == 0 ? juce::var(1.0e100) : juce::var(now + 10000000));
        record->setProperty("checked", scenario == 0 ? juce::var(1.0e100) : juce::var(now + 10000000));
        record->setProperty("body", fixture.body);
        const auto content = scenario == 2 ? juce::String::repeatedString("[", 2000)
            : json(juce::var(record.release()));
        require(fixture.directory.getChildFile("release.json").replaceWithText(content), "cache fixture write");
        Service service(fixture.config(), fixture.transport());
        service.check(); waitFor(service);
        require(fixture.requests == 1 && service.snapshot().phase == Phase::available, "invalid cache blocked fresh checking");
    }
}
}
int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    try
    {
        if (argc == 4 && juce::String(argv[1]) == "--download")
        {
            const juce::File reference {juce::String(argv[3])};
            require(reference.existsAsFile(), "missing download reference");
            juce::TemporaryFile downloaded(".zip");
            const auto reply = fetch({juce::String(argv[2]), downloaded.getFile(), reference.getSize()},
                                     [] { return false; }, [](juce::int64) {});
            require(reply.status == 200 && reply.error.isEmpty(), "native binary download failed");
            require(downloaded.getFile().getSize() == reference.getSize()
                    && juce::SHA256(downloaded.getFile()) == juce::SHA256(reference), "native downloaded bytes differ");
            std::cout << "PASS: native redirected HTTPS binary download, size and SHA256 ("
                      << reference.getSize() << " bytes)\n";
            return 0;
        }
        if (argc == 2 && juce::String(argv[1]) == "--editor")
        {
            mconnect::MConnectAudioProcessor processor;
            double longestClose = 0;
            for (int i = 0; i < 8; ++i)
            {
                std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
                require(editor->getWidth() == 420 && editor->getHeight() == 540, "unexpected editor size");
                for (auto* child : editor->getChildren())
                    require(editor->getLocalBounds().contains(child->getBounds()), "control outside editor");
                if (i == 0)
                {
                    auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 2.0f);
                    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile("output/updates/editor.png");
                    file.getParentDirectory().createDirectory();
                    auto output = file.createOutputStream();
                    require(output != nullptr && juce::PNGImageFormat().writeImageToStream(image, *output), "editor screenshot failed");
                }
                const auto start = juce::Time::getMillisecondCounterHiRes();
                editor.reset();
                longestClose = juce::jmax(longestClose, juce::Time::getMillisecondCounterHiRes() - start);
            }
            require(longestClose < 1000, "editor close blocked on update transport");
            std::cout << "PASS: eight editor opens/closes, layout and snapshot; max close " << longestClose << " ms\n";
            return 0;
        }
        if (argc == 2 && juce::String(argv[1]) == "--network")
        {
            const auto reply = fetch({"https://api.github.com/repos/ibotpafos/MConnect/releases/latest", {}, maximumMetadataBytes}, [] { return false; }, [](juce::int64) {});
            require(reply.status == 200 || reply.status == 404, "native HTTPS GitHub probe failed");
            if (reply.status == 200)
            {
                std::optional<Release> published;
                require(parseRelease(reply.body, repository, "0.0.0", published).wasOk() && published,
                        "published GitHub release does not satisfy update policy");
            }
            const auto start = juce::Time::getMillisecondCounterHiRes();
            const auto cancelled = fetch({"https://api.github.com/repos/ibotpafos/MConnect/releases/latest", {}, maximumMetadataBytes}, [] { return true; }, [](juce::int64) {});
            require(cancelled.error.isNotEmpty() && juce::Time::getMillisecondCounterHiRes() - start < 1000, "native cancellation was slow");
            std::cout << "PASS: native HTTPS status " << reply.status << ", cancellation and callback drain\n";
            return 0;
        }
        versionsAndMetadata(); checkDownloadAndCache(); failedDownloadsAndRetry(); offlineAndShutdown(); corruptedCaches();
    }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
    std::cout << "PASS: update versions, metadata trust/limits, download integrity/retry, persistent/offline cache and cancellation\n";
    return 0;
}
