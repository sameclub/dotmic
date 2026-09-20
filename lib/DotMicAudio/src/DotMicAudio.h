// Copyright 2015-2026 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "soc/soc_caps.h"
#if SOC_USB_OTG_SUPPORTED
#include "sdkconfig.h"
#if CONFIG_TINYUSB_AUDIO_ENABLED
#include "esp_event.h"

/** @file DotMicAudio.h
 *  @brief USB Audio Class (UAC) microphone device API for ESP32 Arduino.
 *
 *  Scoped to DotMic: capture only, no speaker, volume or mute path.
 *  Available when @c SOC_USB_OTG_SUPPORTED and @c CONFIG_TINYUSB_AUDIO_ENABLED
 *  are enabled. Uses TinyUSB UAC1 (full-speed) or UAC2 (high-speed) depending
 *  on the build configuration.
 */

ESP_EVENT_DECLARE_BASE(ARDUINO_USB_AUDIO_CARD_EVENTS);

/** @brief Event identifiers posted on @ref ARDUINO_USB_AUDIO_CARD_EVENTS. */
typedef enum {
  ARDUINO_USB_AUDIO_CARD_ANY_EVENT = ESP_EVENT_ANY_ID, /**< Wildcard: register for all audio card events. */
  ARDUINO_USB_AUDIO_CARD_SAMPLE_RATE_EVENT,            /**< Sample rate changed. */
  ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT,       /**< Microphone streaming interface alt setting changed. */
  ARDUINO_USB_AUDIO_CARD_MAX_EVENT,                    /**< Upper bound for valid event IDs (exclusive). */
} arduino_usb_audio_card_event_t;

/** @brief Number of microphone (capture) channels exposed in the USB descriptor. */
typedef enum {
  UAC_MIC_MONO = 1, /**< Mono microphone. */
  UAC_MIC_STEREO    /**< Stereo microphone. */
} UAC_MIC_Channels;

/** @brief PCM bit depth used for USB audio streams. */
typedef enum {
  UAC_BPS_16 = 16, /**< 16-bit samples. */
  UAC_BPS_24 = 24, /**< 24-bit samples (stored in 32-bit containers where applicable). */
  UAC_BPS_32 = 32  /**< 32-bit samples. */
} UAC_Bits_Per_Sample;

/** @brief Payload for @ref ARDUINO_USB_AUDIO_CARD_EVENTS; only one branch is valid per event type. */
typedef union {
  struct {
    uint32_t rate; /**< Sample rate in Hz. */
  } sample_rate;   /**< Valid for @ref ARDUINO_USB_AUDIO_CARD_SAMPLE_RATE_EVENT. */
  struct {
    bool enable; /**< @c true if the non-zero alternate setting is active (streaming). */
  } interface_enable; /**< Valid for @ref ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT. */
} arduino_usb_audio_card_event_data_t;

/** @brief USB microphone device (UAC).
 *
 *  Construct with the desired sample rate, bit depth and channel count; the
 *  constructor registers the USB audio interface. Use write() to send captured
 *  PCM toward the host and onEvent() for host control changes.
 */
class DotMicAudio {
public:
  /** @brief Creates the audio device configuration and registers the USB audio interface.
   *  @param sample_rate   Initial sample rate in Hz (UAC1 full-speed: typically up to 48000).
   *  @param bps           Bits per sample (@ref UAC_Bits_Per_Sample).
   *  @param mic_channels  Microphone channel layout (@ref UAC_MIC_Channels).
   */
  DotMicAudio(uint32_t sample_rate, UAC_Bits_Per_Sample bps, UAC_MIC_Channels mic_channels = UAC_MIC_MONO);
  ~DotMicAudio();

  /** @brief Sends microphone PCM toward the host (USB IN).
   *  @param data PCM buffer.
   *  @param len  Number of bytes to send.
   *  @return Number of bytes accepted, or 0 if the device is not active.
   */
  uint16_t write(const void *data, uint16_t len);

  /** @brief Registers a handler for all audio card events (@ref ARDUINO_USB_AUDIO_CARD_ANY_EVENT). */
  void onEvent(esp_event_handler_t callback);
  /** @brief Registers a handler for a specific event ID.
   *  @param event    Event to subscribe to, or @ref ARDUINO_USB_AUDIO_CARD_ANY_EVENT for all.
   *  @param callback ESP-IDF event handler; @a event_handler_arg will be @c this (the @ref DotMicAudio instance).
   */
  void onEvent(arduino_usb_audio_card_event_t event, esp_event_handler_t callback);
};

#endif /* CONFIG_TINYUSB_AUDIO_ENABLED */
#endif /* SOC_USB_OTG_SUPPORTED */
