#include "PluginUpdates.h"
#import <Foundation/Foundation.h>

namespace mconnect::updates
{
Reply fetch(const Request& request, const Cancelled& cancelled, const Progress& progress)
{
    @autoreleasepool
    {
        // Each operation has its own ephemeral session: no cookies, credentials,
        // background agent, or activity after the final editor is destroyed.
        auto* address = [NSString stringWithUTF8String:request.url.toRawUTF8()];
        if (!address) return {0, {}, "Invalid URL"};
        auto* url = [NSURL URLWithString:address];
        if (!url || ![url.scheme isEqualToString:@"https"]) return {0, {}, "HTTPS required"};
        auto* configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        configuration.URLCache = nil;
        configuration.HTTPCookieStorage = nil;
        configuration.URLCredentialStorage = nil;
        configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
        configuration.timeoutIntervalForRequest = 10;
        configuration.timeoutIntervalForResource = request.destination == juce::File() ? 15 : 180;
        configuration.waitsForConnectivity = NO;
        auto* queue = [[NSOperationQueue alloc] init];
        queue.maxConcurrentOperationCount = 1;
        queue.qualityOfService = NSQualityOfServiceUtility;
        auto* session = [NSURLSession sessionWithConfiguration:configuration delegate:nil delegateQueue:queue];
        auto* http = [NSMutableURLRequest requestWithURL:url];
        [http setValue:@"MConnect/" MCONNECT_VERSION forHTTPHeaderField:@"User-Agent"];
        [http setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
        [http setValue:@"2022-11-28" forHTTPHeaderField:@"X-GitHub-Api-Version"];
        struct Completion { Reply reply; std::atomic<bool> done {false}; };
        const auto completion = std::make_shared<Completion>();
        const auto semaphore = dispatch_semaphore_create(0);
        auto* task = [session downloadTaskWithRequest:http completionHandler:^(NSURL* location, NSURLResponse* response, NSError* error)
        {
            auto& reply = completion->reply;
            if ([response isKindOfClass:[NSHTTPURLResponse class]])
                reply.status = static_cast<int>(((NSHTTPURLResponse*)response).statusCode);
            // Reject a TLS-to-HTTP redirect even if Foundation followed it.
            if (error || !location || ![response.URL.scheme isEqualToString:@"https"])
                reply.error = "Network request failed";
            else if (reply.status == 200)
            {
                const juce::File file(juce::String(location.path.UTF8String));
                if (file.getSize() <= 0 || file.getSize() > request.limit) reply.error = "Response size limit";
                else if (request.destination == juce::File()) reply.body = file.loadFileAsString();
                else if (!file.copyFileTo(request.destination)) reply.error = "Could not save download";
            }
            completion->done.store(true, std::memory_order_release);
            dispatch_semaphore_signal(semaphore);
        }];
        task.priority = NSURLSessionTaskPriorityLow;
        [task resume];
        bool stopped = false;
        while (!completion->done.load(std::memory_order_acquire))
        {
            const auto bytes = static_cast<juce::int64>(task.countOfBytesReceived);
            if (!stopped && (cancelled() || bytes > request.limit || task.countOfBytesExpectedToReceive > request.limit))
            { stopped = true; [task cancel]; }
            progress(bytes);
            dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC));
        }
        [session finishTasksAndInvalidate];
        // Drain callbacks before the worker/module can go away. No captured UI.
        [queue waitUntilAllOperationsAreFinished];
        if (stopped) return {0, {}, "Request cancelled or too large"};
        return completion->reply;
    }
}
}
