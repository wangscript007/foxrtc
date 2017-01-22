/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

// Unit tests for RedPayloadSplitter class.

#include "webrtc/modules/audio_coding/neteq/red_payload_splitter.h"

#include <assert.h>

#include <memory>
#include <utility>  // pair

#include "webrtc/modules/audio_coding/codecs/builtin_audio_decoder_factory.h"
#include "webrtc/modules/audio_coding/codecs/mock/mock_audio_decoder_factory.h"
#include "webrtc/modules/audio_coding/neteq/mock/mock_decoder_database.h"
#include "webrtc/modules/audio_coding/neteq/packet.h"
#include "webrtc/test/gtest.h"

using ::testing::Return;
using ::testing::ReturnNull;

namespace webrtc {

static const int kRedPayloadType = 100;
static const size_t kPayloadLength = 10;
static const size_t kRedHeaderLength = 4;  // 4 bytes RED header.
static const uint16_t kSequenceNumber = 0;
static const uint32_t kBaseTimestamp = 0x12345678;

// A possible Opus packet that contains FEC is the following.
// The frame is 20 ms in duration.
//
// 0                   1                   2                   3
// 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |0|0|0|0|1|0|0|0|x|1|x|x|x|x|x|x|x|                             |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                             |
// |                    Compressed frame 1 (N-2 bytes)...          :
// :                                                               |
// |                                                               |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
void CreateOpusFecPayload(uint8_t* payload,
                          size_t payload_length,
                          uint8_t payload_value) {
  if (payload_length < 2) {
    return;
  }
  payload[0] = 0x08;
  payload[1] = 0x40;
  memset(&payload[2], payload_value, payload_length - 2);
}

// RED headers (according to RFC 2198):
//
//    0                   1                   2                   3
//    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |F|   block PT  |  timestamp offset         |   block length    |
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Last RED header:
//    0 1 2 3 4 5 6 7
//   +-+-+-+-+-+-+-+-+
//   |0|   Block PT  |
//   +-+-+-+-+-+-+-+-+

// Creates a RED packet, with |num_payloads| payloads, with payload types given
// by the values in array |payload_types| (which must be of length
// |num_payloads|). Each redundant payload is |timestamp_offset| samples
// "behind" the the previous payload.
Packet* CreateRedPayload(size_t num_payloads,
                         uint8_t* payload_types,
                         int timestamp_offset,
                         bool embed_opus_fec = false) {
  Packet* packet = new Packet;
  packet->header.payloadType = kRedPayloadType;
  packet->header.timestamp = kBaseTimestamp;
  packet->header.sequenceNumber = kSequenceNumber;
  packet->payload.SetSize((kPayloadLength + 1) +
                          (num_payloads - 1) *
                              (kPayloadLength + kRedHeaderLength));
  uint8_t* payload_ptr = packet->payload.data();
  for (size_t i = 0; i < num_payloads; ++i) {
    // Write the RED headers.
    if (i == num_payloads - 1) {
      // Special case for last payload.
      *payload_ptr = payload_types[i] & 0x7F;  // F = 0;
      ++payload_ptr;
      break;
    }
    *payload_ptr = payload_types[i] & 0x7F;
    // Not the last block; set F = 1.
    *payload_ptr |= 0x80;
    ++payload_ptr;
    int this_offset = (num_payloads - i - 1) * timestamp_offset;
    *payload_ptr = this_offset >> 6;
    ++payload_ptr;
    assert(kPayloadLength <= 1023);  // Max length described by 10 bits.
    *payload_ptr = ((this_offset & 0x3F) << 2) | (kPayloadLength >> 8);
    ++payload_ptr;
    *payload_ptr = kPayloadLength & 0xFF;
    ++payload_ptr;
  }
  for (size_t i = 0; i < num_payloads; ++i) {
    // Write |i| to all bytes in each payload.
    if (embed_opus_fec) {
      CreateOpusFecPayload(payload_ptr, kPayloadLength,
                           static_cast<uint8_t>(i));
    } else {
      memset(payload_ptr, static_cast<int>(i), kPayloadLength);
    }
    payload_ptr += kPayloadLength;
  }
  return packet;
}

// Create a packet with all payload bytes set to |payload_value|.
Packet* CreatePacket(uint8_t payload_type,
                     size_t payload_length,
                     uint8_t payload_value,
                     bool opus_fec = false) {
  Packet* packet = new Packet;
  packet->header.payloadType = payload_type;
  packet->header.timestamp = kBaseTimestamp;
  packet->header.sequenceNumber = kSequenceNumber;
  packet->payload.SetSize(payload_length);
  if (opus_fec) {
    CreateOpusFecPayload(packet->payload.data(), packet->payload.size(),
                         payload_value);
  } else {
    memset(packet->payload.data(), payload_value, packet->payload.size());
  }
  return packet;
}

// Checks that |packet| has the attributes given in the remaining parameters.
void VerifyPacket(const Packet* packet,
                  size_t payload_length,
                  uint8_t payload_type,
                  uint16_t sequence_number,
                  uint32_t timestamp,
                  uint8_t payload_value,
                  Packet::Priority priority) {
  EXPECT_EQ(payload_length, packet->payload.size());
  EXPECT_EQ(payload_type, packet->header.payloadType);
  EXPECT_EQ(sequence_number, packet->header.sequenceNumber);
  EXPECT_EQ(timestamp, packet->header.timestamp);
  EXPECT_EQ(priority, packet->priority);
  ASSERT_FALSE(packet->payload.empty());
  for (size_t i = 0; i < packet->payload.size(); ++i) {
    ASSERT_EQ(payload_value, packet->payload.data()[i]);
  }
}

void VerifyPacket(const Packet* packet,
                  size_t payload_length,
                  uint8_t payload_type,
                  uint16_t sequence_number,
                  uint32_t timestamp,
                  uint8_t payload_value,
                  bool primary) {
  return VerifyPacket(packet, payload_length, payload_type, sequence_number,
                      timestamp, payload_value,
                      Packet::Priority{0, primary ? 0 : 1});
}

// Start of test definitions.

TEST(RedPayloadSplitter, CreateAndDestroy) {
  RedPayloadSplitter* splitter = new RedPayloadSplitter;
  delete splitter;
}

// Packet A is split into A1 and A2.
TEST(RedPayloadSplitter, OnePacketTwoPayloads) {
  uint8_t payload_types[] = {0, 0};
  const int kTimestampOffset = 160;
  Packet* packet = CreateRedPayload(2, payload_types, kTimestampOffset);
  PacketList packet_list;
  packet_list.push_back(packet);
  RedPayloadSplitter splitter;
  EXPECT_TRUE(splitter.SplitRed(&packet_list));
  ASSERT_EQ(2u, packet_list.size());
  // Check first packet. The first in list should always be the primary payload.
  packet = packet_list.front();
  VerifyPacket(packet, kPayloadLength, payload_types[1], kSequenceNumber,
               kBaseTimestamp, 1, true);
  delete packet;
  packet_list.pop_front();
  // Check second packet.
  packet = packet_list.front();
  VerifyPacket(packet, kPayloadLength, payload_types[0], kSequenceNumber,
               kBaseTimestamp - kTimestampOffset, 0, false);
  delete packet;
}

// Packets A and B are not split at all. Only the RED header in each packet is
// removed.
TEST(RedPayloadSplitter, TwoPacketsOnePayload) {
  uint8_t payload_types[] = {0};
  const int kTimestampOffset = 160;
  // Create first packet, with a single RED payload.
  Packet* packet = CreateRedPayload(1, payload_types, kTimestampOffset);
  PacketList packet_list;
  packet_list.push_back(packet);
  // Create second packet, with a single RED payload.
  packet = CreateRedPayload(1, payload_types, kTimestampOffset);
  // Manually change timestamp and sequence number of second packet.
  packet->header.timestamp += kTimestampOffset;
  packet->header.sequenceNumber++;
  packet_list.push_back(packet);
  RedPayloadSplitter splitter;
  EXPECT_TRUE(splitter.SplitRed(&packet_list));
  ASSERT_EQ(2u, packet_list.size());
  // Check first packet.
  packet = packet_list.front();
  VerifyPacket(packet, kPayloadLength, payload_types[0], kSequenceNumber,
               kBaseTimestamp, 0, true);
  delete packet;
  packet_list.pop_front();
  // Check second packet.
  packet = packet_list.front();
  VerifyPacket(packet, kPayloadLength, payload_types[0], kSequenceNumber + 1,
               kBaseTimestamp + kTimestampOffset, 0, true);
  delete packet;
}

// Packets A and B are split into packets A1, A2, A3, B1, B2, B3, with
// attributes as follows:
//
//                  A1*   A2    A3    B1*   B2    B3
// Payload type     0     1     2     0     1     2
// Timestamp        b     b-o   b-2o  b+o   b     b-o
// Sequence number  0     0     0     1     1     1
//
// b = kBaseTimestamp, o = kTimestampOffset, * = primary.
TEST(RedPayloadSplitter, TwoPacketsThreePayloads) {
  uint8_t payload_types[] = {2, 1, 0};  // Primary is the last one.
  const int kTimestampOffset = 160;
  // Create first packet, with 3 RED payloads.
  Packet* packet = CreateRedPayload(3, payload_types, kTimestampOffset);
  PacketList packet_list;
  packet_list.push_back(packet);
  // Create first packet, with 3 RED payloads.
  packet = CreateRedPayload(3, payload_types, kTimestampOffset);
  // Manually change timestamp and sequence number of second packet.
  packet->header.timestamp += kTimestampOffset;
  packet->header.sequenceNumber++;
  packet_list.push_back(packet);
  RedPayloadSplitter splitter;
  EXPECT_TRUE(splitter.SplitRed(&packet_list));
  ASSERT_EQ(6u, packet_list.size());
  // Check first packet, A1.
  packet = packet_list.front();
  VerifyPacket(packet, kPayloadLength, payload_types[2], kSequenceNumber,
               kBaseTimestamp, 2, {0, 0});
  delete packet;
  packet_list.pop_front();
  // Check second packet, A2.
  packet = packet_list.front();
  VerifyPacket(packet, kPayloadLength, payload_types[1], kSequenceNumber,
               kBaseTimestamp - kTimestampOffset, 1, {0, 1});
  delete packet;
  packet_list.pop_front();
  // Check third packet, A3.
  packet = packet_list.front();
  VerifyPacket(packet, kPayloadLength, payload_types[0], kSequenceNumber,
               kBaseTimestamp - 2 * kTimestampOffset, 0, {0, 2});
  delete packet;
  packet_list.pop_front();
  // Check fourth packet, B1.
  packet = packet_list.front();
  VerifyPacket(packet, kPayloadLength, payload_types[2], kSequenceNumber + 1,
               kBaseTimestamp + kTimestampOffset, 2, {0, 0});
  delete packet;
  packet_list.pop_front();
  // Check fifth packet, B2.
  packet = packet_list.front();
  VerifyPacket(packet, kPayloadLength, payload_types[1], kSequenceNumber + 1,
               kBaseTimestamp, 1, {0, 1});
  delete packet;
  packet_list.pop_front();
  // Check sixth packet, B3.
  packet = packet_list.front();
  VerifyPacket(packet, kPayloadLength, payload_types[0], kSequenceNumber + 1,
               kBaseTimestamp - kTimestampOffset, 0, {0, 2});
  delete packet;
}

// Creates a list with 4 packets with these payload types:
// 0 = CNGnb
// 1 = PCMu
// 2 = DTMF (AVT)
// 3 = iLBC
// We expect the method CheckRedPayloads to discard the iLBC packet, since it
// is a non-CNG, non-DTMF payload of another type than the first speech payload
// found in the list (which is PCMu).
TEST(RedPayloadSplitter, CheckRedPayloads) {
  PacketList packet_list;
  for (uint8_t i = 0; i <= 3; ++i) {
    // Create packet with payload type |i|, payload length 10 bytes, all 0.
    Packet* packet = CreatePacket(i, 10, 0);
    packet_list.push_back(packet);
  }

  // Use a real DecoderDatabase object here instead of a mock, since it is
  // easier to just register the payload types and let the actual implementation
  // do its job.
  DecoderDatabase decoder_database(
      new rtc::RefCountedObject<MockAudioDecoderFactory>);
  decoder_database.RegisterPayload(0, NetEqDecoder::kDecoderCNGnb, "cng-nb");
  decoder_database.RegisterPayload(1, NetEqDecoder::kDecoderPCMu, "pcmu");
  decoder_database.RegisterPayload(2, NetEqDecoder::kDecoderAVT, "avt");
  decoder_database.RegisterPayload(3, NetEqDecoder::kDecoderILBC, "ilbc");

  RedPayloadSplitter splitter;
  splitter.CheckRedPayloads(&packet_list, decoder_database);

  ASSERT_EQ(3u, packet_list.size());  // Should have dropped the last packet.
  // Verify packets. The loop verifies that payload types 0, 1, and 2 are in the
  // list.
  for (int i = 0; i <= 2; ++i) {
    Packet* packet = packet_list.front();
    VerifyPacket(packet, 10, i, kSequenceNumber, kBaseTimestamp, 0, true);
    delete packet;
    packet_list.pop_front();
  }
  EXPECT_TRUE(packet_list.empty());
}

// Packet A is split into A1, A2 and A3. But the length parameter is off, so
// the last payloads should be discarded.
TEST(RedPayloadSplitter, WrongPayloadLength) {
  uint8_t payload_types[] = {0, 0, 0};
  const int kTimestampOffset = 160;
  Packet* packet = CreateRedPayload(3, payload_types, kTimestampOffset);
  // Manually tamper with the payload length of the packet.
  // This is one byte too short for the second payload (out of three).
  // We expect only the first payload to be returned.
  packet->payload.SetSize(packet->payload.size() - (kPayloadLength + 1));
  PacketList packet_list;
  packet_list.push_back(packet);
  RedPayloadSplitter splitter;
  EXPECT_FALSE(splitter.SplitRed(&packet_list));
  ASSERT_EQ(1u, packet_list.size());
  // Check first packet.
  packet = packet_list.front();
  VerifyPacket(packet, kPayloadLength, payload_types[0], kSequenceNumber,
               kBaseTimestamp - 2 * kTimestampOffset, 0, {0, 2});
  delete packet;
  packet_list.pop_front();
}

}  // namespace webrtc