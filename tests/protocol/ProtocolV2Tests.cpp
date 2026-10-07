#include <catch2/catch_test_macros.hpp>
#include "sony/protocol/ProtocolV2.h"
#include "sony/protocol/FrameCodec.h"
#include "sony/transport/FakeTransport.h"
#include "ReplyingFakeTransport.h"

using namespace sony;
using namespace sony::protocol;
using namespace sony::transport;
using sony::test::ReplyingFakeTransport;

TEST_CASE("ProtocolV2: uses opcode 0x22 for battery request", "[protocol][v2]")
{
    FakeTransport fake;
    SonyProtocolSession session(&fake);
    session.connect("11:22:33:44:55:66");

    ProtocolV2 v2(session);
    REQUIRE(v2.generation() == ProtocolGeneration::V2);

    // Reply for single battery: 23 00 80 01 (80%, charging)
    fake.queueIncoming(FrameCodec::encode(SonyFrame{ .type = DataType::Ack, .sequence = 0 }));
    fake.queueIncoming(FrameCodec::encode(SonyFrame{ .type = DataType::DataMdr, .sequence = 1, .payload = {0x23, 0x00, 80, 1} }));

    auto battery = v2.getBattery();
    REQUIRE(battery.main.has_value());
    REQUIRE(*battery.main == 80);
    REQUIRE(battery.charging == true);

    // Verify opcode 0x22 WAS sent
    bool found0x22 = false;
    for (const auto& frameBytes : fake.sentFrames()) {
        auto decoded = FrameCodec::decode(frameBytes);
        if (decoded.type == DataType::DataMdr && !decoded.payload.empty() && decoded.payload[0] == 0x22) {
            found0x22 = true;
            break;
        }
    }
    REQUIRE(found0x22);
}

TEST_CASE("ProtocolV2: handles noise control query and command", "[protocol][v2]")
{
    ReplyingFakeTransport fake;
    SonyProtocolSession session(&fake);
    session.connect("11:22:33:44:55:66");

    ProtocolV2 v2(session);

    SECTION("getNoiseControl decodes ambient mode")
    {
        // Reply: 67 17 01 <on=1> <ambient=1> <voice=0> <level=12>
        fake.queueIncoming(FrameCodec::encode(SonyFrame{ .type = DataType::Ack, .sequence = 0 }));
        fake.queueIncoming(FrameCodec::encode(SonyFrame{ .type = DataType::DataMdr, .sequence = 1, .payload = {0x67, 0x17, 0x01, 1, 1, 0, 12} }));

        auto nc = v2.getNoiseControl();
        REQUIRE(nc.mode == NoiseControlMode::Ambient);
        REQUIRE(nc.ambientLevel == 12);
        REQUIRE(nc.focusOnVoice == false);
    }

    SECTION("setNoiseControl sends V2 layout")
    {
        fake.queueReply({ SonyFrame{ .type = DataType::Ack, .sequence = 0 } });

        NoiseControlState state{
            .mode = NoiseControlMode::NoiseCancelling,
            .ambientLevel = 0,
            .focusOnVoice = false
        };
        v2.setNoiseControl(state);

        REQUIRE(fake.sentCount() == 1);
        auto sent = FrameCodec::decode(fake.lastSentFrame());
        REQUIRE(sent.payload == std::vector<uint8_t>{0x68, 0x17, 0x01, 1, 0, 0, 1});
    }
}

