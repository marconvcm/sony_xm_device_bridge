#include <catch2/catch_test_macros.hpp>
#include "sony/protocol/DeviceEventDispatcher.h"
#include "sony/protocol/DeviceEvents.h"
#include "sony/protocol/DeviceState.h"
#include "Headphones.h"
#include "BluetoothWrapper.h"
#include "CommandSerializer.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace sony::protocol;

namespace {

class DummyConnector final : public IBluetoothConnector {
public:
    int send(char* /*buf*/, size_t len) override { return static_cast<int>(len); }
    int recv(char* /*buf*/, size_t /*len*/) override { return 0; }
    void connect(const std::string&) override {}
    void disconnect() noexcept override {}
    bool isConnected() noexcept override { return true; }
    std::vector<BluetoothDevice> getConnectedDevices() override { return {}; }
    SonyProtocolVersion getProtocolVersion() noexcept override { return SonyProtocolVersion::V2; }
};

} // namespace

TEST_CASE("DeviceEventDispatcher manual dispatch and listener unregistration", "[events]") {
    DeviceEventDispatcher dispatcher;

    int batteryCallCount = 0;
    BatteryState lastBattery;
    auto batSub = dispatcher.onBatteryChanged([&](const BatteryChanged& evt) {
        batteryCallCount++;
        lastBattery = evt.battery;
    });

    int stateCallCount = 0;
    DeviceStateSnapshot lastState;
    auto stateSub = dispatcher.onStateChanged([&](const DeviceStateChanged& evt) {
        stateCallCount++;
        lastState = evt.state;
    });

    BatteryState b;
    b.main = 75;
    b.charging = true;
    dispatcher.dispatch(BatteryChanged{b});

    CHECK(batteryCallCount == 1);
    CHECK(lastBattery.main.value() == 75);
    CHECK(lastBattery.charging);

    DeviceState ds;
    ds.battery = b;
    ds.firmware = "1.0.5";
    auto snap = std::make_shared<const DeviceState>(ds);
    dispatcher.dispatch(DeviceStateChanged{snap});

    CHECK(stateCallCount == 1);
    CHECK(lastState->firmware == "1.0.5");

    // Remove battery listener
    dispatcher.removeListener(batSub);
    b.main = 50;
    dispatcher.dispatch(BatteryChanged{b});
    CHECK(batteryCallCount == 1); // Not incremented

    // Clear all listeners
    dispatcher.clear();
    dispatcher.dispatch(DeviceStateChanged{snap});
    CHECK(stateCallCount == 1); // Not incremented
}

TEST_CASE("DeviceEventDispatcher parses battery notifications", "[events]") {
    DeviceEventDispatcher dispatcher;
    DeviceState state;

    int batteryEventCount = 0;
    BatteryState receivedBattery;
    dispatcher.onBatteryChanged([&](const BatteryChanged& evt) {
        batteryEventCount++;
        receivedBattery = evt.battery;
    });

    int stateEventCount = 0;
    dispatcher.onStateChanged([&](const DeviceStateChanged&) {
        stateEventCount++;
    });

    // 1. Single battery notification (0x25 0x00 <level=88> <charging=1>)
    std::vector<uint8_t> singleBat = {0x25, 0x00, 88, 1};
    bool handled = dispatcher.parseNotificationPayload(singleBat, state);
    CHECK(handled);
    CHECK(batteryEventCount == 1);
    CHECK(stateEventCount == 1);
    CHECK(receivedBattery.main.value() == 88);
    CHECK(receivedBattery.charging);
    CHECK(state.battery.main.value() == 88);

    // 2. TWS battery notification (0x25 0x09 <L=95> <Lchg=0> <R=90> <Rchg=0>)
    std::vector<uint8_t> twsBat = {0x25, 0x09, 95, 0, 90, 0};
    handled = dispatcher.parseNotificationPayload(twsBat, state);
    CHECK(handled);
    CHECK(batteryEventCount == 2);
    CHECK(receivedBattery.left.value() == 95);
    CHECK(receivedBattery.right.value() == 90);
    CHECK(receivedBattery.main.value() == 90); // Min of L and R
    CHECK_FALSE(receivedBattery.charging);

    // 3. Case battery notification (0x25 0x0a <case=60>)
    std::vector<uint8_t> caseBat = {0x25, 0x0a, 60, 0};
    handled = dispatcher.parseNotificationPayload(caseBat, state);
    CHECK(handled);
    CHECK(batteryEventCount == 3);
    CHECK(receivedBattery.caseBattery.value() == 60);
}

