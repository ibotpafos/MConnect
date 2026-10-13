// Test fixture only. Keeps hidden browser audio clocks from being coalesced.
// Never linked into the plugin or installed in the user's applications.
#import <Foundation/Foundation.h>
#include <mach/mach.h>
#include <mach/task_policy.h>

static id browserTestActivity;
static void applyTestTiming()
{
    task_category_policy_data_t category {TASK_FOREGROUND_APPLICATION};
    task_policy_set(mach_task_self(), TASK_CATEGORY_POLICY,
                    reinterpret_cast<task_policy_t>(&category), TASK_CATEGORY_POLICY_COUNT);
    task_qos_policy timing {LATENCY_QOS_TIER_0, THROUGHPUT_QOS_TIER_0};
    task_policy_set(mach_task_self(), TASK_BASE_QOS_POLICY,
                    reinterpret_cast<task_policy_t>(&timing), TASK_QOS_POLICY_COUNT);
    task_policy_set(mach_task_self(), TASK_OVERRIDE_QOS_POLICY,
                    reinterpret_cast<task_policy_t>(&timing), TASK_QOS_POLICY_COUNT);
}

__attribute__((constructor)) static void configureBrowserTestTiming()
{
    applyTestTiming();
    browserTestActivity = [[[NSProcessInfo processInfo]
        beginActivityWithOptions:(NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical)
        reason:@"MConnect browser audio validation"] retain];

}

__attribute__((destructor)) static void finishBrowserTestTiming()
{
    if (browserTestActivity)
    {
        [[NSProcessInfo processInfo] endActivity:browserTestActivity];
        [browserTestActivity release];
    }
}
