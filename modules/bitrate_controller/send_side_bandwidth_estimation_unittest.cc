/*
 *  Copyright (c) 2014 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/bitrate_controller/send_side_bandwidth_estimation.h"

#include "api/rtc_event_log/rtc_event.h"
#include "logging/rtc_event_log/events/rtc_event_bwe_update_loss_based.h"
#include "logging/rtc_event_log/mock/mock_rtc_event_log.h"
#include "test/gmock.h"
#include "test/gtest.h"

namespace webrtc {

MATCHER(LossBasedBweUpdateWithBitrateOnly, "") {
  if (arg->GetType() != RtcEvent::Type::BweUpdateLossBased) {
    return false;
  }
  auto bwe_event = static_cast<RtcEventBweUpdateLossBased*>(arg);
  return bwe_event->bitrate_bps() > 0 && bwe_event->fraction_loss() == 0;
}

MATCHER(LossBasedBweUpdateWithBitrateAndLossFraction, "") {
  if (arg->GetType() != RtcEvent::Type::BweUpdateLossBased) {
    return false;
  }
  auto bwe_event = static_cast<RtcEventBweUpdateLossBased*>(arg);
  return bwe_event->bitrate_bps() > 0 && bwe_event->fraction_loss() > 0;
}

void TestProbing(bool use_delay_based) {
  ::testing::NiceMock<MockRtcEventLog> event_log;
  SendSideBandwidthEstimation bwe(&event_log);
  int64_t now_ms = 0;
  bwe.SetMinMaxBitrate(DataRate::bps(100000), DataRate::bps(1500000));
  bwe.SetSendBitrate(DataRate::bps(200000), Timestamp::ms(now_ms));

  const int kRembBps = 1000000;
  const int kSecondRembBps = kRembBps + 500000;

  bwe.UpdateReceiverBlock(0, TimeDelta::ms(50), 1, Timestamp::ms(now_ms));

  // Initial REMB applies immediately.
  if (use_delay_based) {
    bwe.UpdateDelayBasedEstimate(Timestamp::ms(now_ms),
                                 DataRate::bps(kRembBps));
  } else {
    bwe.UpdateReceiverEstimate(Timestamp::ms(now_ms), DataRate::bps(kRembBps));
  }
  bwe.UpdateEstimate(Timestamp::ms(now_ms));
  int bitrate;
  uint8_t fraction_loss;
  int64_t rtt;
  bwe.CurrentEstimate(&bitrate, &fraction_loss, &rtt);
  EXPECT_EQ(kRembBps, bitrate);

  // Second REMB doesn't apply immediately.
  now_ms += 2001;
  if (use_delay_based) {
    bwe.UpdateDelayBasedEstimate(Timestamp::ms(now_ms),
                                 DataRate::bps(kSecondRembBps));
  } else {
    bwe.UpdateReceiverEstimate(Timestamp::ms(now_ms),
                               DataRate::bps(kSecondRembBps));
  }
  bwe.UpdateEstimate(Timestamp::ms(now_ms));
  bitrate = 0;
  bwe.CurrentEstimate(&bitrate, &fraction_loss, &rtt);
  EXPECT_EQ(kRembBps, bitrate);
}

TEST(SendSideBweTest, InitialRembWithProbing) {
  TestProbing(false);
}

TEST(SendSideBweTest, InitialDelayBasedBweWithProbing) {
  TestProbing(true);
}

TEST(SendSideBweTest, DoesntReapplyBitrateDecreaseWithoutFollowingRemb) {
  MockRtcEventLog event_log;
  EXPECT_CALL(event_log, LogProxy(LossBasedBweUpdateWithBitrateOnly()))
      .Times(1);
  EXPECT_CALL(event_log,
              LogProxy(LossBasedBweUpdateWithBitrateAndLossFraction()))
      .Times(1);
  SendSideBandwidthEstimation bwe(&event_log);
  static const int kMinBitrateBps = 100000;
  static const int kInitialBitrateBps = 1000000;
  int64_t now_ms = 1000;
  bwe.SetMinMaxBitrate(DataRate::bps(kMinBitrateBps), DataRate::bps(1500000));
  bwe.SetSendBitrate(DataRate::bps(kInitialBitrateBps), Timestamp::ms(now_ms));

  static const uint8_t kFractionLoss = 128;
  static const int64_t kRttMs = 50;
  now_ms += 10000;

  int bitrate_bps;
  uint8_t fraction_loss;
  int64_t rtt_ms;
  bwe.CurrentEstimate(&bitrate_bps, &fraction_loss, &rtt_ms);
  EXPECT_EQ(kInitialBitrateBps, bitrate_bps);
  EXPECT_EQ(0, fraction_loss);
  EXPECT_EQ(0, rtt_ms);

  // Signal heavy loss to go down in bitrate.
  bwe.UpdateReceiverBlock(kFractionLoss, TimeDelta::ms(kRttMs), 100,
                          Timestamp::ms(now_ms));
  // Trigger an update 2 seconds later to not be rate limited.
  now_ms += 1000;
  bwe.UpdateEstimate(Timestamp::ms(now_ms));

  bwe.CurrentEstimate(&bitrate_bps, &fraction_loss, &rtt_ms);
  EXPECT_LT(bitrate_bps, kInitialBitrateBps);
  // Verify that the obtained bitrate isn't hitting the min bitrate, or this
  // test doesn't make sense. If this ever happens, update the thresholds or
  // loss rates so that it doesn't hit min bitrate after one bitrate update.
  EXPECT_GT(bitrate_bps, kMinBitrateBps);
  EXPECT_EQ(kFractionLoss, fraction_loss);
  EXPECT_EQ(kRttMs, rtt_ms);

  // Triggering an update shouldn't apply further downgrade nor upgrade since
  // there's no intermediate receiver block received indicating whether this is
  // currently good or not.
  int last_bitrate_bps = bitrate_bps;
  // Trigger an update 2 seconds later to not be rate limited (but it still
  // shouldn't update).
  now_ms += 1000;
  bwe.UpdateEstimate(Timestamp::ms(now_ms));
  bwe.CurrentEstimate(&bitrate_bps, &fraction_loss, &rtt_ms);

  EXPECT_EQ(last_bitrate_bps, bitrate_bps);
  // The old loss rate should still be applied though.
  EXPECT_EQ(kFractionLoss, fraction_loss);
  EXPECT_EQ(kRttMs, rtt_ms);
}

TEST(SendSideBweTest, SettingSendBitrateOverridesDelayBasedEstimate) {
  ::testing::NiceMock<MockRtcEventLog> event_log;
  SendSideBandwidthEstimation bwe(&event_log);
  static const int kMinBitrateBps = 10000;
  static const int kMaxBitrateBps = 10000000;
  static const int kInitialBitrateBps = 300000;
  static const int kDelayBasedBitrateBps = 350000;
  static const int kForcedHighBitrate = 2500000;

  int64_t now_ms = 0;
  int bitrate_bps;
  uint8_t fraction_loss;
  int64_t rtt_ms;

  bwe.SetMinMaxBitrate(DataRate::bps(kMinBitrateBps),
                       DataRate::bps(kMaxBitrateBps));
  bwe.SetSendBitrate(DataRate::bps(kInitialBitrateBps), Timestamp::ms(now_ms));

  bwe.UpdateDelayBasedEstimate(Timestamp::ms(now_ms),
                               DataRate::bps(kDelayBasedBitrateBps));
  bwe.UpdateEstimate(Timestamp::ms(now_ms));
  bwe.CurrentEstimate(&bitrate_bps, &fraction_loss, &rtt_ms);
  EXPECT_GE(bitrate_bps, kInitialBitrateBps);
  EXPECT_LE(bitrate_bps, kDelayBasedBitrateBps);

  bwe.SetSendBitrate(DataRate::bps(kForcedHighBitrate), Timestamp::ms(now_ms));
  bwe.CurrentEstimate(&bitrate_bps, &fraction_loss, &rtt_ms);
  EXPECT_EQ(bitrate_bps, kForcedHighBitrate);
}

}  // namespace webrtc