TEST_CASE("DeviceEventDispatcher parses noise control notifications", "[events]") {
    DeviceEventDispatcher dispatcher;
    DeviceState state;

    NoiseControlState receivedNc;
    int ncEventCount = 0;
    dispatcher.onNoiseControlChanged([&](const NoiseControlChanged& evt) {
        ncEventCount++;
        receivedNc = evt.noiseControl;
    });

    // NC notification: 0x67 0x17 0x01 <effect: 1=ON> <type: 1=Ambient> <voice: 1> <level: 14>
    std::vector<uint8_t> ncPayload = {0x67, 0x17, 0x01, 0x01, 0x01, 0x01, 14};
    bool handled = dispatcher.parseNotificationPayload(ncPayload, state);
    CHECK(handled);
    CHECK(ncEventCount == 1);
    CHECK(receivedNc.mode == NoiseControlMode::Ambient);
    CHECK(receivedNc.ambientLevel == 14);
    CHECK(receivedNc.focusOnVoice);

    // Noise Cancelling mode: 0x67 0x17 0x01 <effect: 1=ON> <type: 0=NC> <voice: 0> <level: 0>
    std::vector<uint8_t> ncOnPayload = {0x67, 0x17, 0x01, 0x01, 0x00, 0x00, 0};
    handled = dispatcher.parseNotificationPayload(ncOnPayload, state);
    CHECK(handled);
    CHECK(ncEventCount == 2);
    CHECK(receivedNc.mode == NoiseControlMode::NoiseCancelling);
    CHECK(receivedNc.ambientLevel == 0);
    CHECK_FALSE(receivedNc.focusOnVoice);
}

TEST_CASE("DeviceEventDispatcher parses type 0x19 noise control notifications (WH-1000XM6)", "[events]") {
    DeviceEventDispatcher dispatcher;
    DeviceState state;

    NoiseControlState receivedNc;
    int ncEventCount = 0;
    dispatcher.onNoiseControlChanged([&](const NoiseControlChanged& evt) {
        ncEventCount++;
        receivedNc = evt.noiseControl;
    });

    // Captured from a WH-1000XM6 (firmware 3.1.5) when pressing the NC/AMB button.
    std::vector<uint8_t> ambientPayload = {0x69, 0x19, 0x01, 0x01, 0x01, 0x00, 0x03, 0x00, 0x00};
    CHECK(dispatcher.parseNotificationPayload(ambientPayload, state));
    CHECK(ncEventCount == 1);
    CHECK(receivedNc.mode == NoiseControlMode::Ambient);
    CHECK(receivedNc.ambientLevel == 3);

    std::vector<uint8_t> ncPayload = {0x69, 0x19, 0x01, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00};
    CHECK(dispatcher.parseNotificationPayload(ncPayload, state));
    CHECK(ncEventCount == 2);
    CHECK(receivedNc.mode == NoiseControlMode::NoiseCancelling);
    CHECK(receivedNc.ambientLevel == 0);
}

TEST_CASE("DeviceEventDispatcher parses equalizer notifications", "[events]") {
    DeviceEventDispatcher dispatcher;
    DeviceState state;

    EqualizerState receivedEq;
    int eqEventCount = 0;
    dispatcher.onEqualizerChanged([&](const EqualizerChanged& evt) {
        eqEventCount++;
        receivedEq = evt.equalizer;
    });

    // EQ: 0x57 0x00 <preset: 0x16 (bass boost)> 0x06 <bass+10: 16 (+6)> <bands+10: 11, 12, 10, 9, 8>
    std::vector<uint8_t> eqPayload = {0x57, 0x00, 0x16, 0x06, 16, 11, 12, 10, 9, 8};
    bool handled = dispatcher.parseNotificationPayload(eqPayload, state);
    CHECK(handled);
    CHECK(eqEventCount == 1);
    CHECK(receivedEq.preset == 0x16);
    CHECK(receivedEq.clearBass == 6);
    CHECK(receivedEq.bands[0] == 1);
    CHECK(receivedEq.bands[1] == 2);
    CHECK(receivedEq.bands[2] == 0);
    CHECK(receivedEq.bands[3] == -1);
    CHECK(receivedEq.bands[4] == -2);
}

