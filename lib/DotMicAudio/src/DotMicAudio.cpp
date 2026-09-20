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
#include "DotMicAudio.h"
#if SOC_USB_OTG_SUPPORTED
#if CONFIG_TINYUSB_AUDIO_ENABLED

#include "esp32-hal-tinyusb.h"
#include "DotMicAudioDescriptors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <inttypes.h>

ESP_EVENT_DEFINE_BASE(ARDUINO_USB_AUDIO_CARD_EVENTS);
esp_err_t arduino_usb_event_post(esp_event_base_t event_base, int32_t event_id, void *event_data, size_t event_data_size, TickType_t ticks_to_wait);
esp_err_t arduino_usb_event_handler_register_with(esp_event_base_t event_base, int32_t event_id, esp_event_handler_t event_handler, void *event_handler_arg);

static uint8_t _itf_num = 0;
static uint32_t _sample_rate = 48000;
static uint8_t _mic_channels = 2;
static uint8_t _bits_per_sample = 24;
static uint8_t _bytes_per_sample = 4;
static DotMicAudio *_uac = NULL;

uint16_t tusb_audio_load_descriptor(uint8_t *dst, uint8_t *itf) {
  _itf_num = *itf;
#if TUD_OPT_HIGH_SPEED
  uint8_t str_index = tinyusb_add_string_descriptor("TinyUSB UAC2");
  uint8_t ep_num = tinyusb_get_free_in_endpoint();
  TU_VERIFY(ep_num != 0);
  uint8_t int_ep_num = tinyusb_get_free_in_endpoint();
  TU_VERIFY(int_ep_num != 0);
  uint8_t descriptor[TUD_AUDIO20_MICROPHONE_DESC_LEN] = {
    // Interface number, string index, EP In & EP Interrupt address, max sample rate, mic channels, bytes per sample, bits used per sample
    TUD_AUDIO20_MICROPHONE_DESCRIPTOR(
      _itf_num, str_index, (uint8_t)(ep_num | 0x80), (uint8_t)(int_ep_num | 0x80), CFG_TUD_AUDIO_MAX_SAMPLE_RATE, _mic_channels, _bytes_per_sample,
      _bits_per_sample
    )
  };
  *itf += 2;
  memcpy(dst, descriptor, TUD_AUDIO20_MICROPHONE_DESC_LEN);
  return TUD_AUDIO20_MICROPHONE_DESC_LEN;
#else
  uint8_t str_index = tinyusb_add_string_descriptor("S3AI DotMic Microphone");
  uint8_t ep_num = tinyusb_get_free_in_endpoint();
  TU_VERIFY(ep_num != 0);
  uint8_t descriptor[TUD_AUDIO10_MICROPHONE_DESC_LEN(1)] = {
    // Interface number, string index, EP In address, max sample rate, microphone channels, bytes per sample, bits used per sample, sample rate
    TUD_AUDIO10_MICROPHONE_DESCRIPTOR(_itf_num, str_index, (uint8_t)(ep_num | 0x80), 48000, _mic_channels, _bytes_per_sample, _bits_per_sample, _sample_rate)
  };
  *itf += 2;
  memcpy(dst, descriptor, TUD_AUDIO10_MICROPHONE_DESC_LEN(1));
  return TUD_AUDIO10_MICROPHONE_DESC_LEN(1);
#endif
}

#define dump_control_request(p_request)                                                                                                          \
  log_v(                                                                                                                                         \
    "Control request: bRequest = 0x%x, wValue = 0x%x, wIndex = 0x%x, wLength = 0x%x", p_request->bRequest, p_request->wValue, p_request->wIndex, \
    p_request->wLength                                                                                                                           \
  )

// Invoked when audio class specific set request received for an EP
bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
  (void)rhport;
  dump_control_request(p_request);
#if TUD_OPT_HIGH_SPEED
  (void)pBuff;
  (void)p_request;
  return false;
