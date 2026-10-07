#pragma once

#include "IProtocol.h"
#include "SonyProtocolSession.h"
#include <mutex>

namespace sony::protocol {

class ProtocolV2 : public IProtocol {
public:
    // tenBandEqualizer selects the newer 10-band equalizer wire format
    // (inquired type 0x04, no separate Clear Bass slot) used by devices such
    // as the WH-1000XM6, instead of the legacy 5-band + Clear Bass format
    // (inquired type 0x00) shared with ProtocolV1. See DeviceProfile.h and
    // issue #10. noiseControlType19 selects inquired type 0x19 for
    // noise-control readback (DeviceCapabilities::noiseControlType19).
    explicit ProtocolV2(SonyProtocolSession& session, bool tenBandEqualizer = false,
                        bool noiseControlType19 = false);
    ~ProtocolV2() override = default;

    [[nodiscard]] ProtocolGeneration generation() const noexcept override {
        return ProtocolGeneration::V2;
    }

    void initDevice() override;

    // V2 uses opcode 0x22 for battery inquiries
    BatteryState getBattery() override;

    NoiseControlState getNoiseControl() override;
    void setNoiseControl(const NoiseControlState& state) override;

    EqualizerState getEqualizer() override;
    void setEqualizerPreset(int preset) override;
    void setEqualizerCustom(int clearBass, const std::vector<int>& bands) override;

    bool getDsee() override;
    void setDsee(bool enabled) override;

    std::string getFirmwareVersion() override;
    std::string getCodec() override;

    int getAutoPowerOff() override;
    void setAutoPowerOff(int index) override;

    bool getSpeakToChat() override;
    void setSpeakToChat(bool enabled) override;

    bool getAdaptiveVolume() override;
    void setAdaptiveVolume(bool enabled) override;

private:
    SonyProtocolSession& _session;
    std::mutex _mutex;
    bool _tenBandEqualizer;
    bool _noiseControlType19;
};

} // namespace sony::protocol