TEST_CASE("DeviceEventDispatcher parses 10-band equalizer notifications (WH-1000XM6)", "[events]") {
    // Previously this fell into the legacy 5-band + Clear Bass parse
    // unconditionally, misreading raw band[0] as "clearBass - 10" and
    // truncating the other nine bands -- see issue #10.
    DeviceEventDispatcher dispatcher;
    DeviceState state;

    EqualizerState receivedEq;
    int eqEventCount = 0;
    dispatcher.onEqualizerChanged([&](const EqualizerChanged& evt) {
        eqEventCount++;
        receivedEq = evt.equalizer;
    });

    // Capture offset 51545: 59 04 20 0a 00 04 08 09 07 05 07 09 06 04
    std::vector<uint8_t> eqPayload = {0x59, 0x04, 0x20, 0x0a, 0x00, 0x04, 0x08, 0x09, 0x07, 0x05, 0x07, 0x09, 0x06, 0x04};
    bool handled = dispatcher.parseNotificationPayload(eqPayload, state);
    CHECK(handled);
    CHECK(eqEventCount == 1);
    CHECK(receivedEq.preset == 0x20);
    CHECK(receivedEq.clearBass == 0);
    CHECK(receivedEq.bands == std::vector<int>{0, 4, 8, 9, 7, 5, 7, 9, 6, 4});
}

TEST_CASE("Headphones event-driven integration", "[events]") {
    auto connector = std::make_unique<DummyConnector>();
    BluetoothWrapper conn(std::move(connector));
    Headphones hp(conn);

    int stateChangeCount = 0;
    DeviceStateSnapshot lastSnap;
    hp.onStateChanged([&](const DeviceStateChanged& evt) {
        stateChangeCount++;
        lastSnap = evt.state;
    });

    int batteryChangeCount = 0;
    hp.onBatteryChanged([&](const BatteryChanged&) {
        batteryChangeCount++;
    });

    int ncChangeCount = 0;
    hp.onNoiseControlChanged([&](const NoiseControlChanged&) {
        ncChangeCount++;
    });

    // Feed unsolicited battery notification via handleNotification
    std::vector<uint8_t> batNtf = {0x25, 0x00, 78, 1};
    bool handled = hp.handleNotification(batNtf);
    CHECK(handled);
    CHECK(batteryChangeCount == 1);
    CHECK(stateChangeCount == 1);
    CHECK(hp.getBatteryLevel() == 78);
    CHECK(hp.isBatteryCharging());
    CHECK(lastSnap->battery.main.value() == 78);

    // Feed unsolicited ambient notification
    std::vector<uint8_t> ncNtf = {0x67, 0x17, 0x01, 0x01, 0x01, 0x01, 16};
    handled = hp.handleNotification(ncNtf);
    CHECK(handled);
    CHECK(ncChangeCount == 1);
    CHECK(stateChangeCount == 2);
    CHECK(hp.getAmbientSoundControl());
    CHECK(hp.getAsmLevel() == 16);
    CHECK(hp.getFocusOnVoice());
}

TEST_CASE("DeviceEventDispatcher concurrency and reentrancy safety", "[events][concurrency]") {
    DeviceEventDispatcher dispatcher;
    DeviceState state;

    std::atomic<bool> running{true};
    std::atomic<int> receivedCount{0};

    // Callback that takes a read or dispatches re-entrantly
    dispatcher.onBatteryChanged([&](const BatteryChanged&) {
        receivedCount.fetch_add(1, std::memory_order_relaxed);
    });

    dispatcher.onStateChanged([&](const DeviceStateChanged&) {
        receivedCount.fetch_add(1, std::memory_order_relaxed);
    });

    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&, id = i]() {
            for (int step = 0; step < 100; ++step) {
                std::vector<uint8_t> bat = {0x25, 0x00, static_cast<uint8_t>(step % 100), 0};
                DeviceState localState;
                dispatcher.parseNotificationPayload(bat, localState);
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    CHECK(receivedCount.load() > 0);
}