#else
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  if (ctrlSel == AUDIO10_EP_CTRL_SAMPLING_FREQ && p_request->bRequest == AUDIO10_CS_REQ_SET_CUR && p_request->wLength == 3) {
    // USB sampling-frequency payload is exactly three bytes, not four.
    uint32_t requested = uint32_t(pBuff[0]) | (uint32_t(pBuff[1]) << 8) | (uint32_t(pBuff[2]) << 16);
    if (requested != _sample_rate) return false; // fixed-rate microphone descriptor
    log_d("EP set current freq: %" PRIu32, _sample_rate);
    // Send SAMPLE RATE Event
    arduino_usb_audio_card_event_data_t p;
    p.sample_rate.rate = _sample_rate;
    arduino_usb_event_post(
      ARDUINO_USB_AUDIO_CARD_EVENTS, ARDUINO_USB_AUDIO_CARD_SAMPLE_RATE_EVENT, &p, sizeof(arduino_usb_audio_card_event_data_t), portMAX_DELAY
    );
    return true;
  }
  log_w("Set EP request not handled, ctrlSel = %d, bRequest = %d, wLength = %d", ctrlSel, p_request->bRequest, p_request->wLength);
  return false;
#endif
}

// Invoked when audio class specific get request received for an EP
bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  dump_control_request(p_request);
#if TUD_OPT_HIGH_SPEED
  (void)rhport;
  (void)p_request;
  return false;
#else
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  if (ctrlSel == AUDIO10_EP_CTRL_SAMPLING_FREQ && p_request->bRequest == AUDIO10_CS_REQ_GET_CUR) {
    log_d("EP get current freq");
    uint8_t freq[3];
    freq[0] = (uint8_t)(_sample_rate & 0xFF);
    freq[1] = (uint8_t)((_sample_rate >> 8) & 0xFF);
    freq[2] = (uint8_t)((_sample_rate >> 16) & 0xFF);
    return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, freq, sizeof(freq));
  }
  log_w("Get EP request not handled, ctrlSel = %d, bRequest = %d, wLength = %d", ctrlSel, p_request->bRequest, p_request->wLength);
  return false;
#endif
}

// Invoked when audio class specific get request received for an entity
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  dump_control_request(p_request);
#if TUD_OPT_HIGH_SPEED
  audio20_control_request_t const *request = (audio20_control_request_t const *)p_request;
  if (request->bEntityID == UAC2_ENTITY_CLOCK) {
    if (request->bControlSelector == AUDIO20_CS_CTRL_SAM_FREQ) {
      if (request->bRequest == AUDIO20_CS_REQ_CUR) {
        log_d("Clock get current freq %" PRIu32, _sample_rate);
        audio20_control_cur_4_t curf = {(int32_t)tu_htole32(_sample_rate)};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &curf, sizeof(curf));
      } else if (request->bRequest == AUDIO20_CS_REQ_RANGE) {
        audio20_control_range_4_n_t(1) rangef = {.wNumSubRanges = tu_htole16(1)};
        rangef.subrange[0].bMin = (int32_t)tu_htole32(_sample_rate);
        rangef.subrange[0].bMax = (int32_t)tu_htole32(_sample_rate);
        rangef.subrange[0].bRes = (int32_t)tu_htole32(0);
        log_d("Clock Range %" PRIu32 ", %" PRIu32 ", %d", _sample_rate, _sample_rate, 0);
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &rangef, sizeof(rangef));
      }
    } else if (request->bControlSelector == AUDIO20_CS_CTRL_CLK_VALID && request->bRequest == AUDIO20_CS_REQ_CUR) {
      audio20_control_cur_1_t cur_valid = {.bCur = 1};
      log_d("Clock get is valid %u", cur_valid.bCur);
      return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_valid, sizeof(cur_valid));
    }
    log_w("Clock get request not supported, entity = %u, selector = %u, request = %u", request->bEntityID, request->bControlSelector, request->bRequest);
    return false;
  }
  log_w("Get request not handled, entity = %d, selector = %d, request = %d", request->bEntityID, request->bControlSelector, request->bRequest);
  return false;
#else
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t entityID = TU_U16_HIGH(p_request->wIndex);
  log_w("Get request not handled, entityID = %d, ctrlSel = %d, bRequest = %d, wLength = %d", entityID, ctrlSel, p_request->bRequest, p_request->wLength);
  return false;
#endif
}

// Invoked when audio class specific set request received for an entity
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
  (void)rhport;
  (void)buf;
  dump_control_request(p_request);
