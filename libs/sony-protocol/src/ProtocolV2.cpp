#include "sony/protocol/ProtocolV2.h"
#include "ProtocolHelpers.h"
#include <algorithm>

namespace sony::protocol {

using detail::clampEqValue;
using detail::clampEqValueRaw;
using detail::codecName;

namespace {

// Auto-power-off codes: 0=Off, 1=5min, 2=30min, 3=1h, 4=3h, 5=when-taken-off
const std::pair<uint8_t, uint8_t> APO_CODES[] = {
    { 0x11, 0x00 }, { 0x00, 0x00 }, { 0x01, 0x01 }, { 0x02, 0x02 }, { 0x03, 0x03 }, { 0x10, 0x00 }
};

int apoIndexFromCode(uint8_t c0, uint8_t c1) {
    for (int i = 0; i < 6; i++) {
        if (APO_CODES[i].first == c0 && APO_CODES[i].second == c1) {
            return i;
        }
    }
    return 0;
}

} // namespace

ProtocolV2::ProtocolV2(SonyProtocolSession& session, bool tenBandEqualizer, bool noiseControlType19)
    : _session(session), _tenBandEqualizer(tenBandEqualizer), _noiseControlType19(noiseControlType19) {}

void ProtocolV2::initDevice() {
    // V2 handshake init: 0x00 0x00 -> RET 0x01
    try {
        _session.sendAndAwaitResponse(
            SonyFrame{ .type = DataType::DataMdr, .payload = {0x00, 0x00} },
            0x01,
            -1,
            std::chrono::milliseconds(1000)
        );
    } catch (const SonyException&) {}
}

BatteryState ProtocolV2::getBattery() {
    // V2 battery request uses opcode 0x22
    BatteryState state;

    // 1. Single battery (over-ear / main): GET 22 00 -> RET 23 00 <level> <charging>
    try {
        auto resp = _session.sendAndAwaitResponse(
            SonyFrame{ .type = DataType::DataMdr, .payload = {0x22, 0x00} },
            0x23,
            0x00,
            std::chrono::milliseconds(1000)
        );
        if (resp.payload.size() >= 4) {
            state.main = static_cast<int>(resp.payload[2]);
            state.charging = (resp.payload[3] == 1);
            return state;
        }
    } catch (const SonyException&) {}

    // 2. Dual L/R battery for TWS earbuds: GET 22 09 -> RET 23 09 <Llvl> <Lchg> <Rlvl> <Rchg>
    try {
        auto resp = _session.sendAndAwaitResponse(
            SonyFrame{ .type = DataType::DataMdr, .payload = {0x22, 0x09} },
            0x23,
            0x09,
            std::chrono::milliseconds(1000)
        );
        if (resp.payload.size() >= 6) {
            state.left = static_cast<int>(resp.payload[2]);
            state.right = static_cast<int>(resp.payload[4]);
            state.main = std::min(*state.left, *state.right);
            state.charging = (resp.payload[3] == 1 || resp.payload[5] == 1);
        }
    } catch (const SonyException&) {}

    // 3. Case battery for TWS earbuds: GET 22 0a -> RET 23 0a <level> <chg>
    try {
        auto resp = _session.sendAndAwaitResponse(
            SonyFrame{ .type = DataType::DataMdr, .payload = {0x22, 0x0a} },
            0x23,
            0x0a,
            std::chrono::milliseconds(1000)
        );
        if (resp.payload.size() >= 4) {
            state.caseBattery = static_cast<int>(resp.payload[2]);
        }
    } catch (const SonyException&) {}

    return state;
}

NoiseControlState ProtocolV2::getNoiseControl() {
    // GET: 66 17 -> RET: 67 17 01 <effect> <settingType 0=NC/1=Ambient> <voice> <level>
    // Type 0x19 devices (WH-1000XM6) use the same first seven bytes plus two trailing bytes.
    const uint8_t inquired = _noiseControlType19 ? 0x19 : 0x17;
    auto resp = _session.sendAndAwaitResponse(
        SonyFrame{ .type = DataType::DataMdr, .payload = {0x66, inquired} },
        0x67,
        -1,
        std::chrono::milliseconds(1000)
    );

    if (resp.payload.size() < 7 || resp.payload[1] != inquired || resp.payload[2] != 1)
        throw SonyException(SonyErrorCode::InvalidResponse, "Malformed noise-control response");
    NoiseControlState state;
    if (resp.payload.size() >= 7) {
        bool on = resp.payload[3] != 0;
        bool ambient = resp.payload[4] != 0;
        bool voice = resp.payload[5] != 0;
        int level = static_cast<int>(resp.payload[6]);

        if (!on) {
            state.mode = NoiseControlMode::Off;
        } else if (ambient) {
            state.mode = NoiseControlMode::Ambient;
        } else {
            state.mode = NoiseControlMode::NoiseCancelling;
        }
        state.ambientLevel = ambient ? level : 0;
        state.focusOnVoice = voice;
    }
    return state;
}

void ProtocolV2::setNoiseControl(const NoiseControlState& state) {
    uint8_t effect = (state.mode == NoiseControlMode::Off) ? 0 : 1;
    uint8_t settingType = (state.mode == NoiseControlMode::Ambient) ? 1 : 0;
    uint8_t voice = state.focusOnVoice ? 1 : 0;
    uint8_t level = static_cast<uint8_t>(state.ambientLevel > 0 ? state.ambientLevel : 1);

    std::vector<uint8_t> payload = {
        0x68,
        0x17,
        0x01,
        effect,
        settingType,
        voice,
        level
    };

    _session.send(SonyFrame{ .type = DataType::DataMdr, .payload = std::move(payload) });
}

EqualizerState ProtocolV2::getEqualizer() {
    // Legacy: GET 56 00 -> RET 57 00 <preset> 06 <bass+10> <b1..b5 +10>
    // 10-band (e.g. WH-1000XM6): GET 56 04 -> RET 57 04 <preset> 0a <b1..b10>,
    // no separate Clear Bass slot. Reverse-engineered from a Sound Connect
    // btsnoop capture -- see issue #10.
    const uint8_t inquired = _tenBandEqualizer ? 0x04 : 0x00;
    auto resp = _session.sendAndAwaitResponse(
        SonyFrame{ .type = DataType::DataMdr, .payload = {0x56, inquired} },
        0x57,
        -1,
        std::chrono::milliseconds(1000)
    );

    if (resp.payload.size() < 4 || resp.payload[1] != inquired)
        throw SonyException(SonyErrorCode::InvalidResponse, "Incomplete equalizer response");
    EqualizerState state;
    state.preset = static_cast<int>(resp.payload[2]);
    const size_t count = resp.payload[3];
    if (resp.payload.size() < 4 + count)
        throw SonyException(SonyErrorCode::InvalidResponse, "Truncated equalizer response");
    if (_tenBandEqualizer) {
        // <preset> 0a <b1..b10>, raw units, no bias.
        state.clearBass = 0;
        state.bands.assign(count, 0);
        for (size_t i = 0; i < count; ++i) {
            state.bands[i] = static_cast<int>(resp.payload[4 + i]);
        }
    } else if (count >= 1) {
        // <preset> 06 <bass+10> <b1..b5 +10>
        state.clearBass = static_cast<int>(resp.payload[4]) - 10;
        state.bands.assign(count - 1, 0);
        for (size_t i = 0; i + 1 < count; ++i) {
            state.bands[i] = static_cast<int>(resp.payload[5 + i]) - 10;
        }
    }
    return state;
}

void ProtocolV2::setEqualizerPreset(int preset) {
    // SET preset: 58 <inquired> <preset> 00 -- same shape on both layouts,
    // only the inquired-type byte differs.
    std::vector<uint8_t> payload = {
        0x58,
        static_cast<uint8_t>(_tenBandEqualizer ? 0x04 : 0x00),
        static_cast<uint8_t>(preset),
        0x00
    };
    _session.send(SonyFrame{ .type = DataType::DataMdr, .payload = std::move(payload) });
}

void ProtocolV2::setEqualizerCustom(int clearBass, const std::vector<int>& bands) {
    if (_tenBandEqualizer) {
        // SET custom: 58 04 A0 <bands.size()> <b1..bN>, raw units, no bias,
        // no separate Clear Bass slot (clearBass is ignored).
        std::vector<uint8_t> payload = {
            0x58,
            0x04,
            0xa0,
            static_cast<uint8_t>(bands.size())
        };
        for (int b : bands) {
            payload.push_back(clampEqValueRaw(b));
        }
        _session.send(SonyFrame{ .type = DataType::DataMdr, .payload = std::move(payload) });
        return;
    }
    // SET custom: 58 00 A0 <1+bands.size()> <clearBass+10> <b1..bN +10>
    std::vector<uint8_t> payload = {
        0x58,
        0x00,
        0xa0,
        static_cast<uint8_t>(1 + bands.size()),
        clampEqValue(clearBass)
    };
    for (int b : bands) {
        payload.push_back(clampEqValue(b));
    }
    _session.send(SonyFrame{ .type = DataType::DataMdr, .payload = std::move(payload) });
}

bool ProtocolV2::getDsee() {
    // GET: e6 01 -> RET: e7 01 <enabled 0/1>
    auto resp = _session.sendAndAwaitResponse(
        SonyFrame{ .type = DataType::DataMdr, .payload = {0xe6, 0x01} },
        0xe7,
        0x01,
        std::chrono::milliseconds(1000)
    );
    if (resp.payload.size() >= 3) {
        return resp.payload[2] != 0;
    }
    throw SonyException(SonyErrorCode::InvalidResponse, "Incomplete Dsee response");
}

void ProtocolV2::setDsee(bool enabled) {
    // SET: e8 01 <enabled 0/1>
    std::vector<uint8_t> payload = {
        0xe8,
        0x01,
        static_cast<uint8_t>(enabled ? 0x01 : 0x00)
    };
    _session.send(SonyFrame{ .type = DataType::DataMdr, .payload = std::move(payload) });
}

std::string ProtocolV2::getFirmwareVersion() {
    // GET: 04 02 -> RET: 05 02 <ascii version...>
    auto resp = _session.sendAndAwaitResponse(
        SonyFrame{ .type = DataType::DataMdr, .payload = {0x04, 0x02} },
        0x05,
        -1,
        std::chrono::milliseconds(1000)
    );
    if (resp.payload.size() > 3) {
        return std::string(resp.payload.begin() + 3, resp.payload.end());
    }
    return "";
}

std::string ProtocolV2::getCodec() {
    // GET: 12 02 -> RET: 13 02 <codec>
    auto resp = _session.sendAndAwaitResponse(
        SonyFrame{ .type = DataType::DataMdr, .payload = {0x12, 0x02} },
        0x13,
        -1,
        std::chrono::milliseconds(1000)
    );
    if (resp.payload.size() >= 3) {
        return codecName(resp.payload[2]);
    }
    return "";
}

int ProtocolV2::getAutoPowerOff() {
    // GET: 26 05 -> RET: 27 05 <c0> <c1>
    auto resp = _session.sendAndAwaitResponse(
        SonyFrame{ .type = DataType::DataMdr, .payload = {0x26, 0x05} },
        0x27,
        -1,
        std::chrono::milliseconds(1000)
    );
    if (resp.payload.size() >= 4) {
        return apoIndexFromCode(resp.payload[2], resp.payload[3]);
    }
    throw SonyException(SonyErrorCode::InvalidResponse, "Incomplete AutoPowerOff response");
}

void ProtocolV2::setAutoPowerOff(int index) {
    if (index < 0 || index >= 6) {
        return;
    }
    // SET: 28 05 <c0> <c1>
    std::vector<uint8_t> payload = {
        0x28,
        0x05,
        APO_CODES[index].first,
        APO_CODES[index].second
    };
    _session.send(SonyFrame{ .type = DataType::DataMdr, .payload = std::move(payload) });
}

bool ProtocolV2::getSpeakToChat() {
    // GET: f6 0c -> RET: f7 0c <inverted_enabled>
    auto resp = _session.sendAndAwaitResponse(
        SonyFrame{ .type = DataType::DataMdr, .payload = {0xf6, 0x0c} },
        0xf7,
        0x0c,
        std::chrono::milliseconds(1000)
    );
    if (resp.payload.size() >= 3) {
        return resp.payload[2] == 0;
    }
    throw SonyException(SonyErrorCode::InvalidResponse, "Incomplete SpeakToChat response");
}

void ProtocolV2::setSpeakToChat(bool enabled) {
    // SET: f8 0c <inverted_enabled> 01
    std::vector<uint8_t> payload = {
        0xf8,
        0x0c,
        static_cast<uint8_t>(enabled ? 0x00 : 0x01),
        0x01
    };
    _session.send(SonyFrame{ .type = DataType::DataMdr, .payload = std::move(payload) });
}

bool ProtocolV2::getAdaptiveVolume() {
    // GET: f6 0a -> RET: f7 0a <inverted_enabled>
    auto resp = _session.sendAndAwaitResponse(
        SonyFrame{ .type = DataType::DataMdr, .payload = {0xf6, 0x0a} },
        0xf7,
        0x0a,
        std::chrono::milliseconds(1000)
    );
    if (resp.payload.size() >= 3) {
        return resp.payload[2] == 0;
    }
    throw SonyException(SonyErrorCode::InvalidResponse, "Incomplete AdaptiveVolume response");
}

void ProtocolV2::setAdaptiveVolume(bool enabled) {
    // SET: f8 0a <inverted_enabled>
    std::vector<uint8_t> payload = {
        0xf8,
        0x0a,
        static_cast<uint8_t>(enabled ? 0x00 : 0x01)
    };
    _session.send(SonyFrame{ .type = DataType::DataMdr, .payload = std::move(payload) });
}

} // namespace sony::protocol
