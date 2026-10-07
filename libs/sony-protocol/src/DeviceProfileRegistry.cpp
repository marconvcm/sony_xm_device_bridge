#include "sony/protocol/DeviceProfileRegistry.h"
#include <algorithm>
#include <cctype>

namespace sony::protocol {

namespace {

std::string toUpper(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (char c : input) {
        result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return result;
}

const std::vector<DeviceProfile>& getStaticProfiles() {
    static const std::vector<DeviceProfile> profiles = {
        // WH-1000XM3 (V1 protocol, ANC/Ambient, Single battery)
        DeviceProfile{
            .model = SonyModel::WH1000XM3,
            .protocol = SonyProtocolVersion::V1,
            .capabilities = DeviceCapabilities{
                .battery = true,
                .dualBattery = false,
                .noiseCancelling = true,
                .ambientSound = true,
                .focusOnVoice = true,
                .equalizer = false,
                .clearBass = false,
                .tenBandEqualizer = false,
                .dsee = false,
                .speakToChat = false,
                .adaptiveVolume = false,
                .autoPowerOff = false,
                .firmwareInfo = false,
                .codecInfo = false,
                .wearSensor = false,
                .multipoint = false
            }
        },
        // WH-1000XM4 (V1 protocol, ANC/Ambient, Single battery, wear sensor, multipoint)
        // Battery, EQ + Clear Bass, firmware and codec readback verified on
        // hardware (firmware 3.0.1) over the legacy V1 opcodes. Speak-to-Chat is
        // Smart Talking Mode (F6 05), not the V2 subtype 0x0c.
        DeviceProfile{
            .model = SonyModel::WH1000XM4,
            .protocol = SonyProtocolVersion::V1,
            .capabilities = DeviceCapabilities{
                .battery = true,
                .dualBattery = false,
                .noiseCancelling = true,
                .ambientSound = true,
                .focusOnVoice = true,
                .equalizer = true,
                .clearBass = true,
                .tenBandEqualizer = false,
                .dsee = false,
                .speakToChat = true,
                .adaptiveVolume = false,
                .autoPowerOff = false,
                .firmwareInfo = true,
                .codecInfo = true,
                .wearSensor = true,
                .multipoint = true
            }
        },
        // WH-1000XM5 (V2 protocol, Full capability set)
        DeviceProfile{
            .model = SonyModel::WH1000XM5,
            .protocol = SonyProtocolVersion::V2,
            .capabilities = DeviceCapabilities{
                .battery = true,
                .dualBattery = false,
                .noiseCancelling = true,
                .ambientSound = true,
                .focusOnVoice = true,
                .equalizer = true,
                .clearBass = true,
                .tenBandEqualizer = false,
                .dsee = true,
                .speakToChat = true,
                .adaptiveVolume = true,
                .autoPowerOff = true,
                .firmwareInfo = true,
                .codecInfo = true,
                .wearSensor = true,
                .multipoint = true
            }
        },
        // WH-1000XM6 (V2 protocol, 10-band equalizer -- no separate Clear
        // Bass control; see issue #10 and DeviceCapabilities::tenBandEqualizer)
        DeviceProfile{
            .model = SonyModel::WH1000XM6,
            .protocol = SonyProtocolVersion::V2,
            .capabilities = DeviceCapabilities{
                .battery = true,
                .dualBattery = false,
                .noiseCancelling = true,
                .ambientSound = true,
                .focusOnVoice = true,
                .equalizer = true,
                .clearBass = false,
                .tenBandEqualizer = true,
                .noiseControlType19 = true,
                .dsee = true,
                .speakToChat = true,
                .adaptiveVolume = true,
                .autoPowerOff = true,
                .firmwareInfo = true,
                .codecInfo = true,
                .wearSensor = true,
                .multipoint = true
            }
        },
        // WF-1000XM4 (V2 protocol, TWS dual battery + case, speak to chat, wear sensor)
        DeviceProfile{
            .model = SonyModel::WF1000XM4,
            .protocol = SonyProtocolVersion::V2,
            .capabilities = DeviceCapabilities{
                .battery = true,
                .dualBattery = true,
                .noiseCancelling = true,
                .ambientSound = true,
                .focusOnVoice = true,
                .equalizer = true,
                .clearBass = true,
                .tenBandEqualizer = false,
                .dsee = true,
                .speakToChat = true,
                .adaptiveVolume = false,
                .autoPowerOff = true,
                .firmwareInfo = true,
                .codecInfo = true,
                .wearSensor = true,
                .multipoint = true
            }
        },
        // WF-1000XM5 (V2 protocol, TWS dual battery + case, adaptive volume, wear sensor)
        DeviceProfile{
            .model = SonyModel::WF1000XM5,
            .protocol = SonyProtocolVersion::V2,
            .capabilities = DeviceCapabilities{
                .battery = true,
                .dualBattery = true,
                .noiseCancelling = true,
                .ambientSound = true,
                .focusOnVoice = true,
                .equalizer = true,
                .clearBass = true,
                .tenBandEqualizer = false,
                .dsee = true,
                .speakToChat = true,
                .adaptiveVolume = true,
                .autoPowerOff = true,
                .firmwareInfo = true,
                .codecInfo = true,
                .wearSensor = true,
                .multipoint = true
            }
        },
        // WF-1000XM6 (V2 protocol, TWS flagship)
        DeviceProfile{
            .model = SonyModel::WF1000XM6,
            .protocol = SonyProtocolVersion::V2,
            .capabilities = DeviceCapabilities{
                .battery = true,
                .dualBattery = true,
                .noiseCancelling = true,
                .ambientSound = true,
                .focusOnVoice = true,
                .equalizer = true,
                .clearBass = true,
                .tenBandEqualizer = false,
                .dsee = true,
                .speakToChat = true,
                .adaptiveVolume = true,
                .autoPowerOff = true,
                .firmwareInfo = true,
                .codecInfo = true,
                .wearSensor = true,
                .multipoint = true
            }
        },

        // WH-CH720N (V2 protocol, over-ear, EQ, DSEE, multipoint)
        DeviceProfile{
            .model = SonyModel::WHCH720N,
            .protocol = SonyProtocolVersion::V2,
            .capabilities = DeviceCapabilities{
                .battery = true,
                .dualBattery = false,
                .noiseCancelling = true,
                .ambientSound = true,
                .focusOnVoice = true,
                .equalizer = true,
                .clearBass = true,
                .tenBandEqualizer = false,
                .dsee = true,
                .speakToChat = false,
                .adaptiveVolume = false,
                .autoPowerOff = true,
                .firmwareInfo = true,
                .codecInfo = true,
                .wearSensor = false,
                .multipoint = true
            }
        },
        // ULT WEAR / WH-ULT900N (V2 protocol, over-ear, wear sensor, multipoint)
        DeviceProfile{
            .model = SonyModel::ULTWear,
            .protocol = SonyProtocolVersion::V2,
            .capabilities = DeviceCapabilities{
                .battery = true,
                .dualBattery = false,
                .noiseCancelling = true,
                .ambientSound = true,
                .focusOnVoice = true,
                .equalizer = true,
                .clearBass = true,
                .tenBandEqualizer = false,
                .dsee = true,
                .speakToChat = false,
                .adaptiveVolume = false,
                .autoPowerOff = true,
                .firmwareInfo = true,
                .codecInfo = true,
                .wearSensor = true,
                .multipoint = true
            }
        },
        // LinkBuds S / WF-LS900N (V2 protocol, TWS dual battery, speak-to-chat, wear sensor)
        DeviceProfile{
            .model = SonyModel::LinkBudsS,
            .protocol = SonyProtocolVersion::V2,
            .capabilities = DeviceCapabilities{
                .battery = true,
                .dualBattery = true,
                .noiseCancelling = true,
                .ambientSound = true,
                .focusOnVoice = true,
                .equalizer = true,
                .clearBass = true,
                .tenBandEqualizer = false,
                .dsee = true,
                .speakToChat = true,
                .adaptiveVolume = false,
                .autoPowerOff = true,
                .firmwareInfo = true,
                .codecInfo = true,
                .wearSensor = true,
                .multipoint = true
            }
        }
    };
    return profiles;
}

} // namespace

SonyModel DeviceProfileRegistry::identifyModel(std::string_view deviceName) noexcept {
    if (deviceName.empty()) {
        return SonyModel::Unknown;
    }

    std::string upper = toUpper(deviceName);

    if (upper.find("WH-1000XM3") != std::string::npos || upper.find("WH1000XM3") != std::string::npos) {
        return SonyModel::WH1000XM3;
    }
    if (upper.find("WH-1000XM4") != std::string::npos || upper.find("WH1000XM4") != std::string::npos) {
        return SonyModel::WH1000XM4;
    }
    if (upper.find("WH-1000XM5") != std::string::npos || upper.find("WH1000XM5") != std::string::npos) {
        return SonyModel::WH1000XM5;
    }
    if (upper.find("WH-1000XM6") != std::string::npos || upper.find("WH1000XM6") != std::string::npos) {
        return SonyModel::WH1000XM6;
    }
    if (upper.find("WF-1000XM4") != std::string::npos || upper.find("WF1000XM4") != std::string::npos) {
        return SonyModel::WF1000XM4;
    }
    if (upper.find("WF-1000XM5") != std::string::npos || upper.find("WF1000XM5") != std::string::npos) {
        return SonyModel::WF1000XM5;
    }
    if (upper.find("WF-1000XM6") != std::string::npos || upper.find("WF1000XM6") != std::string::npos) {
        return SonyModel::WF1000XM6;
    }

    if (upper.find("WH-CH720N") != std::string::npos || upper.find("CH720N") != std::string::npos) {
        return SonyModel::WHCH720N;
    }
    if (upper.find("ULT WEAR") != std::string::npos || upper.find("WH-ULT900N") != std::string::npos || upper.find("ULT900N") != std::string::npos) {
        return SonyModel::ULTWear;
    }
    if (upper.find("LINKBUDS S") != std::string::npos || upper.find("WF-LS900N") != std::string::npos || upper.find("LS900N") != std::string::npos) {
        return SonyModel::LinkBudsS;
    }

    return SonyModel::Unknown;
}

std::optional<DeviceProfile> DeviceProfileRegistry::getProfile(SonyModel model) noexcept {
    if (model == SonyModel::Unknown) {
        return std::nullopt;
    }

    const auto& profiles = getStaticProfiles();
    for (const auto& profile : profiles) {
        if (profile.model == model) {
            return profile;
        }
    }
    return std::nullopt;
}

DeviceProfile DeviceProfileRegistry::getProfileForDevice(std::string_view deviceName) noexcept {
    SonyModel model = identifyModel(deviceName);
    auto known = getProfile(model);
    if (known.has_value()) {
        return *known;
    }

    // Fallback profile for unknown devices (probed dynamically)
    return DeviceProfile{
        .model = SonyModel::Unknown,
        .protocol = SonyProtocolVersion::V1,
        .capabilities = DeviceCapabilities{}
    };
}

bool DeviceProfileRegistry::isKnownDevice(SonyModel model) noexcept {
    return model != SonyModel::Unknown;
}

const std::vector<DeviceProfile>& DeviceProfileRegistry::allProfiles() noexcept {
    return getStaticProfiles();
}

} // namespace sony::protocol
