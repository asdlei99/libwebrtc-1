/*
 *  Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include "modules/audio_processing/aec3/echo_remover.h"

#include <math.h>
#include <stddef.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include "api/array_view.h"
#include "modules/audio_processing/aec3/aec3_common.h"
#include "modules/audio_processing/aec3/aec3_fft.h"
#include "modules/audio_processing/aec3/aec_state.h"
#include "modules/audio_processing/aec3/comfort_noise_generator.h"
#include "modules/audio_processing/aec3/echo_path_variability.h"
#include "modules/audio_processing/aec3/echo_remover_metrics.h"
#include "modules/audio_processing/aec3/fft_data.h"
#include "modules/audio_processing/aec3/render_buffer.h"
#include "modules/audio_processing/aec3/render_signal_analyzer.h"
#include "modules/audio_processing/aec3/residual_echo_estimator.h"
#include "modules/audio_processing/aec3/subtractor.h"
#include "modules/audio_processing/aec3/subtractor_output.h"
#include "modules/audio_processing/aec3/suppression_filter.h"
#include "modules/audio_processing/aec3/suppression_gain.h"
#include "modules/audio_processing/logging/apm_data_dumper.h"
#include "rtc_base/atomic_ops.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"

namespace webrtc {

namespace {

// Maximum number of channels for which the capture channel data is stored on
// the stack. If the number of channels are larger than this, they are stored
// using scratch memory that is pre-allocated on the heap. The reason for this
// partitioning is not to waste heap space for handling the more common numbers
// of channels, while at the same time not limiting the support for higher
// numbers of channels by enforcing the capture channel data to be stored on the
// stack using a fixed maximum value.
constexpr size_t kMaxNumChannelsOnStack = 2;

// Chooses the number of channels to store on the heap when that is required due
// to the number of capture channels being larger than the pre-defined number
// of channels to store on the stack.
size_t NumChannelsOnHeap(size_t num_capture_channels) {
  return num_capture_channels > kMaxNumChannelsOnStack ? num_capture_channels
                                                       : 0;
}

void LinearEchoPower(const FftData& E,
                     const FftData& Y,
                     std::array<float, kFftLengthBy2Plus1>* S2) {
  for (size_t k = 0; k < E.re.size(); ++k) {
    (*S2)[k] = (Y.re[k] - E.re[k]) * (Y.re[k] - E.re[k]) +
               (Y.im[k] - E.im[k]) * (Y.im[k] - E.im[k]);
  }
}

// Fades between two input signals using a fix-sized transition.
void SignalTransition(rtc::ArrayView<const float> from,
                      rtc::ArrayView<const float> to,
                      rtc::ArrayView<float> out) {
  constexpr size_t kTransitionSize = 30;
  constexpr float kOneByTransitionSizePlusOne = 1.f / (kTransitionSize + 1);

  RTC_DCHECK_EQ(from.size(), to.size());
  RTC_DCHECK_EQ(from.size(), out.size());
  RTC_DCHECK_LE(kTransitionSize, out.size());

  for (size_t k = 0; k < kTransitionSize; ++k) {
    float a = (k + 1) * kOneByTransitionSizePlusOne;
    out[k] = a * to[k] + (1.f - a) * from[k];
  }

  std::copy(to.begin() + kTransitionSize, to.end(),
            out.begin() + kTransitionSize);
}

// Computes a windowed (square root Hanning) padded FFT and updates the related
// memory.
void WindowedPaddedFft(const Aec3Fft& fft,
                       rtc::ArrayView<const float> v,
                       rtc::ArrayView<float> v_old,
                       FftData* V) {
  fft.PaddedFft(v, v_old, Aec3Fft::Window::kSqrtHanning, V);
  std::copy(v.begin(), v.end(), v_old.begin());
}

// Class for removing the echo from the capture signal.
class EchoRemoverImpl final : public EchoRemover {
 public:
  EchoRemoverImpl(const EchoCanceller3Config& config,
                  int sample_rate_hz,
                  size_t num_render_channels,
                  size_t num_capture_channels);
  ~EchoRemoverImpl() override;
  EchoRemoverImpl(const EchoRemoverImpl&) = delete;
  EchoRemoverImpl& operator=(const EchoRemoverImpl&) = delete;

  void GetMetrics(EchoControl::Metrics* metrics) const override;

  // Removes the echo from a block of samples from the capture signal. The
  // supplied render signal is assumed to be pre-aligned with the capture
  // signal.
  void ProcessCapture(
      EchoPathVariability echo_path_variability,
      bool capture_signal_saturation,
      const absl::optional<DelayEstimate>& external_delay,
      RenderBuffer* render_buffer,
      std::vector<std::vector<std::vector<float>>>* capture) override;

  // Updates the status on whether echo leakage is detected in the output of the
  // echo remover.
  void UpdateEchoLeakageStatus(bool leakage_detected) override {
    echo_leakage_detected_ = leakage_detected;
  }

 private:
  // Selects which of the shadow and main linear filter outputs that is most
  // appropriate to pass to the suppressor and forms the linear filter output by
  // smoothly transition between those.
  void FormLinearFilterOutput(const SubtractorOutput& subtractor_output,
                              rtc::ArrayView<float> output);

  static int instance_count_;
  const EchoCanceller3Config config_;
  const Aec3Fft fft_;
  std::unique_ptr<ApmDataDumper> data_dumper_;
  const Aec3Optimization optimization_;
  const int sample_rate_hz_;
  const size_t num_render_channels_;
  const size_t num_capture_channels_;
  const bool use_shadow_filter_output_;
  Subtractor subtractor_;
  SuppressionGain suppression_gain_;
  ComfortNoiseGenerator cng_;
  SuppressionFilter suppression_filter_;
  RenderSignalAnalyzer render_signal_analyzer_;
  ResidualEchoEstimator residual_echo_estimator_;
  bool echo_leakage_detected_ = false;
  AecState aec_state_;
  EchoRemoverMetrics metrics_;
  std::array<float, kFftLengthBy2> e_old_;
  std::array<float, kFftLengthBy2> x_old_;
  std::array<float, kFftLengthBy2> y_old_;
  size_t block_counter_ = 0;
  int gain_change_hangover_ = 0;
  bool main_filter_output_last_selected_ = true;
  bool linear_filter_output_last_selected_ = true;

  std::vector<std::array<float, kFftLengthBy2Plus1>> Y2_heap_;
  std::vector<std::array<float, kFftLengthBy2Plus1>> E2_heap_;
  std::vector<std::array<float, kFftLengthBy2Plus1>> R2_heap_;
  std::vector<std::array<float, kFftLengthBy2Plus1>> S2_linear_heap_;
  std::vector<FftData> Y_heap_;
  std::vector<FftData> E_heap_;
  std::vector<FftData> comfort_noise_heap_;
  std::vector<FftData> high_band_comfort_noise_heap_;
  std::vector<SubtractorOutput> subtractor_output_heap_;
};

int EchoRemoverImpl::instance_count_ = 0;

EchoRemoverImpl::EchoRemoverImpl(const EchoCanceller3Config& config,
                                 int sample_rate_hz,
                                 size_t num_render_channels,
                                 size_t num_capture_channels)
    : config_(config),
      fft_(),
      data_dumper_(
          new ApmDataDumper(rtc::AtomicOps::Increment(&instance_count_))),
      optimization_(DetectOptimization()),
      sample_rate_hz_(sample_rate_hz),
      num_render_channels_(num_render_channels),
      num_capture_channels_(num_capture_channels),
      use_shadow_filter_output_(
          config_.filter.enable_shadow_filter_output_usage),
      subtractor_(config,
                  num_render_channels_,
                  num_capture_channels_,
                  data_dumper_.get(),
                  optimization_),
      suppression_gain_(config_, optimization_, sample_rate_hz),
      cng_(optimization_),
      suppression_filter_(optimization_, sample_rate_hz_),
      render_signal_analyzer_(config_),
      residual_echo_estimator_(config_),
      aec_state_(config_),
      Y2_heap_(NumChannelsOnHeap(num_capture_channels_)),
      E2_heap_(NumChannelsOnHeap(num_capture_channels_)),
      R2_heap_(NumChannelsOnHeap(num_capture_channels_)),
      S2_linear_heap_(NumChannelsOnHeap(num_capture_channels_)),
      Y_heap_(NumChannelsOnHeap(num_capture_channels_)),
      E_heap_(NumChannelsOnHeap(num_capture_channels_)),
      comfort_noise_heap_(NumChannelsOnHeap(num_capture_channels_)),
      high_band_comfort_noise_heap_(NumChannelsOnHeap(num_capture_channels_)),
      subtractor_output_heap_(NumChannelsOnHeap(num_capture_channels_)) {
  RTC_DCHECK(ValidFullBandRate(sample_rate_hz));
  x_old_.fill(0.f);
  y_old_.fill(0.f);
  e_old_.fill(0.f);
}

EchoRemoverImpl::~EchoRemoverImpl() = default;

void EchoRemoverImpl::GetMetrics(EchoControl::Metrics* metrics) const {
  // Echo return loss (ERL) is inverted to go from gain to attenuation.
  metrics->echo_return_loss = -10.0 * std::log10(aec_state_.ErlTimeDomain());
  metrics->echo_return_loss_enhancement =
      Log2TodB(aec_state_.FullBandErleLog2());
}

void EchoRemoverImpl::ProcessCapture(
    EchoPathVariability echo_path_variability,
    bool capture_signal_saturation,
    const absl::optional<DelayEstimate>& external_delay,
    RenderBuffer* render_buffer,
    std::vector<std::vector<std::vector<float>>>* capture) {
  ++block_counter_;
  const std::vector<std::vector<std::vector<float>>>& x =
      render_buffer->Block(0);
  std::vector<std::vector<std::vector<float>>>* y = capture;
  RTC_DCHECK(render_buffer);
  RTC_DCHECK(y);
  RTC_DCHECK_EQ(x.size(), NumBandsForRate(sample_rate_hz_));
  RTC_DCHECK_EQ(y->size(), NumBandsForRate(sample_rate_hz_));
  RTC_DCHECK_EQ(x[0].size(), num_render_channels_);
  RTC_DCHECK_EQ((*y)[0].size(), num_capture_channels_);
  RTC_DCHECK_EQ(x[0][0].size(), kBlockSize);
  RTC_DCHECK_EQ((*y)[0][0].size(), kBlockSize);

  // Stack allocated data to use when the number of channels is low.
  std::array<std::array<float, kFftLengthBy2Plus1>, kMaxNumChannelsOnStack>
      Y2_stack;
  std::array<std::array<float, kFftLengthBy2Plus1>, kMaxNumChannelsOnStack>
      E2_stack;
  std::array<std::array<float, kFftLengthBy2Plus1>, kMaxNumChannelsOnStack>
      R2_stack;
  std::array<std::array<float, kFftLengthBy2Plus1>, kMaxNumChannelsOnStack>
      S2_linear_stack;
  std::array<FftData, kMaxNumChannelsOnStack> Y_stack;
  std::array<FftData, kMaxNumChannelsOnStack> E_stack;
  std::array<FftData, kMaxNumChannelsOnStack> comfort_noise_stack;
  std::array<FftData, kMaxNumChannelsOnStack> high_band_comfort_noise_stack;
  std::array<SubtractorOutput, kMaxNumChannelsOnStack> subtractor_output_stack;

  rtc::ArrayView<std::array<float, kFftLengthBy2Plus1>> Y2(
      Y2_stack.data(), num_capture_channels_);
  rtc::ArrayView<std::array<float, kFftLengthBy2Plus1>> E2(
      E2_stack.data(), num_capture_channels_);
  rtc::ArrayView<std::array<float, kFftLengthBy2Plus1>> R2(
      R2_stack.data(), num_capture_channels_);
  rtc::ArrayView<std::array<float, kFftLengthBy2Plus1>> S2_linear(
      S2_linear_stack.data(), num_capture_channels_);
  rtc::ArrayView<FftData> Y(Y_stack.data(), num_capture_channels_);
  rtc::ArrayView<FftData> E(E_stack.data(), num_capture_channels_);
  rtc::ArrayView<FftData> comfort_noise(comfort_noise_stack.data(),
                                        num_capture_channels_);
  rtc::ArrayView<FftData> high_band_comfort_noise(
      high_band_comfort_noise_stack.data(), num_capture_channels_);
  rtc::ArrayView<SubtractorOutput> subtractor_output(
      subtractor_output_stack.data(), num_capture_channels_);
  if (NumChannelsOnHeap(num_capture_channels_) > 0) {
    // If the stack-allocated space is too small, use the heap for storing the
    // microphone data.
    Y2 = rtc::ArrayView<std::array<float, kFftLengthBy2Plus1>>(
        Y2_heap_.data(), num_capture_channels_);
    E2 = rtc::ArrayView<std::array<float, kFftLengthBy2Plus1>>(
        E2_heap_.data(), num_capture_channels_);
    R2 = rtc::ArrayView<std::array<float, kFftLengthBy2Plus1>>(
        R2_heap_.data(), num_capture_channels_);
    S2_linear = rtc::ArrayView<std::array<float, kFftLengthBy2Plus1>>(
        S2_linear_heap_.data(), num_capture_channels_);
    Y = rtc::ArrayView<FftData>(Y_heap_.data(), num_capture_channels_);
    E = rtc::ArrayView<FftData>(E_heap_.data(), num_capture_channels_);
    comfort_noise = rtc::ArrayView<FftData>(comfort_noise_heap_.data(),
                                            num_capture_channels_);
    high_band_comfort_noise = rtc::ArrayView<FftData>(
        high_band_comfort_noise_heap_.data(), num_capture_channels_);
    subtractor_output = rtc::ArrayView<SubtractorOutput>(
        subtractor_output_heap_.data(), num_capture_channels_);
  }

  const std::vector<float>& x0 = x[0][0];
  std::vector<float>& y0 = (*y)[0][0];

  data_dumper_->DumpWav("aec3_echo_remover_capture_input", kBlockSize, &y0[0],
                        16000, 1);
  data_dumper_->DumpWav("aec3_echo_remover_render_input", kBlockSize, &x0[0],
                        16000, 1);
  data_dumper_->DumpRaw("aec3_echo_remover_capture_input", y0);
  data_dumper_->DumpRaw("aec3_echo_remover_render_input", x0);

  aec_state_.UpdateCaptureSaturation(capture_signal_saturation);

  if (echo_path_variability.AudioPathChanged()) {
    // Ensure that the gain change is only acted on once per frame.
    if (echo_path_variability.gain_change) {
      if (gain_change_hangover_ == 0) {
        constexpr int kMaxBlocksPerFrame = 3;
        gain_change_hangover_ = kMaxBlocksPerFrame;
        RTC_LOG(LS_INFO) << "Gain change detected at block " << block_counter_;
      } else {
        echo_path_variability.gain_change = false;
      }
    }

    subtractor_.HandleEchoPathChange(echo_path_variability);
    aec_state_.HandleEchoPathChange(echo_path_variability);

    if (echo_path_variability.delay_change !=
        EchoPathVariability::DelayAdjustment::kNone) {
      suppression_gain_.SetInitialState(true);
    }
  }
  if (gain_change_hangover_ > 0) {
    --gain_change_hangover_;
  }

  float high_bands_gain;
  std::array<float, kFftLengthBy2Plus1> G;

  // Analyze the render signal.
  render_signal_analyzer_.Update(*render_buffer,
                                 aec_state_.FilterDelayBlocks());

  // Perform linear echo cancellation.
  if (aec_state_.TransitionTriggered()) {
    subtractor_.ExitInitialState();
    suppression_gain_.SetInitialState(false);
  }

  // If the delay is known, use the echo subtractor.
  subtractor_.Process(*render_buffer, y0, render_signal_analyzer_, aec_state_,
                      &subtractor_output[0]);
  std::array<float, kBlockSize> e;
  FormLinearFilterOutput(subtractor_output[0], e);

  // Compute spectra.
  WindowedPaddedFft(fft_, y0, y_old_, &Y[0]);
  WindowedPaddedFft(fft_, e, e_old_, &E[0]);
  LinearEchoPower(E[0], Y[0], &S2_linear[0]);
  Y[0].Spectrum(optimization_, Y2[0]);
  E[0].Spectrum(optimization_, E2[0]);

  // Update the AEC state information.
  aec_state_.Update(external_delay, subtractor_.FilterFrequencyResponse(),
                    subtractor_.FilterImpulseResponse(), *render_buffer, E2[0],
                    Y2[0], subtractor_output[0], y0);

  // Choose the linear output.
  data_dumper_->DumpWav("aec3_output_linear2", kBlockSize, &e[0], 16000, 1);
  if (aec_state_.UseLinearFilterOutput()) {
    if (!linear_filter_output_last_selected_) {
      SignalTransition(y0, e, y0);
    } else {
      std::copy(e.begin(), e.end(), y0.begin());
    }
  } else {
    if (linear_filter_output_last_selected_) {
      SignalTransition(e, y0, y0);
    }
  }
  linear_filter_output_last_selected_ = aec_state_.UseLinearFilterOutput();
  const auto& Y_fft = aec_state_.UseLinearFilterOutput() ? E[0] : Y[0];

  data_dumper_->DumpWav("aec3_output_linear", kBlockSize, &y0[0], 16000, 1);

  // Estimate the residual echo power.
  residual_echo_estimator_.Estimate(aec_state_, *render_buffer, S2_linear[0],
                                    Y2[0], &R2[0]);

  // Estimate the comfort noise.
  cng_.Compute(aec_state_, Y2[0], &comfort_noise[0],
               &high_band_comfort_noise[0]);

  // Suppressor echo estimate.
  const auto& echo_spectrum =
      aec_state_.UsableLinearEstimate() ? S2_linear[0] : R2[0];

  // Suppressor nearend estimate.
  std::array<float, kFftLengthBy2Plus1> nearend_spectrum_bounded;
  if (aec_state_.UsableLinearEstimate()) {
    std::transform(E2[0].begin(), E2[0].end(), Y2[0].begin(),
                   nearend_spectrum_bounded.begin(),
                   [](float a, float b) { return std::min(a, b); });
  }
  const auto& nearend_spectrum =
      aec_state_.UsableLinearEstimate() ? nearend_spectrum_bounded : Y2[0];

  // Compute and apply the suppression gain.
  suppression_gain_.GetGain(nearend_spectrum, echo_spectrum, R2[0],
                            cng_.NoiseSpectrum(), render_signal_analyzer_,
                            aec_state_, x, &high_bands_gain, &G);

  suppression_filter_.ApplyGain(comfort_noise[0], high_band_comfort_noise[0], G,
                                high_bands_gain, Y_fft, y);

  // Update the metrics.
  metrics_.Update(aec_state_, cng_.NoiseSpectrum(), G);

  // Debug outputs for the purpose of development and analysis.
  data_dumper_->DumpWav("aec3_echo_estimate", kBlockSize,
                        &subtractor_output[0].s_main[0], 16000, 1);
  data_dumper_->DumpRaw("aec3_output", y0);
  data_dumper_->DumpRaw("aec3_narrow_render",
                        render_signal_analyzer_.NarrowPeakBand() ? 1 : 0);
  data_dumper_->DumpRaw("aec3_N2", cng_.NoiseSpectrum());
  data_dumper_->DumpRaw("aec3_suppressor_gain", G);
  data_dumper_->DumpWav(
      "aec3_output", rtc::ArrayView<const float>(&y0[0], kBlockSize), 16000, 1);
  data_dumper_->DumpRaw("aec3_using_subtractor_output[0]",
                        aec_state_.UseLinearFilterOutput() ? 1 : 0);
  data_dumper_->DumpRaw("aec3_E2", E2[0]);
  data_dumper_->DumpRaw("aec3_S2_linear", S2_linear[0]);
  data_dumper_->DumpRaw("aec3_Y2", Y2[0]);
  data_dumper_->DumpRaw(
      "aec3_X2",
      render_buffer->Spectrum(aec_state_.FilterDelayBlocks(), /*channel=*/0));
  data_dumper_->DumpRaw("aec3_R2", R2[0]);
  data_dumper_->DumpRaw("aec3_R2_reverb",
                        residual_echo_estimator_.GetReverbPowerSpectrum());
  data_dumper_->DumpRaw("aec3_filter_delay", aec_state_.FilterDelayBlocks());
  data_dumper_->DumpRaw("aec3_capture_saturation",
                        aec_state_.SaturatedCapture() ? 1 : 0);
}

