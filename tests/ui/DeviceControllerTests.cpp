#include <QtTest>
#include "DeviceCenterController.h"
#include "sony/core/DeviceService.h"
#include "../support/ReplyTransport.h"
#include <chrono>
using namespace sony;
using namespace sony::devicecenter;
class SlowService : public core::IDeviceService {
public:
    void tick() override { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
    std::vector<core::DiscoveredDevice> discoverDevices() override { return {}; }
    void connect(const transport::DeviceAddress&, std::string_view) override {}
    void disconnect() noexcept override {}
    bool isConnected() const noexcept override { return false; }
    core::SonyDevice* activeDevice() noexcept override { return nullptr; }
    protocol::DeviceStateSnapshot snapshot() const override { return std::make_shared<const protocol::DeviceState>(); }
};
class DeviceControllerTests : public QObject {
    Q_OBJECT
private slots:
    void startupDoesNotBlockGui() {
        auto service = std::make_shared<SlowService>();
        QElapsedTimer elapsed; elapsed.start();
        DeviceCenterController controller(nullptr, service);
        QVERIFY(elapsed.elapsed() < 100);
        QVERIFY(controller.busy());
        bool guiTick = false;
        QTimer::singleShot(10, [&] { guiTick = true; });
        QTRY_VERIFY_WITH_TIMEOUT(guiTick, 100);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QCOMPARE(controller.batteryLevel(), -1);
        QCOMPARE(controller.noiseControlMode(), QString("unknown"));
        QCOMPARE(controller.codec(), QString("Unknown"));
    }
    void equalizerCapabilities_data() {
        QTest::addColumn<QString>("model");
        QTest::addColumn<bool>("tenBand");
        QTest::newRow("WH-1000XM6") << QString("WH-1000XM6") << true;
        QTest::newRow("WH-1000XM5") << QString("WH-1000XM5") << false;
    }
    void equalizerCapabilities() {
        QFETCH(QString, model);
        QFETCH(bool, tenBand);
        auto transport = std::make_shared<ReplyTransport>();
        auto service = std::make_shared<core::DeviceService>(transport);
        service->connect(transport::DeviceAddress("11:22:33:44:55:66"), model.toStdString());
        DeviceCenterController controller(nullptr, service);
        QSignalSpy capabilitiesChanged(&controller, &DeviceCenterController::capabilitiesChanged);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QVERIFY(!capabilitiesChanged.isEmpty());
        QVERIFY(controller.hasEqualizer());
        QCOMPARE(controller.tenBandEqualizer(), tenBand);
        QCOMPARE(controller.property("tenBandEqualizer"), QVariant(tenBand));
        QCOMPARE(controller.hasClearBass(), !tenBand);
    }
    void failedActionPreservesConfirmedValue() {
        auto transport = std::make_shared<ReplyTransport>();
        auto service = std::make_shared<core::DeviceService>(transport);
        service->connect(transport::DeviceAddress("11:22:33:44:55:66"),"WH-1000XM5");
        DeviceCenterController controller(nullptr, service);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),2000);
        QCOMPARE(controller.noiseControlMode(),QString("cancelling"));
        transport->simulateTimeoutOnSend();
        controller.setNoiseControlOff();
        QVERIFY(controller.busy());
        QCOMPARE(controller.noiseControlMode(),QString("cancelling"));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),3000);
        QVERIFY(!controller.lastError().isEmpty());
        QCOMPARE(controller.noiseControlMode(),QString("cancelling"));
    }
    void notificationsUpdateStateAndBands() {
        auto transport = std::make_shared<ReplyTransport>();
        auto service = std::make_shared<core::DeviceService>(transport);
        service->connect(transport::DeviceAddress("11:22:33:44:55:66"),"WH-1000XM5");
        DeviceCenterController controller(nullptr, service);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),2000);
        QCOMPARE(controller.equalizerBands().size(),5);
        QCOMPARE(controller.equalizerBands()[4].toInt(),5);
        transport->notify({0x69,0x17,1,1,1,0,12});
        QTRY_COMPARE_WITH_TIMEOUT(controller.noiseControlMode(),QString("ambient"),1000);
        QCOMPARE(controller.ambientLevel(),12);
    }
    void ambientWithUnknownLevelUsesFallback() {
        auto transport = std::make_shared<ReplyTransport>();
        auto service = std::make_shared<core::DeviceService>(transport);
        service->connect(transport::DeviceAddress("11:22:33:44:55:66"),"WH-1000XM6");
        DeviceCenterController controller(nullptr, service);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),2000);
        QCOMPARE(controller.ambientLevel(),0);
        auto lastAmbientLevel = [&transport]() -> int {
            int level = -1;
            for (const auto& bytes : transport->sentFrames()) {
                const auto p = protocol::FrameCodec::decode(bytes).payload;
                if (p.size() == 7 && p[0] == 0x68 && p[1] == 0x17) level = p[6];
            }
            return level;
        };
        controller.setAmbient(controller.ambientLevel(), false);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),2000);
        QVERIFY2(controller.lastError().isEmpty(), qPrintable(controller.lastError()));
        QCOMPARE(lastAmbientLevel(),10);
        controller.setAmbient(15, false);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),2000);
        controller.setAmbient(0, true);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),2000);
        QVERIFY2(controller.lastError().isEmpty(), qPrintable(controller.lastError()));
        QCOMPARE(lastAmbientLevel(),15);
    }
    void destructionDrainsWorkerAndCallbacks() {
        auto service = std::make_shared<SlowService>();
        auto controller = std::make_unique<DeviceCenterController>(nullptr,service);
        QTest::qWait(20);
        controller.reset();
        QCoreApplication::processEvents();
    }
};
QTEST_GUILESS_MAIN(DeviceControllerTests)
#include "DeviceControllerTests.moc"