TEST_CASE("ProtocolV2: handles equalizer queries and settings", "[protocol][v2]")
{
    ReplyingFakeTransport fake;
    SonyProtocolSession session(&fake);
    session.connect("11:22:33:44:55:66");

    ProtocolV2 v2(session);

    SECTION("getEqualizer decodes preset and bands")
    {
        // Reply: 57 00 <preset=0x16: Bass Boost> 06 <clearBass=13 (+3)> <10, 11, 12, 13, 14 (0, +1, +2, +3, +4)>
        fake.queueIncoming(FrameCodec::encode(SonyFrame{ .type = DataType::Ack, .sequence = 0 }));
        fake.queueIncoming(FrameCodec::encode(SonyFrame{ .type = DataType::DataMdr, .sequence = 1, .payload = {0x57, 0x00, 0x16, 0x06, 13, 10, 11, 12, 13, 14} }));

        auto eq = v2.getEqualizer();
        REQUIRE(eq.preset == 0x16);
        REQUIRE(eq.clearBass == 3);
        REQUIRE(eq.bands == std::vector<int>{0, 1, 2, 3, 4});
    }

    SECTION("setEqualizerPreset sends command")
    {
        fake.queueReply({ SonyFrame{ .type = DataType::Ack, .sequence = 0 } });
        v2.setEqualizerPreset(0x16);

        REQUIRE(fake.sentCount() == 1);
        auto sent = FrameCodec::decode(fake.lastSentFrame());
        REQUIRE(sent.payload == std::vector<uint8_t>{0x58, 0x00, 0x16, 0x00});
    }

    SECTION("setEqualizerCustom sends clamped bands")
    {
        fake.queueReply({ SonyFrame{ .type = DataType::Ack, .sequence = 0 } });
        v2.setEqualizerCustom(5, { -2, 0, 3, 7, 10 });

        REQUIRE(fake.sentCount() == 1);
        auto sent = FrameCodec::decode(fake.lastSentFrame());
        REQUIRE(sent.payload == std::vector<uint8_t>{0x58, 0x00, 0xa0, 0x06, 15, 8, 10, 13, 17, 20});
    }
}

// WH-1000XM6 speaks a different, 10-band equalizer layout under inquired
// type 0x04 instead of the legacy 5-band + Clear Bass layout under 0x00 --
// reverse-engineered from a real Sound Connect btsnoop capture (issue #10).
// Frame bytes below are taken directly from that capture.
TEST_CASE("ProtocolV2: handles the 10-band equalizer layout (WH-1000XM6)", "[protocol][v2]")
{
    ReplyingFakeTransport fake;
    SonyProtocolSession session(&fake);
    session.connect("11:22:33:44:55:66");

    ProtocolV2 v2(session, /*tenBandEqualizer=*/true);

    SECTION("getEqualizer queries inquired type 0x04 and decodes 10 raw bands")
    {
        // Capture offset 22375: 57 04 a0 0a 07 07 07 07 06 06 07 08 08 08
        fake.queueReply({
            SonyFrame{ .type = DataType::Ack, .sequence = 0 },
            SonyFrame{ .type = DataType::DataMdr, .sequence = 1,
                .payload = {0x57, 0x04, 0xa0, 0x0a, 0x07, 0x07, 0x07, 0x07, 0x06, 0x06, 0x07, 0x08, 0x08, 0x08} }
        });

        auto eq = v2.getEqualizer();
        REQUIRE(fake.sentCount() >= 1);
        REQUIRE(FrameCodec::decode(fake.sentFrames().front()).payload == std::vector<uint8_t>{0x56, 0x04});
        REQUIRE(eq.preset == 0xa0);
        REQUIRE(eq.clearBass == 0);
        REQUIRE(eq.bands == std::vector<int>{7, 7, 7, 7, 6, 6, 7, 8, 8, 8});
    }

    SECTION("setEqualizerPreset uses inquired type 0x04")
    {
        fake.queueReply({ SonyFrame{ .type = DataType::Ack, .sequence = 0 } });
        v2.setEqualizerPreset(0x30);

        REQUIRE(fake.sentCount() == 1);
        auto sent = FrameCodec::decode(fake.lastSentFrame());
        REQUIRE(sent.payload == std::vector<uint8_t>{0x58, 0x04, 0x30, 0x00});
    }

    SECTION("setEqualizerCustom sends raw, unbiased band values with no Clear Bass slot")
    {
        fake.queueReply({ SonyFrame{ .type = DataType::Ack, .sequence = 0 } });
        // clearBass is meaningless on this layout and must be ignored.
        v2.setEqualizerCustom(5, {7, 7, 7, 7, 6, 6, 3, 7, 3, 8});

        REQUIRE(fake.sentCount() == 1);
        auto sent = FrameCodec::decode(fake.lastSentFrame());
        REQUIRE(sent.payload == std::vector<uint8_t>{0x58, 0x04, 0xa0, 0x0a, 7, 7, 7, 7, 6, 6, 3, 7, 3, 8});
    }

    SECTION("setEqualizerCustom clamps to the observed 0..12 raw range")
    {
        fake.queueReply({ SonyFrame{ .type = DataType::Ack, .sequence = 0 } });
        v2.setEqualizerCustom(0, {-5, 0, 6, 12, 99, -1, 1, 5, 11, 13});

        REQUIRE(fake.sentCount() == 1);
        auto sent = FrameCodec::decode(fake.lastSentFrame());
        REQUIRE(sent.payload == std::vector<uint8_t>{0x58, 0x04, 0xa0, 0x0a, 0, 0, 6, 12, 12, 0, 1, 5, 11, 12});
    }
}

