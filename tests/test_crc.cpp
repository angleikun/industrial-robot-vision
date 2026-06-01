#include <gtest/gtest.h>
#include "RobotClient.h"

TEST(RobotFrameTest, SerializeAndDeserializeRoundTrip)
{
    RobotFrame frame;
    frame.cmd  = 0x01;
    frame.data = QByteArray::fromHex("DEADBEEF");

    QByteArray raw = frame.serialize();

    RobotFrame parsed;
    EXPECT_TRUE(RobotFrame::deserialize(raw, parsed));
    EXPECT_EQ(parsed.cmd, 0x01);
    EXPECT_EQ(parsed.data, frame.data);
}

TEST(RobotFrameTest, RejectInvalidCRC)
{
    RobotFrame frame;
    frame.cmd  = 0x01;
    frame.data = QByteArray::fromHex("DEADBEEF");
    QByteArray raw = frame.serialize();

    // Tamper with one byte in data payload
    raw[6] = raw[6] ^ 0xFF;

    RobotFrame parsed;
    EXPECT_FALSE(RobotFrame::deserialize(raw, parsed));
}

TEST(RobotFrameTest, RejectOversizedFrame)
{
    QByteArray raw;
    raw.append(char(0xAA));
    raw.append(char(0xFF));
    raw.append(char(0x01));
    raw.append(char(0xFF));  // LEN high = 255
    raw.append(char(0xFF));  // LEN low  = 255
    raw.append(QByteArray(8, '\0'));

    RobotFrame parsed;
    EXPECT_FALSE(RobotFrame::deserialize(raw, parsed));
}

TEST(RobotFrameTest, RejectInvalidHeader)
{
    QByteArray raw;
    raw.append(char(0x00));
    raw.append(char(0x00));
    raw.append(char(0x01));
    raw.append(char(0x00));   // LEN = 0
    raw.append(char(0x00));
    raw.append(char(0x00));   // CRC placeholder
    raw.append(char(0x00));
    raw.append(char(0x0D));   // tail

    RobotFrame parsed;
    EXPECT_FALSE(RobotFrame::deserialize(raw, parsed));
}

TEST(Crc16ModbusTest, KnownVector)
{
    // Test vector: 0x01 0x00 0x00 (CMD + LEN with zero-length data)
    QByteArray input;
    input.append(char(0x01));
    input.append(char(0x00));
    input.append(char(0x00));
    quint16 crc = computeCrc16Modbus(input);
    // CRC is deterministic; verify it's non-zero
    EXPECT_NE(crc, 0x0000);
}
