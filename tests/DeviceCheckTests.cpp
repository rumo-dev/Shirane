#include "model/DeviceCheck.h"
#include "model/DefaultDeviceCheck.h"
#include <gtest/gtest.h>

namespace {
using namespace shirane;
DeviceSnapshot ready() {
    DeviceSnapshot value;
    value.generation = 1;
    value.status = DeviceStatus::Ready;
    value.notificationsAvailable = true;
    value.endpoints = {{"mic", "USB mic", DeviceFlow::Capture, DeviceState::Active}};
    return value;
}
TEST(DeviceCheckTest, ObservesDisconnectReconnectWithoutChangingSelection) {
    auto snapshot = ready(); DeviceCheck check;
    ASSERT_TRUE(check.start(snapshot, "mic", 0));
    snapshot.generation++; snapshot.notificationCount++; snapshot.endpoints.clear();
    check.observe(snapshot, "mic", 1);
    EXPECT_EQ(check.state(), DeviceCheckState::AwaitReconnect);
    snapshot.generation++; snapshot.notificationCount++;
    snapshot.endpoints = ready().endpoints;
    check.observe(snapshot, "mic", 2);
    EXPECT_EQ(check.state(), DeviceCheckState::Observed);
}
TEST(DeviceCheckTest, PollingWithoutNotificationsCannotPass) {
    auto snapshot = ready(); DeviceCheck check;
    ASSERT_TRUE(check.start(snapshot, "mic", 0));
    snapshot.generation++; snapshot.endpoints.clear();
    check.observe(snapshot, "mic", 1);
    EXPECT_EQ(check.state(), DeviceCheckState::NotificationMissing);
}
TEST(DeviceCheckTest, FailedEnumerationIsNotDisconnection) {
    for (const auto status : {DeviceStatus::Partial, DeviceStatus::Failed, DeviceStatus::Loading}) {
        auto snapshot = ready(); DeviceCheck check;
        ASSERT_TRUE(check.start(snapshot, "mic", 0));
        snapshot.generation++; snapshot.notificationCount++; snapshot.endpoints.clear(); snapshot.status = status;
        check.observe(snapshot, "mic", 1);
        EXPECT_EQ(check.state(), DeviceCheckState::EnumerationFailed);
    }
}
TEST(DeviceCheckTest, OtherDevicesAndDefaultsDoNotCompleteCycle) {
    auto snapshot = ready(); DeviceCheck check;
    ASSERT_TRUE(check.start(snapshot, "mic", 0));
    snapshot.generation++; snapshot.notificationCount++;
    snapshot.endpoints.front().multimediaDefault = true;
    check.observe(snapshot, "mic", 1);
    EXPECT_EQ(check.state(), DeviceCheckState::AwaitDisconnect);
    snapshot.generation++; snapshot.notificationCount++; snapshot.endpoints.front().state = DeviceState::Disabled;
    check.observe(snapshot, "mic", 2);
    EXPECT_EQ(check.state(), DeviceCheckState::AwaitDisconnect);
    snapshot.generation++; snapshot.notificationCount++; snapshot.endpoints.front().id = "different-mic";
    check.observe(snapshot, "mic", 3);
    EXPECT_EQ(check.state(), DeviceCheckState::AwaitReconnect);
    snapshot.generation++; snapshot.notificationCount++;
    check.observe(snapshot, "mic", 4);
    EXPECT_EQ(check.state(), DeviceCheckState::AwaitReconnect);
}
TEST(DeviceCheckTest, RejectsSelectionChangesTimeoutAndStaleGeneration) {
    auto snapshot = ready(); DeviceCheck check;
    ASSERT_TRUE(check.start(snapshot, "mic", 0));
    check.observe(snapshot, "other", 1);
    EXPECT_EQ(check.state(), DeviceCheckState::SelectionChanged);
    ASSERT_TRUE(check.start(snapshot, "mic", 0));
    snapshot.endpoints.clear(); snapshot.notificationCount++;
    check.observe(snapshot, "mic", 1);
    EXPECT_EQ(check.state(), DeviceCheckState::AwaitDisconnect);
    check.observe(snapshot, "mic", 120);
    EXPECT_EQ(check.state(), DeviceCheckState::TimedOut);
    check.cancel(); EXPECT_EQ(check.state(), DeviceCheckState::Idle);
}
TEST(DeviceCheckTest, RequiresHealthyActiveInitialDevice) {
    auto snapshot = ready(); DeviceCheck check;
    EXPECT_FALSE(check.start(snapshot, "", 0));
    EXPECT_FALSE(check.start(snapshot, "missing", 0));
    snapshot.notificationsAvailable = false;
    EXPECT_FALSE(check.start(snapshot, "mic", 0));
    snapshot.notificationsAvailable = true; snapshot.endpoints.front().state = DeviceState::Unplugged;
    EXPECT_FALSE(check.start(snapshot, "mic", 0));
}
TEST(DeviceCheckTest, NotificationMayPrecedeEnumerationStateChange) {
    auto snapshot = ready(); DeviceCheck check;
    ASSERT_TRUE(check.start(snapshot, "mic", 0));
    snapshot.generation++; snapshot.notificationCount++;
    check.observe(snapshot, "mic", 1);
    snapshot.generation++; snapshot.endpoints.clear();
    check.observe(snapshot, "mic", 2);
    EXPECT_EQ(check.state(), DeviceCheckState::AwaitReconnect);
    snapshot.generation++; snapshot.endpoints = ready().endpoints;
    check.observe(snapshot, "mic", 3);
    EXPECT_EQ(check.state(), DeviceCheckState::NotificationMissing);
}
TEST(DefaultDeviceCheckTest, ObservesChangeAndRestoreWithPinnedSelection) {
    auto snapshot = ready(); snapshot.endpoints[0].multimediaDefault = true;
    snapshot.endpoints.push_back({"other", "Other mic", DeviceFlow::Capture, DeviceState::Active});
    DefaultDeviceCheck check; ASSERT_TRUE(check.start(snapshot, "mic", 0));
    snapshot.endpoints[0].multimediaDefault = false; snapshot.endpoints[1].multimediaDefault = true;
    snapshot.generation++; snapshot.notificationCount++;
    check.observe(snapshot, "mic", 1); EXPECT_EQ(check.state(), DefaultCheckState::AwaitRestore);
    snapshot.endpoints[0].multimediaDefault = true; snapshot.endpoints[1].multimediaDefault = false;
    snapshot.generation++; snapshot.notificationCount++;
    check.observe(snapshot, "mic", 2); EXPECT_EQ(check.state(), DefaultCheckState::Observed);
}
TEST(DefaultDeviceCheckTest, RejectsSelectionChangeMissingNotificationAndIncompleteData) {
    auto baseline = ready(); baseline.endpoints[0].multimediaDefault = true;
    baseline.endpoints.push_back({"other", "Other mic", DeviceFlow::Capture, DeviceState::Active});
    for (int mode = 0; mode < 3; ++mode) {
        auto snapshot = baseline; DefaultDeviceCheck check; ASSERT_TRUE(check.start(snapshot, "mic", 0));
        snapshot.generation++;
        snapshot.endpoints[0].multimediaDefault = false; snapshot.endpoints[1].multimediaDefault = true;
        if (mode != 1) snapshot.notificationCount++;
        if (mode == 2) snapshot.status = DeviceStatus::Partial;
        check.observe(snapshot, mode == 0 ? "other" : "mic", 1);
        EXPECT_EQ(check.state(), DefaultCheckState::Invalid);
    }
}
TEST(DefaultDeviceCheckTest, UnrelatedChangesDoNotPassAndTimeoutRequiresManualRestoration) {
    auto snapshot = ready(); snapshot.endpoints[0].multimediaDefault = true;
    DefaultDeviceCheck check; ASSERT_TRUE(check.start(snapshot, "mic", 0));
    snapshot.generation++; snapshot.notificationCount++;
    check.observe(snapshot, "mic", 1); EXPECT_EQ(check.state(), DefaultCheckState::AwaitChange);
    check.observe(snapshot, "mic", 180); EXPECT_EQ(check.state(), DefaultCheckState::TimedOut);
    check.cancel(); EXPECT_EQ(check.state(), DefaultCheckState::Idle);
    snapshot.endpoints[0].multimediaDefault = false;
    EXPECT_FALSE(check.start(snapshot, "mic", 0));
}
}
