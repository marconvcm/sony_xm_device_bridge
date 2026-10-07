#include "DeviceCenterController.h"
#include "DeviceBackend.h"
#include <QJsonDocument>
#include <QJsonArray>
#include "I18nManager.h"
#include "sony/core/DeviceService.h"
#include "sony/core/IpcProtocol.h"
#include "sony/protocol/DeviceProfileRegistry.h"
#include "sony/protocol/EqualizerPresets.h"
#include "sony/transport/PlatformTransport.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>
#include <QVariantMap>
#include <algorithm>
#include <sstream>

namespace sony::devicecenter {

DeviceCenterController::DeviceCenterController(QObject* parent, std::shared_ptr<core::IDeviceService> service)
    : QObject(parent) {
    QSettings settings("SonyBridge", "SonyDeviceCenter");
    _currentLanguage = settings.value("language", "en").toString();
    _backend = new DeviceBackend(std::move(service));
    _backend->moveToThread(&_worker);
    connect(&_worker, &QThread::started, _backend, &DeviceBackend::start);
    connect(&_worker, &QThread::finished, _backend, &QObject::deleteLater);
    connect(_backend, &DeviceBackend::snapshotReady, this, [this](const QByteArray& data, quint64 generation) {
        if (generation == _generation) _applySnapshot(data);
    });
    connect(_backend, &DeviceBackend::devicesReady, this, [this](const QByteArray& data) {
        _pairedDevices = QJsonDocument::fromJson(data).array().toVariantList();
        emit pairedDevicesChanged();
    });
    connect(_backend, &DeviceBackend::error, this, [this](const QString& error, quint64 generation) {
        if (generation == _generation) { _lastError = error; emit stateChanged(); }
    });
    connect(_backend, &DeviceBackend::completed, this, [this](quint64 generation) {
        if (generation == _generation) { _busy = false; emit stateChanged(); }
    });
    _worker.start();
}
DeviceCenterController::~DeviceCenterController() {
    QMetaObject::invokeMethod(_backend, &DeviceBackend::shutdown, Qt::BlockingQueuedConnection);
    _worker.quit(); _worker.wait();
}
void DeviceCenterController::_send(const QString& method, const QJsonObject& params) {
    if (_busy) return;
    _busy = true; _lastError.clear(); const auto generation = ++_generation;
    emit stateChanged();
    const auto bytes = QJsonDocument(QJsonObject{{"method",method},{"params",params}}).toJson(QJsonDocument::Compact);
    QMetaObject::invokeMethod(_backend, [backend = _backend, bytes, generation] { backend->command(bytes, generation); }, Qt::QueuedConnection);
}
void DeviceCenterController::_applySnapshot(const QByteArray& data) {
    const auto s = QJsonDocument::fromJson(data).object();
    _connected = s.value("connected").toBool();
    _connectionState = s.value("connectionState").toString("disconnected");
    if (s.contains("name")) _deviceName = s.value("name").toString();
    if (s.contains("address")) _deviceAddress = s.value("address").toString();
    if (!s.contains("features")) {
        _batteryLevel = -1; _noiseControlMode = "unknown";
        emit stateChanged(); return;
    }
    _features = s.value("features").toObject().toVariantMap();
    _capabilities = s.value("capabilities").toObject();
    auto valid = [&s](const char* feature) {
        return s.value("features").toObject().value(feature).toObject().value("availability").toString() == "valid";
    };
    const auto battery = s.value("battery").toObject();
    _batteryLevel = _connected && valid("battery") ? battery.value("main").toInt(-1) : -1;
    _isCharging = _connected && battery.value("charging").toBool();
    const auto nc = s.value("noiseControl").toObject();
    _noiseControlMode = _connected && valid("noiseControl") ? nc.value("mode").toString() : "unknown";
    _ambientLevel = nc.value("ambientLevel").toInt(); _focusOnVoice = nc.value("focusOnVoice").toBool();
    const auto eq = s.value("equalizer").toObject();
    _equalizerPreset = valid("equalizer") ? eq.value("preset").toInt() : -1;
    _equalizerPresetName = valid("equalizer") ? eq.value("presetName").toString() : "Unknown";
    _clearBass = eq.value("clearBass").toInt(); _equalizerBands = eq.value("bands").toArray().toVariantList();
    _dsee = s.value("dsee").toBool(); _speakToChat = s.value("speakToChat").toBool();
    _adaptiveVolume = s.value("adaptiveVolume").toBool(); _autoPowerOff = s.value("autoPowerOff").toInt();
    _codec = _connected && valid("codec") ? s.value("codec").toString("Unknown") : "Unknown";
    emit stateChanged(); emit capabilitiesChanged();
}

QString DeviceCenterController::deviceName() const { return _deviceName; }
QString DeviceCenterController::deviceAddress() const { return _deviceAddress; }
bool DeviceCenterController::isConnected() const { return _connected; }
int DeviceCenterController::batteryLevel() const { return _batteryLevel; }
bool DeviceCenterController::isCharging() const { return _isCharging; }
QString DeviceCenterController::noiseControlMode() const { return _noiseControlMode; }
int DeviceCenterController::ambientLevel() const { return _ambientLevel; }
bool DeviceCenterController::focusOnVoice() const { return _focusOnVoice; }
int DeviceCenterController::equalizerPreset() const { return _equalizerPreset; }
QString DeviceCenterController::equalizerPresetName() const { return _equalizerPresetName; }
int DeviceCenterController::clearBass() const { return _clearBass; }
QVariantList DeviceCenterController::equalizerBands() const { return _equalizerBands; }
bool DeviceCenterController::dsee() const { return _dsee; }
bool DeviceCenterController::speakToChat() const { return _speakToChat; }
bool DeviceCenterController::adaptiveVolume() const { return _adaptiveVolume; }
int DeviceCenterController::autoPowerOff() const { return _autoPowerOff; }

QString DeviceCenterController::heroImagePath() const {
    QString lower = _deviceName.toLower();
    if (lower.contains("wf") && lower.contains("xm6")) {
        return "resources/devices/wf-1000xm6.png";
    } else if (lower.contains("wf") && lower.contains("xm5")) {
        return "resources/devices/wf-1000xm5.png";
    } else if (lower.contains("wf") && lower.contains("xm4")) {
        return "resources/devices/wf-1000xm4.png";
    } else if (lower.contains("wf") && lower.contains("xm3")) {
        return "resources/devices/wf-1000xm3.png";
    } else if (lower.contains("xm6")) {
        return "resources/devices/wh-1000xm6.png";
    } else if (lower.contains("xm5")) {
        return "resources/devices/wh-1000xm5.png";
    } else if (lower.contains("xm4")) {
        return "resources/devices/wh-1000xm4.png";
    } else if (lower.contains("xm3")) {
        return "resources/devices/wh-1000xm3.png";
    } else if (lower.contains("ch720")) {
        return "resources/devices/wh-ch720n.png";
    } else if (lower.contains("linkbuds")) {
        return "resources/devices/linkbuds-s.png";
    }
    return "resources/devices/wh-1000xm5.png";
}

bool DeviceCenterController::hasAnc() const { return _capabilities.value("anc").toBool(); }

bool DeviceCenterController::hasAmbient() const { return _capabilities.value("ambient").toBool(); }

bool DeviceCenterController::hasEqualizer() const { return _capabilities.value("equalizer").toBool(); }

bool DeviceCenterController::hasClearBass() const { return _capabilities.value("clearBass").toBool(); }

bool DeviceCenterController::tenBandEqualizer() const { return _capabilities.value("tenBandEqualizer").toBool(); }

bool DeviceCenterController::hasDsee() const { return _capabilities.value("dsee").toBool(); }

bool DeviceCenterController::hasSpeakToChat() const { return _capabilities.value("speakToChat").toBool(); }

bool DeviceCenterController::hasAdaptiveVolume() const { return _capabilities.value("adaptiveVolume").toBool(); }

QVariantList DeviceCenterController::pairedDevices() const { return _pairedDevices; }

void DeviceCenterController::setAnc(bool enabled) { _send("anc", {{"enabled",enabled}}); }
void DeviceCenterController::setAmbient(int level, bool voice) {
    if (level < 1 || level > 20) level = _lastAmbientLevel;
    _lastAmbientLevel = level;
    _send("ambient", {{"level",level},{"focusOnVoice",voice}});
}
void DeviceCenterController::setNoiseControlOff() { setAnc(false); }
void DeviceCenterController::setEqualizerPreset(int preset) { _send("eqPreset", {{"preset",preset}}); }
void DeviceCenterController::setEqualizerCustom(int bass, const QVariantList& bands) {
    _send("eqCustom", {{"clearBass",bass},{"bands",QJsonArray::fromVariantList(bands)}});
}
void DeviceCenterController::setDsee(bool enabled) { _send("dsee", {{"enabled",enabled}}); }
void DeviceCenterController::setSpeakToChat(bool enabled) { _send("speakToChat", {{"enabled",enabled}}); }
void DeviceCenterController::setAdaptiveVolume(bool enabled) { _send("adaptiveVolume", {{"enabled",enabled}}); }
void DeviceCenterController::setAutoPowerOff(int index) { _send("autoPowerOff", {{"index",index}}); }
void DeviceCenterController::connectDevice(const QString& address, const QString& name) { _send("connect", {{"address",address},{"name",name}}); }
void DeviceCenterController::disconnectDevice() { _send("disconnect"); }
void DeviceCenterController::refreshDiscoveredDevices() {
    QMetaObject::invokeMethod(_backend, &DeviceBackend::discover, Qt::QueuedConnection);
}

bool DeviceCenterController::autostart() const {
#if defined(Q_OS_LINUX)
    QString autostartDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart";
    QString desktopFile = autostartDir + "/sony-device-center.desktop";
    return QFileInfo::exists(desktopFile);
#elif defined(Q_OS_WIN)
    QSettings bootSettings("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
    return bootSettings.contains("SonyDeviceCenter");
#else
    return false;
#endif
}

void DeviceCenterController::setAutostart(bool enable) {
#if defined(Q_OS_LINUX)
    QString autostartDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart";
    QString desktopFile = autostartDir + "/sony-device-center.desktop";
    if (enable) {
        QDir().mkpath(autostartDir);
        QFile file(desktopFile);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&file);
            out << "[Desktop Entry]\n";
            out << "Type=Application\n";
            out << "Name=Sony Device Center\n";
            out << "Comment=Unofficial open-source companion for Sony WH/WF/LinkBuds audio devices\n";
            out << "Exec=" << QCoreApplication::applicationFilePath() << "\n";
            out << "Icon=sony-device-center\n";
            out << "Terminal=false\n";
            out << "Categories=Audio;AudioVideo;Settings;\n";
            out << "X-GNOME-Autostart-enabled=true\n";
        }
    } else {
        QFile::remove(desktopFile);
    }
#elif defined(Q_OS_WIN)
    QSettings bootSettings("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
    if (enable) {
        QString appPath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        bootSettings.setValue("SonyDeviceCenter", "\"" + appPath + "\"");
    } else {
        bootSettings.remove("SonyDeviceCenter");
    }
#endif
    emit autostartChanged();
}

QString DeviceCenterController::appVersion() const {
    return QCoreApplication::applicationVersion();
}

QString DeviceCenterController::currentLanguage() const {
    return _currentLanguage;
}

QVariantList DeviceCenterController::availableLanguages() const {
    return I18nManager::instance().availableLanguages();
}

void DeviceCenterController::setLanguage(const QString& langCode) {
    if (_currentLanguage != langCode) {
        _currentLanguage = langCode;
        QSettings settings("SonyBridge", "SonyDeviceCenter");
        settings.setValue("language", langCode);
        emit languageChanged();
    }
}

QString DeviceCenterController::t(const QString& key) const {
    return I18nManager::instance().translate(key, _currentLanguage);
}

void DeviceCenterController::openUrl(const QString& url) {
    QDesktopServices::openUrl(QUrl(url));
}

} // namespace sony::devicecenter