TEST_CASE("ProtocolV2: handles DSEE query and control", "[protocol][v2]")
{
    ReplyingFakeTransport fake;
    SonyProtocolSession session(&fake);
    session.connect("11:22:33:44:55:66");

    ProtocolV2 v2(session);

    fake.queueIncoming(FrameCodec::encode(SonyFrame{ .type = DataType::Ack, .sequence = 0 }));
    fake.queueIncoming(FrameCodec::encode(SonyFrame{ .type = DataType::DataMdr, .sequence = 1, .payload = {0xe7, 0x01, 0x01} }));

    bool dsee = v2.getDsee();
    REQUIRE(dsee == true);

    fake.queueReply({ SonyFrame{ .type = DataType::Ack, .sequence = 1 } });
    v2.setDsee(false);

    auto sent = FrameCodec::decode(fake.lastSentFrame());
    REQUIRE(sent.payload == std::vector<uint8_t>{0xe8, 0x01, 0x00});
}

TEST_CASE("ProtocolV2: handles peripheral feature inquiries", "[protocol][v2]")
{
    FakeTransport fake;
    SonyProtocolSession session(&fake);
    session.connect("11:22:33:44:55:66");

    ProtocolV2 v2(session);

    SECTION("firmware version")
    {
        fake.queueIncoming(FrameCodec::encode(SonyFrame{ .type = DataType::Ack, .sequence = 0 }));
        fake.queueIncoming(FrameCodec::encode(SonyFrame{ .type = DataType::DataMdr, .sequence = 1, .payload = {0x05, 0x02, 0x00, '2', '.', '0', '.', '1'} }));

        auto fw = v2.getFirmwareVersion();
        REQUIRE(fw == "2.0.1");
    }

    SECTION("codec inquiry")
    {
        fake.queueIncoming(FrameCodec::encode(SonyFrame{ .type = DataType::Ack, .sequence = 0 }));
        fake.queueIncoming(FrameCodec::encode(SonyFrame{ .type = DataType::DataMdr, .sequence = 1, .payload = {0x13, 0x02, 0x10} })); // 0x10 = LDAC

        auto codec = v2.getCodec();
        REQUIRE(codec == "LDAC");
    }
}

TEST_CASE("ProtocolV2: reads noise control via inquired type 0x19 (WH-1000XM6)", "[protocol][v2]")
{
    ReplyingFakeTransport fake;
    SonyProtocolSession session(&fake);
    session.connect("11:22:33:44:55:66");

    ProtocolV2 v2(session, true, true);

    // Captured from a WH-1000XM6 (firmware 3.1.5) in Ambient, level 10.
    fake.queueReply({ SonyFrame{ .type = DataType::Ack, .sequence = 0 },
                      SonyFrame{ .type = DataType::DataMdr, .sequence = 1,
                                 .payload = {0x67, 0x19, 0x01, 0x01, 0x01, 0x00, 0x0a, 0x00, 0x00} } });

    auto nc = v2.getNoiseControl();
    REQUIRE(nc.mode == NoiseControlMode::Ambient);
    REQUIRE(nc.ambientLevel == 10);
    REQUIRE_FALSE(nc.focusOnVoice);

    auto sent = FrameCodec::decode(fake.sentFrames().front());
    REQUIRE(sent.payload == std::vector<uint8_t>{0x66, 0x19});
}