#if TUD_OPT_HIGH_SPEED
  audio20_control_request_t const *request = (audio20_control_request_t const *)p_request;
  if (request->bEntityID == UAC2_ENTITY_CLOCK && request->bRequest == AUDIO20_CS_REQ_CUR) {
    if (request->bControlSelector == AUDIO20_CS_CTRL_SAM_FREQ) {
      TU_VERIFY(request->wLength == sizeof(audio20_control_cur_4_t));
      _sample_rate = (uint32_t)((audio20_control_cur_4_t const *)buf)->bCur;
      log_d("Clock set current freq: %" PRIu32, _sample_rate);
      // Send SAMPLE RATE Event
      arduino_usb_audio_card_event_data_t p;
      p.sample_rate.rate = _sample_rate;
      arduino_usb_event_post(
        ARDUINO_USB_AUDIO_CARD_EVENTS, ARDUINO_USB_AUDIO_CARD_SAMPLE_RATE_EVENT, &p, sizeof(arduino_usb_audio_card_event_data_t), portMAX_DELAY
      );
      return true;
    }
    log_w("Clock set request not supported, entity = %u, selector = %u, request = %u", request->bEntityID, request->bControlSelector, request->bRequest);
    return false;
  }
  log_w("Set request not handled, entity = %d, selector = %d, request = %d", request->bEntityID, request->bControlSelector, request->bRequest);
  return false;
#else
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t entityID = TU_U16_HIGH(p_request->wIndex);
  log_w("Set request not handled, entityID = %d, ctrlSel = %d, bRequest = %d, wLength = %d", entityID, ctrlSel, p_request->bRequest, p_request->wLength);
  return false;
#endif
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void)rhport;
  dump_control_request(p_request);
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_DEBUG
  uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex)) - _itf_num;
  uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));
  log_d("Close EP interface %d alt %d", itf, alt);
#endif
  return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void)rhport;
  dump_control_request(p_request);
  uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));
  log_d("Set microphone interface alt %d", alt);
  // Send Interface Event
  arduino_usb_audio_card_event_data_t p;
  p.interface_enable.enable = alt != 0;
  arduino_usb_event_post(
    ARDUINO_USB_AUDIO_CARD_EVENTS, ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT, &p, sizeof(arduino_usb_audio_card_event_data_t), portMAX_DELAY
  );
  return true;
}

DotMicAudio::DotMicAudio(uint32_t sample_rate, UAC_Bits_Per_Sample bps, UAC_MIC_Channels mic_channels) {
  if (_uac == NULL) {
    _uac = this;
    _sample_rate = sample_rate;
    _bits_per_sample = (uint8_t)bps;
    _bytes_per_sample = (_bits_per_sample <= 16) ? 2 : 4;
    _mic_channels = (uint8_t)mic_channels;

    uint16_t descriptor_len = 0;
#if TUD_OPT_HIGH_SPEED
    if (_sample_rate > CFG_TUD_AUDIO_MAX_SAMPLE_RATE) {
      log_e("Maximum %u sample rate supported!", CFG_TUD_AUDIO_MAX_SAMPLE_RATE);
      _uac = NULL;
      return;
    }
    descriptor_len = TUD_AUDIO20_MICROPHONE_DESC_LEN;
#else
    if (_sample_rate > 48000) {
      log_e("Maximum 48000 sample rate supported!");
      _uac = NULL;
      return;
    }
    if ((_mic_channels * _bytes_per_sample) > 8) {
      log_e("Too many channels or too high bits per sample selected! Audio might not work!");
    }
    descriptor_len = TUD_AUDIO10_MICROPHONE_DESC_LEN(1);
#endif
    tinyusb_enable_interface(USB_INTERFACE_AUDIO, descriptor_len, tusb_audio_load_descriptor);
  }
}

DotMicAudio::~DotMicAudio() {
  _uac = NULL;
}

uint16_t DotMicAudio::write(const void *data, uint16_t len) {
  if (_uac != NULL) {
    return tud_audio_write(data, len);
  }
  return 0;
}

void DotMicAudio::onEvent(esp_event_handler_t callback) {
  onEvent(ARDUINO_USB_AUDIO_CARD_ANY_EVENT, callback);
}

void DotMicAudio::onEvent(arduino_usb_audio_card_event_t event, esp_event_handler_t callback) {
  arduino_usb_event_handler_register_with(ARDUINO_USB_AUDIO_CARD_EVENTS, event, callback, this);
}

#endif /* CONFIG_TINYUSB_AUDIO_ENABLED */
#endif /* SOC_USB_OTG_SUPPORTED */