void EchoRemoverImpl::FormLinearFilterOutput(
    const SubtractorOutput& subtractor_output,
    rtc::ArrayView<float> output) {
  RTC_DCHECK_EQ(subtractor_output.e_main.size(), output.size());
  RTC_DCHECK_EQ(subtractor_output.e_shadow.size(), output.size());
  bool use_main_output = true;
  if (use_shadow_filter_output_) {
    // As the output of the main adaptive filter generally should be better
    // than the shadow filter output, add a margin and threshold for when
    // choosing the shadow filter output.
    if (subtractor_output.e2_shadow < 0.9f * subtractor_output.e2_main &&
        subtractor_output.y2 > 30.f * 30.f * kBlockSize &&
        (subtractor_output.s2_main > 60.f * 60.f * kBlockSize ||
         subtractor_output.s2_shadow > 60.f * 60.f * kBlockSize)) {
      use_main_output = false;
    } else {
      // If the main filter is diverged, choose the filter output that has the
      // lowest power.
      if (subtractor_output.e2_shadow < subtractor_output.e2_main &&
          subtractor_output.y2 < subtractor_output.e2_main) {
        use_main_output = false;
      }
    }
  }

  if (use_main_output) {
    if (!main_filter_output_last_selected_) {
      SignalTransition(subtractor_output.e_shadow, subtractor_output.e_main,
                       output);
    } else {
      std::copy(subtractor_output.e_main.begin(),
                subtractor_output.e_main.end(), output.begin());
    }
  } else {
    if (main_filter_output_last_selected_) {
      SignalTransition(subtractor_output.e_main, subtractor_output.e_shadow,
                       output);
    } else {
      std::copy(subtractor_output.e_shadow.begin(),
                subtractor_output.e_shadow.end(), output.begin());
    }
  }
  main_filter_output_last_selected_ = use_main_output;
}

}  // namespace

EchoRemover* EchoRemover::Create(const EchoCanceller3Config& config,
                                 int sample_rate_hz,
                                 size_t num_render_channels,
                                 size_t num_capture_channels) {
  return new EchoRemoverImpl(config, sample_rate_hz, num_render_channels,
                             num_capture_channels);
}

}  // namespace webrtc
