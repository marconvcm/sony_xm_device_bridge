#include "sony/core/SonyDevice.h"
#include "sony/protocol/DeviceProfileRegistry.h"
#include "sony/protocol/ProtocolV1.h"
#include "sony/protocol/ProtocolV2.h"
#include "sony/transport/Logger.h"

namespace sony::core {

SonyDevice::SonyDevice(
    std::shared_ptr<transport::ITransport> transport,
    SonyProtocolVersion version)
    : _transport(std::move(transport)), _version(version) {
    auto defaultProf = protocol::DeviceProfileRegistry::getProfile(protocol::SonyModel::Unknown);
    _profile = defaultProf.value_or(protocol::DeviceProfile{});
    _capabilities = _profile.capabilities;
    _name = std::string(to_string(_profile.model));
}

SonyDevice::~SonyDevice() {
    disconnect();
}

void SonyDevice::connect(const transport::DeviceAddress& address, std::string_view deviceName) {
    if (!_transport) {
        throw SonyException(SonyErrorCode::TransportFailure, "No transport configured");
    }

    if (isConnected()) {
        disconnect();
    }

    // Resolve the generation on every connect, including when no name is
    // supplied. An unknown name yields the V1 fallback profile, which is the
    // safe direction: opcode 0x22 requests the battery on V2 but means POWER
    // OFF on V1, so a V1 device driven as V2 switches itself off. Carrying a
    // stale V2 version over from a previously connected device would do the
    // same, which is why this no longer runs only for a non-empty name.
    _profile = protocol::DeviceProfileRegistry::getProfileForDevice(deviceName);
    _capabilities = _profile.capabilities;
    _version = _profile.protocol;
    _name = deviceName.empty() ? std::string(to_string(_profile.model))
                               : std::string(deviceName);

    // Tear the previous session down *before* opening the new connection.
    // ~SonyProtocolSession disconnects the transport, and assigning over the
    // unique_ptr in _setupSession() destroys the old session only after the new
    // one exists — which would drop the link that was just established. This is
    // what makes reconnecting, or switching to a second device, work at all.
    _protocol.reset();
    _session.reset();

    {
        std::lock_guard lock(_stateMutex);
        _state = {}; _refreshStep = 0;
        const auto& c = _capabilities;
        for (const auto& [name, supported] : std::initializer_list<std::pair<std::string, bool>>{
            {"battery",c.battery},{"noiseControl",c.noiseCancelling || c.ambientSound},
            {"equalizer",c.equalizer},{"dsee",c.dsee},{"codec",c.codecInfo},{"firmware",c.firmwareInfo},
            {"speakToChat",c.speakToChat},{"adaptiveVolume",c.adaptiveVolume},{"autoPowerOff",c.autoPowerOff}})
            _state.features[name].availability = supported ? "unknown" : "unsupported";
    }
    _transport->connect(address);
    _setupSession();

    if (_protocol) {
        try {
            _protocol->initDevice();
        } catch (const SonyException& ex) {
            Logger::warn(LogCategory::Device, "Device init handshake failed: " + std::string(ex.what()));
        }
    }

    refreshAll();

    _dispatcher.dispatch(protocol::ConnectionChanged{
        .connected = true,
        .deviceAddress = address.str()
    });
}

void SonyDevice::disconnect() noexcept {
    if (_session) {
        _session->disconnect();
    }
    if (_transport && _transport->isConnected()) {
        _transport->disconnect();
    }
    {
        std::lock_guard lock(_stateMutex);
        for (auto& [name, status] : _state.features)
            if (status.availability == "valid") status.availability = "stale";
    }
    _dispatcher.dispatch(protocol::ConnectionChanged{
        .connected = false,
        .deviceAddress = ""
    });
}

bool SonyDevice::isConnected() const noexcept {
    return _session && _session->isConnected();
}

SonyProtocolVersion SonyDevice::protocolVersion() const noexcept {
    return _version;
}

const std::string& SonyDevice::name() const noexcept {
    return _name;
}

const protocol::DeviceProfile& SonyDevice::profile() const noexcept {
    return _profile;
}

const protocol::DeviceCapabilities& SonyDevice::capabilities() const noexcept {
    return _capabilities;
}

protocol::DeviceState SonyDevice::state() const {
    std::lock_guard lock(_stateMutex);
    return _state;
}

protocol::DeviceStateSnapshot SonyDevice::snapshot() const {
    std::lock_guard lock(_stateMutex);
    return std::make_shared<const protocol::DeviceState>(_state);
}

protocol::DeviceEventDispatcher& SonyDevice::events() noexcept {
    return _dispatcher;
}

void SonyDevice::_setupSession() {
    _session = std::make_unique<protocol::SonyProtocolSession>(_transport.get());
    if (_version == SonyProtocolVersion::V1) {
        _protocol = std::make_unique<protocol::ProtocolV1>(*_session);
    } else {
        _protocol = std::make_unique<protocol::ProtocolV2>(*_session, _capabilities.tenBandEqualizer,
                                                           _capabilities.noiseControlType19);
    }

    _session->onNotification([this](const protocol::SonyFrame& frame) {
        _onNotification(frame);
    });

    _session->start();
}

void SonyDevice::_onNotification(const protocol::SonyFrame& frame) {
    protocol::DeviceStateSnapshot updated;
    std::string feature;
    {
        std::lock_guard lock(_stateMutex);
        if (!_dispatcher.parseNotificationPayload(frame.payload, _state, false)) return;
        const auto opcode = frame.payload[0];
        feature = (opcode == 0x23 || opcode == 0x25) ? "battery" :
            (opcode == 0x67 || opcode == 0x69) ? "noiseControl" : "equalizer";
        _markSuccess(feature);
        updated = std::make_shared<const protocol::DeviceState>(_state);
    }
    if (feature == "battery") _dispatcher.dispatch(protocol::BatteryChanged{updated->battery});
    if (feature == "noiseControl") _dispatcher.dispatch(protocol::NoiseControlChanged{updated->noiseControl});
    if (feature == "equalizer") _dispatcher.dispatch(protocol::EqualizerChanged{updated->equalizer});
    _dispatcher.dispatch(protocol::DeviceStateChanged{updated});
}

void SonyDevice::refreshAll() {
    refreshBattery();
    refreshNoiseControl();
    refreshEqualizer();
    refreshDsee();
}

void SonyDevice::refreshBattery() {
    if (!_protocol) return;
    if (!_capabilities.battery) return;
    try {
        auto bat = _protocol->getBattery();
        if (!bat.main && !bat.left && !bat.right && !bat.caseBattery)
            throw SonyException(SonyErrorCode::InvalidResponse, "No battery reading received");
        {
            std::lock_guard lock(_stateMutex);
            _state.battery = bat;
        _markSuccess("battery");
        }
        _dispatcher.dispatch(protocol::BatteryChanged{bat});
        _dispatcher.dispatch(protocol::DeviceStateChanged{snapshot()});
    } catch (const SonyException& ex) {
        _markError("battery", ex);
        Logger::debug(LogCategory::Device, "refreshBattery error: " + std::string(ex.what()));
    }
}

void SonyDevice::refreshNoiseControl() {
    if (!_protocol) return;
    if (!_capabilities.noiseCancelling && !_capabilities.ambientSound) return;
    try {
        auto nc = _protocol->getNoiseControl();
        {
            std::lock_guard lock(_stateMutex);
            _state.noiseControl = nc;
        _markSuccess("noiseControl");
        }
        _dispatcher.dispatch(protocol::NoiseControlChanged{nc});
        _dispatcher.dispatch(protocol::DeviceStateChanged{snapshot()});
    } catch (const SonyException& ex) {
        _markError("noiseControl", ex);
        Logger::debug(LogCategory::Device, "refreshNoiseControl error: " + std::string(ex.what()));
    }
}

void SonyDevice::refreshEqualizer() {
    if (!_protocol) return;
    if (!_capabilities.equalizer) return;
    try {
        auto eq = _protocol->getEqualizer();
        {
            std::lock_guard lock(_stateMutex);
            _state.equalizer = eq;
        _markSuccess("equalizer");
        }
        _dispatcher.dispatch(protocol::EqualizerChanged{eq});
        _dispatcher.dispatch(protocol::DeviceStateChanged{snapshot()});
    } catch (const SonyException& ex) {
        _markError("equalizer", ex);
        Logger::debug(LogCategory::Device, "refreshEqualizer error: " + std::string(ex.what()));
    }
}

void SonyDevice::refreshDsee() {
    if (!_protocol) return;
    if (!_capabilities.dsee) return;
    try {
        bool dsee = _protocol->getDsee();
        {
            std::lock_guard lock(_stateMutex);
            _state.dsee = dsee;
        _markSuccess("dsee");
        }
        _dispatcher.dispatch(protocol::DeviceStateChanged{snapshot()});
    } catch (const SonyException& ex) {
        _markError("dsee", ex);
        Logger::debug(LogCategory::Device, "refreshDsee error: " + std::string(ex.what()));
    }
}

void SonyDevice::setNoiseControl(const protocol::NoiseControlState& nc) {
    if (!_protocol) return;
    _protocol->setNoiseControl(nc);
    {
        std::lock_guard lock(_stateMutex);
        _state.noiseControl = nc;
        _markSuccess("noiseControl");
    }
    _dispatcher.dispatch(protocol::NoiseControlChanged{nc});
    _dispatcher.dispatch(protocol::DeviceStateChanged{snapshot()});
}

void SonyDevice::setAnc(bool enabled) {
    protocol::NoiseControlState nc;
    nc.mode = enabled ? protocol::NoiseControlMode::NoiseCancelling : protocol::NoiseControlMode::Off;
    nc.ambientLevel = 0;
    nc.focusOnVoice = false;
    setNoiseControl(nc);
}

void SonyDevice::setAmbient(int level, bool focusOnVoice) {
    protocol::NoiseControlState nc;
    nc.mode = protocol::NoiseControlMode::Ambient;
    nc.ambientLevel = level;
    nc.focusOnVoice = focusOnVoice;
    setNoiseControl(nc);
}

void SonyDevice::setEqualizerPreset(int preset) {
    if (preset < 0 || preset > 255) return;
    if (!_protocol) return;
    _protocol->setEqualizerPreset(preset);
    {
        std::lock_guard lock(_stateMutex);
        _state.equalizer.preset = preset;
        _markSuccess("equalizer");
    }
    _dispatcher.dispatch(protocol::EqualizerChanged{snapshot()->equalizer});
    _dispatcher.dispatch(protocol::DeviceStateChanged{snapshot()});
}

void SonyDevice::setEqualizerCustom(int clearBass, const std::vector<int>& bands) {
    if (!_protocol) return;
    _protocol->setEqualizerCustom(clearBass, bands);
    {
        std::lock_guard lock(_stateMutex);
        _state.equalizer.preset = 0xa0; // MANUAL
        _state.equalizer.clearBass = clearBass;
        _state.equalizer.bands = bands;
        _markSuccess("equalizer");
    }
    _dispatcher.dispatch(protocol::EqualizerChanged{snapshot()->equalizer});
    _dispatcher.dispatch(protocol::DeviceStateChanged{snapshot()});
}

void SonyDevice::setDsee(bool enabled) {
    if (!_protocol) return;
    _protocol->setDsee(enabled);
    {
        std::lock_guard lock(_stateMutex);
        _state.dsee = enabled;
        _markSuccess("dsee");
    }
    _dispatcher.dispatch(protocol::DeviceStateChanged{snapshot()});
}

void SonyDevice::setAutoPowerOff(int index) {
    if (!_protocol) return;
    _protocol->setAutoPowerOff(index);
    {
        std::lock_guard lock(_stateMutex);
        _state.autoPowerOff = index;
        _markSuccess("autoPowerOff");
    }
    _dispatcher.dispatch(protocol::DeviceStateChanged{snapshot()});
}

void SonyDevice::setSpeakToChat(bool enabled) {
    if (!_protocol) return;
    _protocol->setSpeakToChat(enabled);
    {
        std::lock_guard lock(_stateMutex);
        _state.speakToChat = enabled;
        _markSuccess("speakToChat");
    }
    _dispatcher.dispatch(protocol::DeviceStateChanged{snapshot()});
}

void SonyDevice::setAdaptiveVolume(bool enabled) {
    if (!_protocol) return;
    _protocol->setAdaptiveVolume(enabled);
    {
        std::lock_guard lock(_stateMutex);
        _state.adaptiveVolume = enabled;
        _markSuccess("adaptiveVolume");
    }
    _dispatcher.dispatch(protocol::DeviceStateChanged{snapshot()});
}

void SonyDevice::_markSuccess(const std::string& feature) {
    std::lock_guard lock(_stateMutex);
    _state.features[feature] = {"valid", protocol::stateTimestamp(), {}};
}
void SonyDevice::_markError(const std::string& feature, const SonyException& ex) {
    std::lock_guard lock(_stateMutex);
    auto& status = _state.features[feature];
    status.availability = ex.code() == SonyErrorCode::Unsupported ? "unsupported" : status.lastSuccessMs ? "stale" : "unknown";
    status.error = ex.what();
}
void SonyDevice::refreshSettingsStep() {
    if (!_protocol || !isConnected()) return;
    // Nine steps, each scheduled separately. Optional features are never probed
    // on profiles that don't advertise them.
    const auto step = _refreshStep++ % 9;
    if (step == 0) { refreshNoiseControl(); return; }
    if (step == 1) { refreshEqualizer(); return; }
    if (step == 2) { refreshDsee(); return; }
    std::string feature;
    try {
        if (step == 3 && _capabilities.codecInfo) {
            feature = "codec"; auto value = _protocol->getCodec();
            if (value.empty()) throw SonyException(SonyErrorCode::InvalidResponse, "Codec not reported");
            std::lock_guard lock(_stateMutex); _state.codec = value; _markSuccess(feature);
        } else if (step == 4 && _capabilities.firmwareInfo) {
            feature = "firmware"; auto value = _protocol->getFirmwareVersion();
            if (value.empty()) throw SonyException(SonyErrorCode::InvalidResponse, "Firmware not reported");
            std::lock_guard lock(_stateMutex); _state.firmware = value; _markSuccess(feature);
        } else if (step == 5 && _capabilities.speakToChat) {
            feature = "speakToChat"; auto value = _protocol->getSpeakToChat();
            std::lock_guard lock(_stateMutex); _state.speakToChat = value; _markSuccess(feature);
        } else if (step == 6 && _capabilities.adaptiveVolume) {
            feature = "adaptiveVolume"; auto value = _protocol->getAdaptiveVolume();
            std::lock_guard lock(_stateMutex); _state.adaptiveVolume = value; _markSuccess(feature);
        } else if (step == 7 && _capabilities.autoPowerOff) {
            feature = "autoPowerOff"; auto value = _protocol->getAutoPowerOff();
            std::lock_guard lock(_stateMutex); _state.autoPowerOff = value; _markSuccess(feature);
        }
    } catch (const SonyException& ex) { if (!feature.empty()) _markError(feature, ex); }
    _dispatcher.dispatch(protocol::DeviceStateChanged{snapshot()});
}
} // namespace sony::core
