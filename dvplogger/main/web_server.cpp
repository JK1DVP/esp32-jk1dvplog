/*
 * dvplogger - field companion for ham radio operator
 * dvplogger - アマチュア無線家のためのフィールド支援ツール
 * Copyright (c) 2021-2026 Eiichiro Araki
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
// SPDX-FileCopyrightText: 2025 2021-2025 Eiichiro Araki
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "web_server.h"
#include "decl.h"
#include "cluster.h"
#include "zserver.h"
#include "network.h"
#include "SD.h"
#include "variables.h"
#include "qso.h"
//#include <SPIFFS.h>
#include <math.h>
#include <maidenhead.h>
#include "settings.h"
#include "misc.h"
#include "cat.h"
#include <map>  // std::mapを使用するためにインクルード
#include "so2r.h"
#include "log.h"
#include "contest.h"
#include "user_contest_md.h"
#include "timekeep.h"
#include "esp_heap_caps.h"
#include "bandmap.h"
#include "dupechk.h"
#include "antenna.h"
#include "satellite.h"
#include "ui.h"
#include "Plan13.h"
#include "multi_process.h"
#include "iambic_keyer.h"
#include "cw_keying.h"
#include <algorithm>
#include <memory>


#ifndef BANDMAP_TRACE
#define BANDMAP_TRACE 0
#endif
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>

// ui.cpp currently does not expose this display helper in ui.h.
// Declare it here so the Web WPM command can reuse the LCD speed display.
void show_cw_spd();

namespace {
constexpr size_t WEB_LOG_LINE_SIZE = 192;
constexpr uint8_t WEB_LOG_QUEUE_LEN = 4;

struct WebLogLine {
  char text[WEB_LOG_LINE_SIZE];
};

static WebLogLine web_log_queue[WEB_LOG_QUEUE_LEN];
static volatile uint8_t web_log_head = 0;
static volatile uint8_t web_log_tail = 0;
static volatile uint32_t web_log_dropped = 0;
static portMUX_TYPE web_log_mux = portMUX_INITIALIZER_UNLOCKED;

static void enqueue_web_log_line(const char *text) {
  if (!text) return;
  portENTER_CRITICAL(&web_log_mux);
  uint8_t next = (uint8_t)((web_log_head + 1) % WEB_LOG_QUEUE_LEN);
  if (next == web_log_tail) {
    ++web_log_dropped;
    portEXIT_CRITICAL(&web_log_mux);
    return;
  }
  strlcpy(web_log_queue[web_log_head].text, text, WEB_LOG_LINE_SIZE);
  web_log_head = next;
  portEXIT_CRITICAL(&web_log_mux);
}

class DeferredWebLogPrint : public Print {
public:
  size_t write(uint8_t c) override {
    portENTER_CRITICAL(&mux_);
    if (c == '\r') {
      portEXIT_CRITICAL(&mux_);
      return 1;
    }
    if (c == '\n' || len_ >= sizeof(line_) - 1) {
      line_[len_] = '\0';
      if (len_ > 0) enqueue_web_log_line(line_);
      len_ = 0;
      if (c != '\n') line_[len_++] = (char)c;
    } else {
      line_[len_++] = (char)c;
    }
    portEXIT_CRITICAL(&mux_);
    return 1;
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    if (!buffer) return 0;
    for (size_t i = 0; i < size; ++i) write(buffer[i]);
    return size;
  }

private:
  char line_[WEB_LOG_LINE_SIZE];
  size_t len_ = 0;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

static DeferredWebLogPrint webLog;

static String normalize_op_value(int index, const String &source) {
  String out;
  out.reserve(source.length());
  for (size_t i = 0; i < source.length(); ++i) {
    unsigned char uc = (unsigned char)source[i];
    char c = (char)uc;
    if (index == 0 || index == 1 || index == 4 || index == 5 ||
        index == 10 || index == 11 || index == 12) {
      if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    }
    bool accept = false;
    switch (index) {
      case 0: case 1:
        accept = ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '-');
        break;
      case 4:
        // Sent Exch is a CW macro source.  Preserve characters such as '$'
        // exactly as entered; expansion is performed only when transmitting.
        accept = (uc >= 0x21 && uc <= 0x7e);
        break;
      case 2: case 3:
        accept = (c >= '0' && c <= '9');
        break;
      case 5: case 10: case 11: case 12:
        accept = (uc >= 0x21 && uc <= 0x7e);  // printable ASCII except space
        break;
      case 13:
        accept = (uc >= 0x21 && uc <= 0x7e);  // contest name: keep case, omit spaces
        break;
      default:
        accept = (uc >= 0x20 && uc <= 0x7e);
        break;
    }
    if (accept) out += c;
  }
  return out;
}

static void normalize_op_cstr(int index, char *dst, size_t dst_size, const String &source) {
  String value = normalize_op_value(index, source);
  strlcpy(dst, value.c_str(), dst_size);
}

enum WebUiCommandType : uint8_t {
  WEB_UI_KEY=1,
  WEB_UI_CONTROL,
  WEB_UI_ENTER,
  WEB_UI_SET,
  WEB_UI_CW_SEND,
  WEB_UI_WPM_SET,
  WEB_UI_RADIO_MODE,
  WEB_UI_TONE_CW,
  WEB_UI_CQSP_TOGGLE
};
struct WebUiCommand {
  uint8_t type;
  int16_t value;
  int8_t index;
  int8_t radio_index;  // focused radio captured when the Web command was queued
  char name[12];
  // input0 is also used for Sent Exch, which is longer than recv_exch.
  char input0[100];
  char input1[LEN_DUAL_EXCH_WINDOW + 1];
};
static QueueHandle_t s_web_ui_queue = nullptr;

static bool enqueue_web_ui(const WebUiCommand &cmd) {
  if (!s_web_ui_queue) s_web_ui_queue = xQueueCreate(8, sizeof(WebUiCommand));
  return s_web_ui_queue && xQueueSend(s_web_ui_queue, &cmd, 0) == pdTRUE;
}
}

void process_web_ui_queue() {
  if (!s_web_ui_queue) return;
  WebUiCommand cmd;
  int budget=8;
  while (budget-- > 0 && xQueueReceive(s_web_ui_queue, &cmd, 0) == pdTRUE) {
    struct radio *radio = so2r.radio_selected();
    if (cmd.type == WEB_UI_KEY) {
      if (cmd.value >= 112 && cmd.value <= 116) {
        so2r.cancel_msg_tx();
        so2r.set_msg_tx_to_focused();
        so2r.set_rx_in_sending_msg();
        function_keys(cmd.value - 54, 0);
      } else if (cmd.value == 27) {
        so2r.cancel_msg_tx();
        switch (so2r.radio_mode) {
          case SO2R::RADIO_MODE_SO2R: so2r.sequence_mode(SO2R::SO2R_CQSandP); break;
          case SO2R::RADIO_MODE_SO1R:
          case SO2R::RADIO_MODE_SAT: so2r.sequence_mode(SO2R::Manual); break;
        }
        so2r.sequence_stat(SO2R::Default);
      }
    } else if (cmd.type == WEB_UI_CONTROL) {
      if (!strcmp(cmd.name,"Radio")) {
        so2r.change_focused_radio(cmd.value);
      } else if (!strcmp(cmd.name,"Mode")) {
        radio=so2r.radio_selected();
        int mt=modetype_string(cmd.input0);
        int filt=radio->filtbank[radio->bandid][radio->cq[mt]][mt];
        if (!filt) filt=default_filt(cmd.input0);
        set_mode(cmd.input0,filt,radio);
        send_mode_set_civ(cmd.input0,filt);
        upd_display();
      } else if (!strcmp(cmd.name,"Band")) {
        radio=so2r.radio_selected();
        if (cmd.value>=1 && cmd.value<N_BAND && (((1<<(cmd.value-1)) & radio->band_mask)==0))
          band_change(cmd.value,radio);
      }
    } else if (cmd.type == WEB_UI_ENTER) {
      // Keep the radio selected on /op when Enter was pressed.  The queue is
      // processed later in the main loop, so relying on the then-current
      // focus can apply the command to a different radio.
      if (cmd.radio_index >= 0 && cmd.radio_index < N_RADIO &&
          cmd.radio_index != so2r.focused_radio()) {
        so2r.change_focused_radio(cmd.radio_index);
      }
      radio = so2r.radio_selected();

      // Populate the same edit buffers used by the LCD/keyboard UI, then run
      // the common Enter interpreter.  This preserves command, frequency and
      // mode parsing (for example, "7030" sets 7030 kHz).  When the Call
      // field is not a recognised command, process_enter() continues through
      // the normal ESM behavior.
      set_callsign_and_request_dupe(radio, cmd.input0, true);
      strlcpy(radio->recv_exch + 2, cmd.input1, LEN_DUAL_EXCH_WINDOW + 1);
      radio->ptr_curr = (cmd.index == 1) ? 1 : 0;
      // value=1 is the /op "Log (No TX)" action: commit the QSO using
      // the normal Enter path, but suppress ESM exchange/TU transmission.
      process_enter(cmd.value ? 1 : 0);
    } else if (cmd.type == WEB_UI_CW_SEND) {
      if (cmd.radio_index < 0 || cmd.radio_index >= N_RADIO) continue;

      // The browser supplies the /op-selected radio explicitly.  Do not move
      // focused_radio here: in SO2R the RX/focus may legitimately move during
      // transmission, while msg_tx_radio/tx must remain fixed.
      radio = &radio_list[cmd.radio_index];

      const bool send_as_cw =
          (radio->modetype == LOG_MODETYPE_CW) || radio->f_tone_keying;
      const bool send_as_voice = (radio->modetype == LOG_MODETYPE_PH);

      if (!send_as_cw && !send_as_voice) {
        upd_display_info_flash("CW/Voice unavailable\nfor this mode");
        continue;
      }
      if (send_as_voice && plogw->voice_memory_enable < 3) {
        upd_display_info_flash("Voice text requires\nVoiceMemory=3");
        continue;
      }

      so2r.cancel_msg_tx();
      so2r.set_msg_tx_to_radio(cmd.radio_index);
      so2r.set_tx_to_msg_tx();
      so2r.set_rx_in_sending_msg();

      if (send_as_cw) {
        // Feed the normal CW buffer so case, punctuation and $ macros are
        // handled exactly like LCD/function-key CW messages.
        append_cwbuf_string(cmd.input0);
        append_cwbuf('$');
      } else {
        // PHONE arbitrary text uses the existing SUBCPU voice-synthesis macro
        // path.  Its pronunciation normalization is intentionally voice-only.
        so2r.play_string_macro(cmd.input0);
      }
      so2r.sequence_stat(SO2R::Sending_Msg);
    } else if (cmd.type == WEB_UI_WPM_SET) {
      cw_spd = cmd.value;
      clamp_cw_speed();
      loadWPM(cw_spd);
      save_settings("");
      show_cw_spd();
    } else if (cmd.type == WEB_UI_CQSP_TOGGLE) {
      if (cmd.radio_index < 0 || cmd.radio_index >= N_RADIO) continue;
      radio = &radio_list[cmd.radio_index];

      // Match the normal Alt-Q CQ/S&P transition: preserve the current
      // frequency/mode/filter bank, switch the operating state, remember it
      // for this band/mode, then recall the corresponding bank.
      save_freq_mode_filt(radio);
      radio->cq[radio->modetype] =
          (radio->cq[radio->modetype] == LOG_CQ) ? LOG_SandP : LOG_CQ;
      radio->cq_bank[radio->bandid][radio->modetype] = radio->cq[radio->modetype];
      recall_freq_mode_filt(radio);

      request_display_update_on_demand();
      request_bandmap_update_on_demand();
      upd_display_stat();
      bandmap_disp.f_update = 0;
      upd_display();
    } else if (cmd.type == WEB_UI_TONE_CW) {
      if (cmd.radio_index < 0 || cmd.radio_index >= N_RADIO) continue;
      radio = &radio_list[cmd.radio_index];
      radio->f_tone_keying = !radio->f_tone_keying;
      set_tone_keying(radio);
      set_tone(0, 0);
      keying(0);
      clear_cwbuf();
      if (radio->f_tone_keying) {
        radio->modetype = LOG_MODETYPE_CW;
      } else {
        radio->modetype = modetype_string(radio->opmode);
      }
      set_log_rst(radio);
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "Tone CW=%d\nRadio %d",
               radio->f_tone_keying ? 1 : 0, cmd.radio_index);
      upd_display_info_flash(dp->lcdbuf);
      upd_display();
    } else if (cmd.type == WEB_UI_RADIO_MODE) {
      if (cmd.value < SO2R::RADIO_MODE_SO1R || cmd.value > SO2R::RADIO_MODE_SO2R) continue;
      bool transmitting = (so2r.sequence_stat() != SO2R::Default);
      for (int i = 0; i < N_RADIO && !transmitting; ++i) transmitting = radio_list[i].ptt_stat != 0;
      if (transmitting) { upd_display_info_flash("RADIO MODE BUSY\nTX/sequence active"); continue; }
      so2r.cancel_msg_tx();
      so2r.radio_mode = static_cast<SO2R::RadioMode>(cmd.value);
      if (so2r.radio_mode == SO2R::RADIO_MODE_SO2R) {
        so2r.validate_so2r_pairs();
        so2r.sequence_mode(SO2R::SO2R_CQSandP);
      } else {
        so2r.sequence_mode(SO2R::Manual);
      }
      so2r.sequence_stat(SO2R::Default);
      so2r.set_status();
      save_settings("");
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "RADIO MODE\n%s\nSaved",
               so2r.radio_mode == SO2R::RADIO_MODE_SO1R ? "SO1R" :
               so2r.radio_mode == SO2R::RADIO_MODE_SAT ? "SAT" : "SO2R");
      request_display_update_on_demand();
      request_bandmap_update_on_demand();
      upd_display_info_flash(dp->lcdbuf);
    } else if (cmd.type == WEB_UI_SET) {
      switch (cmd.index) {
        case 0:
          set_callsign_and_request_dupe(radio, cmd.input0, true);
          break;
        case 1: strlcpy(radio->recv_exch+2, cmd.input0, LEN_DUAL_EXCH_WINDOW + 1); break;
        case 2: strlcpy(radio->recv_rst+2, cmd.input0, LEN_RST_WINDOW + 1); break;
        case 3: strlcpy(radio->sent_rst+2, cmd.input0, LEN_RST_WINDOW + 1); break;
        case 4:
          // Keep the unexpanded macro text (for example, "11$P") in the
          // runtime setting.  expand_sent_exch() expands it only for output.
          strlcpy(plogw->sent_exch + 2, cmd.input0, LEN_SENT_EXCH_WINDOW + 1);
          plogw->sent_exch[1] = strlen(plogw->sent_exch + 2);
          // Keep the currently selected contest preset in sync with /op.
          save_contest_runtime_preset(plogw->contest_name + 2);
          break;
        case 5: strlcpy(plogw->my_callsign+2, cmd.input0, LEN_CALL_WINDOW + 1); break;
      }
      upd_display();
    }
  }
}

void process_web_terminal_log_queue() {
  for (;;) {
    WebLogLine line;
    bool have_line = false;
    uint32_t dropped = 0;

    portENTER_CRITICAL(&web_log_mux);
    if (web_log_tail != web_log_head) {
      line = web_log_queue[web_log_tail];
      web_log_tail = (uint8_t)((web_log_tail + 1) % WEB_LOG_QUEUE_LEN);
      have_line = true;
    } else if (web_log_dropped != 0) {
      dropped = web_log_dropped;
      web_log_dropped = 0;
    }
    portEXIT_CRITICAL(&web_log_mux);

    if (have_line) {
      console->print("[WEB] ");
      console->println(line.text);
      continue;
    }
    if (dropped) {
      console->printf("[WEB] %lu log message(s) dropped\n", (unsigned long)dropped);
    }
    break;
  }
}


AsyncWebServer web_server(80);

const char* PARAM_MESSAGE = "message";

void notFound(AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
}


void rebootESP(String message) {
  webLog.print("Rebooting ESP32: "); webLog.println(message);
  ESP.restart();
}

String humanReadableSize(const size_t bytes);

// Format a byte count without allocating Arduino String objects.
static void humanReadableSizeToBuffer(size_t bytes, char *out, size_t out_size) {
  if (!out || out_size == 0) return;
  if (bytes < 1024) {
    snprintf(out, out_size, "%u B", (unsigned)bytes);
  } else if (bytes < (1024UL * 1024UL)) {
    snprintf(out, out_size, "%.1f KB", (double)bytes / 1024.0);
  } else if (bytes < (1024UL * 1024UL * 1024UL)) {
    snprintf(out, out_size, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
  } else {
    snprintf(out, out_size, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
  }
}

// Stream the SD-card directory as HTML.  Only one table row is retained in
// RAM, rather than constructing the complete file list in a String.
static void setupSdFileListHandler() {
  web_server.on("/filelist", HTTP_GET, [](AsyncWebServerRequest *request) {
    struct FileListState {
      enum Stage : uint8_t { Header, OpenEntry, Row, Footer, Done } stage = Header;
      File root;
      File entry;
      size_t offset = 0;
      size_t length = 0;
      char text[320];
    };

    std::shared_ptr<FileListState> state = std::make_shared<FileListState>();
    if (!state) {
      request->send(503, "text/plain", "Not enough memory for SD file list");
      return;
    }

    state->root = SD.open("/");
    if (!state->root) {
      request->send(500, "text/plain", "Failed to open SD card root");
      return;
    }

    webLog.println("Listing files stored on SD card (streamed)");

    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "text/html; charset=utf-8",
      [state](uint8_t *buffer, size_t maxLen, size_t index) mutable -> size_t {
        (void)index;
        size_t written = 0;

        auto copy_pending = [&]() -> bool {
          if (state->offset >= state->length) return true;
          const size_t remain = state->length - state->offset;
          const size_t available = maxLen - written;
          const size_t ncopy = remain < available ? remain : available;
          if (ncopy) {
            memcpy(buffer + written, state->text + state->offset, ncopy);
            state->offset += ncopy;
            written += ncopy;
          }
          return state->offset >= state->length;
        };

        auto set_text = [&](const char *text) {
          strlcpy(state->text, text, sizeof(state->text));
          state->length = strlen(state->text);
          state->offset = 0;
        };

        while (written < maxLen && state->stage != FileListState::Done) {
          switch (state->stage) {
          case FileListState::Header:
            if (state->length == 0) {
              set_text("<table><tr><th align='left'>Name</th>"
                       "<th align='left'>Size</th>"
                       "<th align='left'>Modified</th></tr>");
            }
            if (copy_pending()) {
              state->length = 0;
              state->stage = FileListState::OpenEntry;
            }
            break;

          case FileListState::OpenEntry:
            state->entry = state->root.openNextFile();
            if (!state->entry) {
              state->root.close();
              state->stage = FileListState::Footer;
              break;
            }
            {
              char size_text[32];
              char modified_text[32] = "-";
              humanReadableSizeToBuffer(state->entry.size(), size_text, sizeof(size_text));

              time_t modified = state->entry.getLastWrite();
              if (modified > 0) {
                struct tm tm_info;
                if (localtime_r(&modified, &tm_info) != nullptr) {
                  strftime(modified_text, sizeof(modified_text),
                           "%Y-%m-%d %H:%M:%S", &tm_info);
                }
              }

              snprintf(state->text, sizeof(state->text),
                       "<tr align='left'><td>%s</td><td>%s</td><td>%s</td></tr>",
                       state->entry.name(), size_text, modified_text);
              state->entry.close();
              state->length = strlen(state->text);
              state->offset = 0;
              state->stage = FileListState::Row;
            }
            break;

          case FileListState::Row:
            if (copy_pending()) {
              state->length = 0;
              state->stage = FileListState::OpenEntry;
            }
            break;

          case FileListState::Footer:
            if (state->length == 0) set_text("</table>");
            if (copy_pending()) {
              state->length = 0;
              state->stage = FileListState::Done;
            }
            break;

          case FileListState::Done:
            break;
          }
        }
        return written;
      });

    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });
}

// Make size of files human readable.  Used only for the three small storage
// values inserted into the top page; the directory itself is streamed above.
String humanReadableSize(const size_t bytes) {
  char text[32];
  humanReadableSizeToBuffer(bytes, text, sizeof(text));
  return String(text);
}

// Upload to an 8.3-compatible temporary name first.  The existing target
// remains untouched until the complete request has been received, flushed,
// closed, reopened, and its stored size has been verified.
//
// Example:
//   app0.bin   -> app0.tmp   (incoming upload)
//                app0.bak   (old target during commit)
//   spiffs.bin -> spiffs.tmp / spiffs.bak
//
// This intentionally assumes an 8.3 target filename.  Using the same base
// name with TMP/BAK extensions keeps all sidecar names within the same limit.
static bool makeUploadSidecarNames(const String& filename,
                                   String& targetPath,
                                   String& tempPath,
                                   String& backupPath) {
  String name = filename;

  // Browsers normally send only a basename, but discard any client-side path
  // defensively before constructing an SD-card path.
  int slash = name.lastIndexOf('/');
  int backslash = name.lastIndexOf('\\');
  int cut = slash > backslash ? slash : backslash;
  if (cut >= 0) name = name.substring(cut + 1);

  if (name.length() == 0) return false;

  int dot = name.lastIndexOf('.');
  String base = (dot > 0) ? name.substring(0, dot) : name;
  String ext = (dot > 0) ? name.substring(dot + 1) : String("");

  // The SD configuration used by DVPlogger is limited to 8.3 names.  Reject
  // a name that cannot be represented rather than silently truncating it and
  // possibly overwriting another file.
  if (base.length() == 0 || base.length() > 8 || ext.length() > 3) {
    return false;
  }

  targetPath = "/" + name;
  tempPath = "/" + base + ".tmp";
  backupPath = "/" + base + ".bak";
  return true;
}

// Progress logging is deliberately sparse.  Logging every TCP-sized chunk can
// enqueue thousands of messages during a firmware image upload and starve the
// asynchronous web/TCP processing that is receiving the file.
void handleUpload(AsyncWebServerRequest *request, String filename, size_t index,
                  uint8_t *data, size_t len, bool final) {
  String targetPath;
  String tempPath;
  String backupPath;
  if (!makeUploadSidecarNames(filename, targetPath, tempPath, backupPath)) {
    if (index == 0) {
      webLog.print("Upload Error: filename is not 8.3 compatible: ");
      webLog.println(filename);
    }
    return;
  }

  if (index == 0) {
    // A stale .tmp means a previous HTTP upload was interrupted.  Removing it
    // is safe because the committed target was never touched by that request.
    if (SD.exists(tempPath)) {
      SD.remove(tempPath);
    }
    request->_tempFile = SD.open(tempPath, "w");

    webLog.print("Upload Start: ");
    webLog.print(filename);
    webLog.print(" temp=");
    webLog.print(tempPath);
    webLog.print(" client=");
    webLog.println(request->client()->remoteIP().toString());

    if (!request->_tempFile) {
      webLog.print("Upload Error: cannot open ");
      webLog.println(tempPath);
      return;
    }
  }

  if (len != 0) {
    // After an unrecoverable write error the file is closed.  The HTTP stack
    // still delivers the remaining body chunks, so discard them silently.
    if (!request->_tempFile) {
      return;
    }

    // SD writes can occasionally return zero while another task or the card is
    // briefly busy.  Retry the same chunk for a few milliseconds.  No bytes
    // are skipped: a partial write resumes from data + written.
    size_t written = 0;
    uint8_t retry_count = 0;
    static const uint8_t max_write_retries = 8;
    while (written < len) {
      const size_t n = request->_tempFile.write(data + written, len - written);
      if (n != 0) {
        written += n;
        retry_count = 0;
        continue;
      }

      if (++retry_count > max_write_retries) {
        break;
      }
      delay(2);
      yield();
    }

    if (written != len) {
      webLog.print("Upload Error: SD write stalled file=");
      webLog.print(filename);
      webLog.print(" index=");
      webLog.print(index);
      webLog.print(" requested=");
      webLog.print(len);
      webLog.print(" written=");
      webLog.println(written);
      request->_tempFile.close();
      // Leave the incomplete .tmp in place for post-mortem inspection.  A
      // later upload of the same filename removes it before starting.
      return;
    }

    static const size_t flush_step = 256 * 1024;
    const size_t end_index = index + len;
    if (index / flush_step != end_index / flush_step) {
      request->_tempFile.flush();
    }

    static const size_t progress_step = 64 * 1024;
    if (index == 0 || index / progress_step != end_index / progress_step) {
      webLog.print("Upload Progress: ");
      webLog.print(filename);
      webLog.print(" received=");
      webLog.println(end_index);
    }
  }

  if (!final) return;

  const size_t receivedSize = index + len;
  if (!request->_tempFile) {
    webLog.print("Upload Error: incomplete file=");
    webLog.println(filename);
    return;
  }

  request->_tempFile.flush();
  request->_tempFile.close();

  // Reopen the temporary file after close/flush.  This verifies the size that
  // is actually recorded in the filesystem rather than trusting HTTP index.
  File verify = SD.open(tempPath, "r");
  if (!verify) {
    webLog.print("Upload Error: cannot reopen temp file ");
    webLog.println(tempPath);
    return;
  }
  const size_t storedSize = verify.size();
  verify.close();

  webLog.print("Upload Verify: ");
  webLog.print(filename);
  webLog.print(" received=");
  webLog.print(receivedSize);
  webLog.print(" stored=");
  webLog.println(storedSize);

  if (storedSize != receivedSize) {
    webLog.print("Upload Error: size mismatch file=");
    webLog.print(filename);
    webLog.print(" received=");
    webLog.print(receivedSize);
    webLog.print(" stored=");
    webLog.println(storedSize);
    // Do not replace the previous known-good target.
    return;
  }

  // Transactional replacement using only 8.3 filenames.  Keep the old target
  // as .bak until the verified .tmp has been promoted successfully.
  if (SD.exists(backupPath)) {
    SD.remove(backupPath);
  }

  bool hadOldTarget = SD.exists(targetPath);
  if (hadOldTarget && !SD.rename(targetPath, backupPath)) {
    webLog.print("Upload Error: cannot backup ");
    webLog.print(targetPath);
    webLog.print(" -> ");
    webLog.println(backupPath);
    return;
  }

  if (!SD.rename(tempPath, targetPath)) {
    webLog.print("Upload Error: cannot commit ");
    webLog.print(tempPath);
    webLog.print(" -> ");
    webLog.println(targetPath);
    if (hadOldTarget) {
      if (!SD.rename(backupPath, targetPath)) {
        webLog.print("Upload Error: rollback failed ");
        webLog.print(backupPath);
        webLog.print(" -> ");
        webLog.println(targetPath);
      }
    }
    return;
  }

  // Verify the committed name as a final guard.  If it somehow differs, put
  // the previous target back when possible.
  File committed = SD.open(targetPath, "r");
  const size_t committedSize = committed ? committed.size() : 0;
  if (committed) committed.close();

  if (committedSize != receivedSize) {
    webLog.print("Upload Error: committed size mismatch file=");
    webLog.print(filename);
    webLog.print(" expected=");
    webLog.print(receivedSize);
    webLog.print(" actual=");
    webLog.println(committedSize);

    SD.remove(targetPath);
    if (hadOldTarget) SD.rename(backupPath, targetPath);
    return;
  }

  if (hadOldTarget && SD.exists(backupPath)) {
    SD.remove(backupPath);
  }

  webLog.print("Upload Complete: ");
  webLog.print(filename);
  webLog.print(" received=");
  webLog.print(receivedSize);
  webLog.print(" stored=");
  webLog.println(committedSize);
}

String processor(const String& var) {
  if (var == "FREESPIFFS") {
    return humanReadableSize((SD.totalBytes() - SD.usedBytes()));
  }

  if (var == "USEDSPIFFS") {
    return humanReadableSize(SD.usedBytes());
  }

  if (var == "TOTALSPIFFS") {
    return humanReadableSize(SD.totalBytes());
  }

  if (var == "GRID_LOCATOR") {
    if (plogw!=NULL) {
      return String(plogw->grid_locator_set);
    } else {
      return String("");
    }
  }

  return String();
}


// Maidenhead Grid Locator → 緯度・経度 (中心点)
void gridToLatLon(const char* grid, double& lat, double& lon) {
  if (!grid || strlen(grid) < 4) {
    lat = 0;
    lon = 0;
    return;
  }
  lon = (grid[0] - 'A') * 20 - 180;
  lat = (grid[1] - 'A') * 10 - 90;

  lon += (grid[2] - '0') * 2;
  lat += (grid[3] - '0') * 1;

  if (strlen(grid) >= 6) {
    lon += (grid[4] - 'A') * (2.0 / 24.0);
    lat += (grid[5] - 'A') * (1.0 / 24.0);

    // 中心に合わせる
    lon += (2.0 / 24.0) / 2.0;
    lat += (1.0 / 24.0) / 2.0;
  } else {
    // 4文字グリッドなら中央に補正
    lon += 1.0;
    lat += 0.5;
  }
}


// ハバーサイン距離（km）
double haversine(double lat1, double lon1, double lat2, double lon2) {
  double R = 6371.0;
  double dLat = radians(lat2 - lat1);
  double dLon = radians(lon2 - lon1);
  lat1 = radians(lat1);
  lat2 = radians(lat2);
  double a = sin(dLat/2) * sin(dLat/2) +
             sin(dLon/2) * sin(dLon/2) * cos(lat1) * cos(lat2);
  double c = 2 * atan2(sqrt(a), sqrt(1-a));
  return R * c;
}

// 2地点間の初期方位（北=0°, 東=90°…）を度で返す
double calculateBearing(double lat1, double lon1, double lat2, double lon2) {
  double dLon = (lon2 - lon1) * DEG_TO_RAD;
  lat1 *= DEG_TO_RAD;
  lat2 *= DEG_TO_RAD;

  double y = sin(dLon) * cos(lat2);
  double x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dLon);
  double bearing = atan2(y, x) * RAD_TO_DEG;

  // 0～360度に正規化
  return fmod((bearing + 360.0), 360.0);
}


struct ParkInfo {
  double dist;   // km
  String code;   // JA-0001 …
  String name;
  double bearing;
  /* デフォルト (必須) */
  ParkInfo() : dist(1e9), code(""), name(""),bearing(0.0) {}

  /* 値付きコンストラクタ */
  ParkInfo(double d, const String& c, const String& n, double b)
    : dist(d), code(c), name(n),bearing(b) {}
};



// `/nearest?grid=PM95ru`
void setupNearestHandler(AsyncWebServer &server) {
  server.on("/nearest", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("grid")) {
      request->send(400, "text/plain", "Missing grid param");
      return;
    }

    String grid = request->getParam("grid")->value();
    grid.toUpperCase();  // 必須
    float myLat = 0, myLon = 0;
    //    gridToLatLon(grid.c_str(), myLat, myLon);

    char gridstr[10];
    strcpy(gridstr,grid.c_str());
    webLog.print("grid=");webLog.print(gridstr);
    webLog.print("<-");webLog.println(grid.c_str());
	      
    myLat=mh2lat(gridstr);
    myLon=mh2lon(gridstr);

    webLog.print("lat,lon=");webLog.print(myLat);webLog.print(" ");webLog.println(myLon);

    File f = SD.open("/pota-jp.csv", "r");
    if (!f) {
      request->send(500, "text/plain", "File open error");
      return;
    }

    ParkInfo top3[3];
    int found = 0;

    while (f.available()) {
      String line = f.readStringUntil('\n');

      if (line.startsWith("reference")) continue;

      int c1 = line.indexOf(',');
      int c2 = line.indexOf(',', c1 + 1);
      int c3 = line.indexOf(',', c2 + 1);
      int c4 = line.indexOf(',', c3 + 1);
      String code = line.substring(0, c1);
      String name = line.substring(c1 + 1, c2);
      double lat = line.substring(c2 + 1, c3).toFloat();
      double lon = line.substring(c3 + 1, c4).toFloat();
      /*      webLog.print("code:");webLog.print(code);
      webLog.print("name:");webLog.print(name);      
      webLog.print(" lat:");webLog.print(lat);
      webLog.print(" lon:");webLog.println(lon);
      */
      

      if (lat == 0 || lon == 0) continue;

      double dist = haversine(myLat, myLon, lat, lon);
      double bearing = calculateBearing(myLat, myLon, lat, lon);

      yield();
      // 上位3件に追加または更新
      if (found < 3) {
        top3[found++] = {dist, code, name, bearing };
      } else {
        // 一番遠いのを探して置き換え
        int maxIndex = 0;
        for (int i = 1; i < 3; ++i) {
          if (top3[i].dist > top3[maxIndex].dist) maxIndex = i;
        }
        if (dist < top3[maxIndex].dist) {
          top3[maxIndex] = {dist, code, name, bearing};
        }
      }
    }
    f.close();

    // ソート（昇順）
    for (int i = 0; i < found - 1; ++i) {
      for (int j = i + 1; j < found; ++j) {
        if (top3[i].dist > top3[j].dist) {
          ParkInfo temp = top3[i];
          top3[i] = top3[j];
          top3[j] = temp;
        }
      }
    }

    // JSON出力
    String result = "[";
    for (int i = 0; i < found; ++i) {
      if (i > 0) result += ",";
      result += "{\"code\":\"" + top3[i].code + "\",";
      result += "\"name\":\"" + top3[i].name + "\",";
      result += "\"distance_km\":" + String(top3[i].dist, 2) +",";
      result += "\"bearing_deg\":" + String(top3[i].bearing, 1) + "}";
    }
    result += "]";
    request->send(200, "application/json", result);
  });
}


struct SummitInfo {
  double dist;
  String code;   // JA/KN-001 …
  String name;
  int    alt;    // 標高 m
  double bearing;

  SummitInfo() : dist(1e9), code(""), name(""),  alt(0), bearing(0) {}
  SummitInfo(double d, const String& c, const String& n, int a,double b)
    : dist(d), code(c), name(n), alt(a),bearing(b) {}
};

void setupNearestSummit(AsyncWebServer &server) {
  server.on("/nearest_summit", HTTP_GET, [](AsyncWebServerRequest *req){
    if (!req->hasParam("grid")) { req->send(400,"text/plain","grid?"); return; }
    String grid = req->getParam("grid")->value();
    grid.toUpperCase();  // 必須
    float myLat=0,myLon=0;
    //gridToLatLon(grid.c_str(), myLat, myLon);
    char gridstr[10];
    strcpy(gridstr,grid.c_str());
    webLog.print("grid=");webLog.print(gridstr);
    webLog.print("<-");webLog.println(grid.c_str());
	      
    myLat=mh2lat(gridstr);
    myLon=mh2lon(gridstr);
    webLog.print("lat,lon=");webLog.print(myLat);webLog.print(" ");webLog.println(myLon);

    File f = SD.open("/ja_sota.csv","r");
    if (!f){ req->send(500,"text/plain","SOTA CSV open err"); return; }
    webLog.println("open ja_sota.csv");

    SummitInfo best[3]; int filled=0;
    while(f.available()){
      String line=f.readStringUntil('\n');
      if(line.startsWith("summitCode")) continue;
      //      webLog.println(line);
      int c1=line.indexOf(',');
      int c2=line.indexOf(',',c1+1);
      int c3=line.indexOf(',',c2+1);
      int c4=line.indexOf(',',c3+1);
      int c5=line.indexOf(',',c4+1);
      //JA/YN-082,Oomuroyama,35.44090,138.65359,1468
      String code=line.substring(0,c1);
      String name=line.substring(c1+1,c2);
      double lat= line.substring(c2+1,c3).toFloat();
      double lon= line.substring(c3+1,c4).toFloat();
      int alt   = line.substring(c4+1,c5).toInt();      
      if(lat==0||lon==0) continue;
      //      webLog.print("lat:");webLog.print(lat);
      //      webLog.print("lon:");webLog.println(lon);      
      yield();
      double d = haversine(myLat,myLon,lat,lon);
      double bearing = calculateBearing(myLat, myLon, lat, lon);

      if(filled<3){ best[filled++]={d,code,name,alt,bearing }; }
      else{
        int far=0;
	for(int i=1;i<3;i++) {
	  if(best[i].dist>best[far].dist) far=i;
	}
        if(d<best[far].dist) {
	  best[far]={d,code,name,alt,bearing };
	}
      }
    }
    f.close();
    // 距離で昇順ソート
    for(int i=0;i<filled-1;i++) for(int j=i+1;j<filled;j++)
      if(best[i].dist>best[j].dist){ SummitInfo t=best[i]; best[i]=best[j]; best[j]=t; }

    String json="[";
    for(int i=0;i<filled;i++){
      if(i) json+=",";
      json+="{\"code\":\""+best[i].code+"\",\"name\":\""+best[i].name+
            "\",\"alt\":"+String(best[i].alt)+
            ",\"distance_km\":"+String(best[i].dist,2)+
            ",\"bearing_deg\":"+String(best[i].bearing,1)+	
	"}";
    }
    json+="]";
    webLog.println(json);
    req->send(200,"application/json",json);
  });
}

//#include <AsyncTCP.h>
//#include <ESPAsyncWebServer.h>

// 必要な外部参照
//extern const char *pwin_index(int i);         // 編集する文字列
//extern const char *pwin_name_index(int i);    // 項目名

enum InputRestrict { Allowall, Callsign, Nospace };

static bool normalize_web_mdns_hostname(char *name)
{
  if (!name || !*name) return false;
  const size_t len = strlen(name);
  if (len == 0 || len > LEN_HOST_NAME) return false;
  if (name[0] == '-' || name[len - 1] == '-') return false;

  for (size_t i = 0; i < len; ++i) {
    unsigned char c = (unsigned char)name[i];
    if (c >= 'A' && c <= 'Z') {
      name[i] = (char)(c - 'A' + 'a');
      c = (unsigned char)name[i];
    }
    if (!((c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-'))
      return false;
  }
  return true;
}

InputRestrict pwin_type_index(int i) {
  switch (i) {
  case 0:return Callsign; // My Call
  case 1:return Nospace; // SentExch
  case 7:return Nospace; // Cluster Name
  case 8:return Allowall; // Cluster Cmd
  case 20:return Nospace; // Cluster2 Name
  case 21:return Allowall; // Cluster2 Cmd
  case 22:
  case 23:
  case 24:
  case 25:
  case 26:return Allowall; // Cluster2 startup commands
  case 5:return Allowall; // "Wifi_SSID" may legally contain spaces
  case 6:return Nospace; // "Wifi_Passwd";
  default : return Allowall;
  }
}  

const int N_EDITWIN=28;
const char *pwin_name_index(int i) {
  switch (i) {
  case 0:return "My Call";
  case 1:return "Sent Exch";
  case 2:return "Contest Name";
  case 3:return "Power Code";    
  case 4:return "JCC/JCG POTA/ SOTA/";
  case 5:return "Wifi_SSID";
  case 6:return "Wifi_Passwd";
  case 7:return "Cluster Name";        
  case 8:return "Cluster Cmd";    
  case 9:return "Sat Name";
  case 10:return "Grid Locator";    
  case 11:return "My Name";
  case 12:return "zServer Name";        
  case 13:return "CW Message 0 F1";
  case 14:return "CW Message 1 F2";
  case 15:return "CW Message 2 F3";
  case 16:return "CW Message 3 F4";
  case 17:return "CW Message 4 F5";
  case 18:return "CW Message 5 F6";
  case 19:return "CW Message 6 F7";        
  case 20:return "Cluster2 Name";
  case 21:return "Cluster2 Cmd";
  case 22:return "Cluster2 Startup Command 1";
  case 23:return "Cluster2 Startup Command 2";
  case 24:return "Cluster2 Startup Command 3";
  case 25:return "Cluster2 Startup Command 4";
  case 26:return "Cluster2 Startup Command 5";
  case 27:return "Hostname (mDNS)";
  default : return "--";
  }
  
}

char *pwin_index(int i) {
  switch (i) {
  case 0:return plogw->my_callsign;
  case 1:return plogw->sent_exch;
  case 2:return plogw->contest_name;   // "Contest Name";
  case 3:return plogw->power_code;// "Power Code";    
  case 4:return plogw->jcc;  // "JCC/JCG POTA/ SOTA/";
  case 5:return plogw->wifi_ssid; //"Wifi_SSID";
  case 6:return plogw->wifi_passwd; //"Wifi_Passwd";
  case 7:return plogw->cluster_name; //"Cluster Name";
  case 8:return plogw->cluster_cmd; //"Cluster Cmd";
  case 9:return plogw->sat_name;//"Sat Name";
  case 10:return plogw->grid_locator; //"Grid Locator";    
  case 11:return plogw->my_name; //"My Name";
  case 12:return plogw->zserver_name; //"zServer Name";        
  case 13:return plogw->cw_msg[0]; // cw_msg[0]
  case 14:return plogw->cw_msg[1]; // cw_msg[1]
  case 15:return plogw->cw_msg[2]; // cw_msg[2]
  case 16:return plogw->cw_msg[3]; // cw_msg[3]
  case 17:return plogw->cw_msg[4]; // cw_msg[4]
  case 18:return plogw->cw_msg[5]; // cw_msg[5]                    
  case 19:return plogw->cw_msg[6]; // cw_msg[6]                    
  case 20:return plogw->cluster2_name;
  case 21:return plogw->cluster2_cmd;
  case 22:return cluster2_startup_cmd[0];
  case 23:return cluster2_startup_cmd[1];
  case 24:return cluster2_startup_cmd[2];
  case 25:return cluster2_startup_cmd[3];
  case 26:return cluster2_startup_cmd[4];
  case 27:return plogw->hostname;
  default:return NULL;
  }
}



const char *settings_page_html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>DVPlogger Settings</title>
  <style>
    body { font-family: sans-serif; margin: 20px; }
    input[type="text"] { width: min(60%, 42em); padding: 8px; margin: 5px 0; font-size:16px; }
    button { min-height: 42px; margin: 4px; }
    label { display: block; margin-top: 10px; font-weight: bold; }
    .setting { margin-bottom: 15px; }
    #status { min-height: 1.4em; font-weight: bold; }
    .saved { color: #176b2c; }
    .dirty { color: #9a6700; }
    .error { color: #b42318; }
  </style>
</head>
<body>
  <h2>DVPlogger Settings</h2>
<button onclick="fetch('/save_settings').then(() => alert('Settings saved'));">Save</button>
<button onclick="fetch('/load_settings').then(() => {
  alert('Settings loaded');
  location.reload();  // ページを再読み込み
});">Load</button>

  <div class="setting">
    <label>Clock Display</label>
    <label style="display:inline; font-weight:normal;"><input type="radio" name="clockMode" value="0"> JST</label>
    <label style="display:inline; font-weight:normal; margin-left:16px;"><input type="radio" name="clockMode" value="1"> UTC</label>
  </div>

  <div class="setting">
    <label for="bandmap_lifetime">Bandmap spot lifetime (minutes)</label>
    <input id="bandmap_lifetime" type="number" min="1" max="1440"
           value="%BANDMAP_LIFETIME%" style="width:10em">
    <button type="button" onclick="updateBandmapLifetime()">Apply and save</button>
    <div>Equivalent keyboard command: <code>OLDEST10</code> for 10 minutes.</div>
  </div>

  <form id="settingsForm">
    %SETTINGS_INPUTS%
  </form>
  <p id="status"></p>
<script>
function updateBandmapLifetime() {
  const input = document.getElementById('bandmap_lifetime');
  const value = encodeURIComponent(input.value);
  fetch(`/set_bandmap_lifetime?minutes=${value}`)
    .then(res => res.text().then(text => ({ok: res.ok, text})))
    .then(result => {
      const status = document.getElementById("status");
      status.innerText = result.text;
      status.className = result.ok ? "saved" : "error";
    });
}

function updateSetting(index) {
  const input = document.getElementById('edit_' + index);
  const value = encodeURIComponent(input.value);
  fetch(`/set_edit?index=${index}&value=${value}`)
    .then(res => res.text())
    .then(msg => {
      document.getElementById("status").innerText = msg;
    });
}

document.addEventListener("DOMContentLoaded", () => {
  fetch('/clock_display_mode')
    .then(res => res.text())
    .then(mode => {
      const item = document.querySelector(`input[name="clockMode"][value="${mode.trim()}"]`);
      if (item) item.checked = true;
    });
  document.querySelectorAll('input[name="clockMode"]').forEach(item => {
    item.addEventListener('change', function() {
      fetch(`/set_clock_display_mode?mode=${this.value}`)
        .then(res => res.text())
        .then(msg => { document.getElementById('status').innerText = msg; });
    });
  });
  const inputs = document.querySelectorAll("input[type=text]");
  inputs.forEach(input => {
    input.addEventListener("keydown", function(event) {
      if (event.key === "Enter") {
        event.preventDefault();
        const index = this.dataset.index;
        updateSetting(index);
      }
    });
  });
});
</script>
</body>
</html>
)rawliteral";


static const char rigs_page_header[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>DVPlogger RIG Settings</title>
  <style>
    body { font-family: sans-serif; margin: 20px; }
    input[type="text"] { width: min(70%, 48em); padding: 8px; margin: 5px 0; font-size:16px; }
    button { min-height: 42px; margin: 4px; }
    label { display: block; margin-top: 10px; font-weight: bold; }
    .setting { margin-bottom: 15px; }
  </style>
</head>
<body>
  <h2>DVPlogger RIG Settings</h2>
<p>Separated args by ,(comma)</p>
<ul>
  <li><strong>CW:<em>port</em></strong> CW key output:
    0=LED/GPIO, 1=KEY1, 2=KEY2, 3=USB DTR, 4=USB RTS.
    For USB rigs, select the line matching the rig's PC KEYING setting.</li>
  <li><strong>FSK:<em>port</em></strong> RTTY FSK key output:
    0=LED/GPIO, 1=KEY1, 2=KEY2, 3=USB DTR, 4=USB RTS.
    CW and FSK are independent. If omitted, FSK uses the CW port for backward compatibility.</li>
  <li><strong>RP:<em>0|1</em></strong> per-rig RTTY polarity:
    0=normal, 1=reverse. If omitted, the legacy/global <code>rttyinvert</code> setting is used.</li>
  <li><strong>PTT:<em>0..4</em></strong> additional PTT method:
    0/1=don't care (none), 2=CAT/CI-V, 3=USB DTR, 4=USB RTS.<br>
    Hardware MIC/PTT is independent of this setting:
    Radio0&rarr;PTT1, Radio1&rarr;PTT2, Radio2&rarr;PTT3.
    Therefore <code>PTT:2</code> means hardware PTT plus CAT/CI-V PTT.</li>
  <li><strong>B:<em>baudrate</em></strong></li>
  <li><strong>P:<em>catport_number</em></strong> ((-2:Manual) ‑1:USB, 1:Bluetooth, 2:CI‑V, 3:CAT, 4:CAT2)</li>
  <li><strong>ADR:<em>CI‑V_address</em></strong></li>
  <li><strong>NAME:<em>rig_name</em></strong></li>
  <li><strong>XVTR:<em>transverter_frequencies</em></strong> Frequencies joined by `_` from index 0: IFlo_0 IFhi_0 RFlo_0 RFhi_0 IFlo_1 …</li>
  <li><strong>R:<em>CAT_reverse_polarity</em></strong> (0/1)</li>
  <li><strong>BM:<em>band_mask</em></strong> in HEX (0 = enabled, 1 = disabled)<br>
    <strong>Typical values:</strong><br>
    0xFE00 &nbsp; Binary: 1111 1110 0000 0000 &nbsp; = 1.8/3.5/7/14/21/28/50/144/430 MHz (WARC excluded)<br>
    0xFFC0 &nbsp; Binary: 1111 1111 1100 0000 &nbsp; = 1.8/3.5/7/14/21/28 MHz (WARC excluded)<br>
    0xF07F &nbsp; Binary: 1111 0000 0111 1111 &nbsp; = 144/430/1200/2400/5600 MHz
  </li>
  <li><strong>TP:<em>cat_type</em>_<em>rig_type</em></strong><br>
    <strong>cat_type</strong> (CAT protocol/transport handler):<br>
    0 ICOM CI-V, 1 Yaesu New CAT, 2 Kenwood CAT, 3 Manual (No CAT),
    4 Yaesu Old CAT, 5 Elecraft KX, 6 Yaesu FT-817/818, 7 QRP Labs QMX USB CAT,
    8 ATS Mini USB remote protocol<br>
    <strong>rig_type</strong> (model-specific behavior):<br>
    0 IC-705, 1 IC-9700, 2 Yaesu, 3 Kenwood, 4 Manual,
    5 IC-7300, 6 Elecraft KX, 7 Xiegu X6100, 8 QRP Labs QMX, 9 ATS Mini<br>
    <strong>Typical combinations:</strong>
    IC-705 = TP:0_0, IC-9700 = TP:0_1, IC-7300 = TP:0_5,
    Yaesu New = TP:1_2, Yaesu Old = TP:4_2, FT-817/818 = TP:6_2,
    Kenwood = TP:2_3, Elecraft KX = TP:5_6, X6100 = TP:0_7,
    Manual = TP:3_4, QMX = TP:7_8, ATS Mini = TP:8_9
  </li>
</ul>
<h3>CW / RTTY example</h3>
<p><code>CW:4,FSK:3,PTT:2,RP:0</code> means CW=USB RTS, RTTY FSK=USB DTR,
RADIO-associated hardware PTT + CAT/CI-V PTT, and normal RTTY polarity.</p>
<p>If MARK/SPACE polarity is reversed, change <code>RP:0</code> to <code>RP:1</code>.</p>
<p>Press Enter or tap Apply beside an input box to reflect changes.</p>
<p><a href="/" >go back to Home</a></p>
<button type="button" onclick="saveRigs();">Save RIGs</button>
<button type="button" onclick="loadRigs();">Load RIGs</button>
  <form id="settingsForm">
)rawliteral";

static const char rigs_page_footer[] PROGMEM = R"rawliteral(
  </form>
  <p id="status"></p>
<script>
let rigsDirty = false;
let rigUpdateInProgress = false;

function setRigStatus(message, state) {
  const status = document.getElementById("status");
  status.textContent = message;
  status.className = state || "";
}

async function updateBandmapLifetime() {
  const input = document.getElementById('bandmap_lifetime');
  const value = encodeURIComponent(input.value);
  fetch(`/set_bandmap_lifetime?minutes=${value}`)
    .then(res => res.text().then(text => ({ok: res.ok, text})))
    .then(result => {
      const status = document.getElementById("status");
      status.innerText = result.text;
      status.className = result.ok ? "saved" : "error";
    });
}

async function updateSetting(index) {
  const input = document.getElementById('edit_' + index);
  if (!input || rigUpdateInProgress) return;

  const requested = input.value;
  rigUpdateInProgress = true;
  input.disabled = true;
  try {
    const params = new URLSearchParams({
      index: String(index),
      value: requested
    });
    const res = await fetch(`/rig_edit?${params.toString()}`, { cache: 'no-store' });
    const canonical = await res.text();
    if (!res.ok) throw new Error(canonical || `HTTP ${res.status}`);

    // rig_spec[] is the single source of truth.  Always show the string
    // regenerated from the RAM structure, never a browser-only edit.
    input.value = canonical;
    input.dataset.canonical = canonical;
    rigsDirty = true;
    setRigStatus(`RIG ${index} updated in RAM (not saved yet).`, "dirty");
  } catch (error) {
    // Keep the browser field consistent with the unchanged RAM setting when
    // the update is rejected (for example, a duplicate NAME).
    if (input.dataset.canonical !== undefined) {
      input.value = input.dataset.canonical;
    }
    setRigStatus(`Update failed: ${error.message}`, "error");
  } finally {
    rigUpdateInProgress = false;
    input.disabled = false;
    input.focus();
    input.select();
  }
}

async function saveRigs() {
  if (rigUpdateInProgress) {
    setRigStatus("RIG update is still in progress. Press Save again after it completes.", "error");
    return;
  }
  try {
    const res = await fetch('/save_rigs', { cache: 'no-store' });
    const msg = await res.text();
    if (!res.ok) throw new Error(msg || `HTTP ${res.status}`);
    rigsDirty = false;
    setRigStatus(msg, "saved");
  } catch (error) {
    setRigStatus(`Save failed: ${error.message}`, "error");
  }
}

async function loadRigs() {
  try {
    const res = await fetch('/load_rigs', { cache: 'no-store' });
    const msg = await res.text();
    if (!res.ok) throw new Error(msg || `HTTP ${res.status}`);
    rigsDirty = false;
    setRigStatus(msg, "saved");
    setTimeout(() => location.reload(), 200);
  } catch (error) {
    setRigStatus(`Load failed: ${error.message}`, "error");
  }
}

window.addEventListener('beforeunload', event => {
  if (!rigsDirty) return;
  event.preventDefault();
  event.returnValue = '';
});
document.addEventListener("DOMContentLoaded", () => {
  const inputs = document.querySelectorAll("input[type=text]");
  inputs.forEach(input => {
    if (input.id && input.id.startsWith("edit_")) {
      input.dataset.canonical = input.value;
    }
    input.addEventListener("keydown", function(event) {
      if (event.key === "Enter") {
        event.preventDefault();
        const index = this.dataset.index;
        updateSetting(index);
      }
    });
  });
});
</script>
</body>
</html>
)rawliteral";

const char *example_input_html = R"rawliteral(
<div class="setting">
  <label for="edit_%d">%s</label>
  <input type="text" id="edit_%d" data-index="%d" value="%s" maxlength="%d"
    enterkeyhint="done" autocomplete="off" spellcheck="false" %s>
  <button type="button" onclick="updateSetting(%d)">Apply</button>
</div>
)rawliteral";


const char *pattern_upper = "oninput=\"this.value = this.value.toUpperCase()\"";
const char *pattern_no_space = "oninput=\"this.value = this.value.replace(/[ \\t]/g,'')\"";



// また組み合わせパターン
const char *pattern_both = 
  "oninput=\"this.value = this.value.toUpperCase().replace(/[^A-Z0-9\\/]/g,'')\"";


// settings_page_htmlとexample_input_html は前述の定数文字列

static const char contests_page_header[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DVPlogger コンテスト設定</title>
<style>
body{font-family:sans-serif;margin:18px;max-width:1250px}
table{border-collapse:collapse;width:100%;margin:12px 0 24px}
th,td{border:1px solid #aaa;padding:6px;text-align:left;vertical-align:middle}
tr.current{font-weight:bold;background:#e8f3ff}
input{box-sizing:border-box;padding:5px;font-size:.95em;width:100%;min-width:9em}
button{padding:5px 10px;white-space:nowrap}.dupe-ok{color:#075f16;font-weight:bold}.dupe-ng{color:#9b1c1c;font-weight:bold}
#status{min-height:1.4em;font-weight:bold}.note{font-size:.9em}.name{white-space:nowrap}
.contest-wrap{overflow-x:auto;width:100%}.contest-table{table-layout:fixed;min-width:1180px}
.contest-table .col-id{width:38px}.contest-table .col-name{width:220px}.contest-table .col-dupe{width:80px}
.contest-table .col-f1{width:175px}.contest-table .col-f2,.contest-table .col-f3{width:135px}
.contest-table .col-f5{width:160px}.contest-table .col-exch{width:145px}
.contest-name-field{display:flex;align-items:center;gap:8px;white-space:nowrap}.contest-name-field .name-text{flex:1;min-width:0}
.nav-links{display:flex;gap:1rem;align-items:center;margin:.2rem 0 1rem}.nav-links a{white-space:nowrap}
.user-table{table-layout:fixed;min-width:1200px}
.user-table .col-user-name{width:220px}.user-table .col-msg{width:175px}
.user-table .col-exch{width:150px}.user-table .col-dupe{width:110px}
.user-name-field{display:flex;align-items:center;gap:4px;white-space:nowrap}
.user-name-field input{min-width:0;flex:1}
.guide{max-width:1000px;border:1px solid #bbb;border-radius:8px;padding:12px 16px;margin:12px 0;background:#fafafa}.guide h3{margin:.4rem 0}.guide ul{margin:.5rem 0;padding-left:1.4rem}.warn{background:#fff4d6;border-left:5px solid #d29a00;padding:8px 12px;margin:10px 0}.help{max-width:900px}.help th:first-child,.help td:first-child{white-space:nowrap}.examples code{white-space:nowrap}
</style></head><body><h2>コンテスト設定</h2>
<nav class="nav-links"><a href="/">ホーム</a><a href="/op">運用画面</a><a href="/bandmap">バンドマップ</a><a href="/contests?lang=en">English</a></nav>
<p>現在選択中: <strong>%CURRENT_CONTEST%</strong></p>
<p class="note"><strong>%SD_STATUS%</strong><br>直前の処理: %LAST_STATUS%</p><p id="status"></p>
<div class="guide"><h3>このページの使い方</h3>
<ul><li>参加するコンテストの行で、必要に応じてCWメッセージと送出ナンバーを編集します。</li>
<li>コンテスト名の右にある「選択して保存」を押すと、その行の設定をSDカードの <code>/CONTEST.TXT</code> に保存し、直ちにそのコンテストへ切り替えます。</li>
<li>コンテストを切り替えると、選択したコンテスト名に対応する過去QSOだけを使ってデュープ・マルチと連番を再構築します。複数コンテストを往復して運用できます。</li>
<li><strong>Ctrl-2</strong>：登録されているコンテストを順番に切り替えます。</li>
<li><strong>Ctrl-Shift-2</strong>：現在のコンテストと直前に使用していたコンテストを交互に切り替えます。2つのコンテストを並行して運用するときに便利です。</li></ul>
<p class="note"><strong>Web画面とキーボードの設定は共通です。</strong>どちらから切り替えても、そのコンテスト用のCWメッセージ、送出ナンバー、連番が復元され、デュープ・マルチ情報が再構築されます。</p>
<div class="warn"><strong>コンテスト内／外の区別：</strong>通常QSOをコンテスト集計から除外したいときは、端末のコール欄に <code>OFFCONTEST</code> と入力します。再び現在のコンテストへ戻すときは <code>ONCONTEST</code> と入力します。OFFCONTEST中のQSOはRemarksに区別情報が記録され、コンテスト別のデュープ集計から除外されます。</div>
<p class="note"><strong>選び方の目安：</strong><code>NOMULTI</code> はマルチを使用しない一般QSO向けです。交換ナンバー形式が近い既定コンテストを選ぶか、一覧にない場合は下の「ユーザー定義コンテスト」を使用してください。</p></div>
<div class="contest-wrap"><table class="contest-table"><colgroup>
<col class="col-id"><col class="col-name"><col class="col-dupe">
<col class="col-f1"><col class="col-f2"><col class="col-f3"><col class="col-f5">
<col class="col-exch"></colgroup>
<thead><tr><th>ID</th><th>コンテスト／選択</th><th>デュープ規則</th><th>CW F1（CQ）</th><th>CW F2</th><th>CW F3</th><th>CW F5</th><th>送出ナンバー</th></tr></thead><tbody>
)rawliteral";

static const char contests_page_footer[] PROGMEM = R"rawliteral(
</tbody></table></div><h3>ユーザー定義コンテスト（.MD）</h3>
<div class="guide"><p>CTESTWIN等のMD定義ファイルを本体トップページのFile Uploadから、8.3形式の <code>FILENAME.MD</code> としてSDカードへアップロードして使用します。</p>
<ul><li>ファイル名欄には <code>User</code> と <code>.MD</code> を除いた部分だけを入力します。例：<code>TOKYO.MD</code> なら <code>TOKYO</code>。</li>
<li>2行は独立した切替枠です。別々のMDファイルとCWメッセージを保存し、ボタン一つで切り替えられます。</li>
<li>MDファイルが見つからない場合でも、マルチなしのコンテストとして開始できます。デュープ規則は左のチェック欄で指定します。</li></ul></div>
<div class="contest-wrap"><table class="user-table"><colgroup>
<col class="col-user-name"><col class="col-dupe"><col class="col-msg"><col class="col-msg"><col class="col-msg"><col class="col-msg"><col class="col-exch">
</colgroup><thead><tr><th>MDファイル名／選択</th><th>CW／Phoneデュープ</th><th>CW F1（CQ）</th><th>CW F2</th><th>CW F3</th><th>CW F5</th><th>送出ナンバー</th></tr></thead><tbody>
<tr%USER1_CLASS%>
<td><form id="user_contest_form_1" method="GET" action="/select_user_contest"><input type="hidden" name="lang" value="%LANG%"><input type="hidden" name="slot" value="0"></form><div class="user-name-field"><span>User</span><input form="user_contest_form_1" name="filename" maxlength="8" value="%USER1_FILENAME%" placeholder="PRESET1" oninput="this.value=this.value.toUpperCase().replace(/[^A-Z0-9_-]/g,'')"><button form="user_contest_form_1" type="submit">%USER1_ACTION%</button></div></td>
<td><label><input form="user_contest_form_1" type="checkbox" name="dupe_separate" value="1" %USER1_DUPE_CHECKED% style="width:auto;min-width:0"> CWとPhoneを別交信として許可</label></td>
<td><input form="user_contest_form_1" name="f1" maxlength="30" value="%USER1_F1%"></td><td><input form="user_contest_form_1" name="f2" maxlength="30" value="%USER1_F2%"></td><td><input form="user_contest_form_1" name="f3" maxlength="30" value="%USER1_F3%"></td><td><input form="user_contest_form_1" name="f5" maxlength="30" value="%USER1_F5%"></td><td><input form="user_contest_form_1" name="exch" maxlength="17" value="%USER1_EXCH%"></td></tr>
<tr%USER2_CLASS%>
<td><form id="user_contest_form_2" method="GET" action="/select_user_contest"><input type="hidden" name="lang" value="%LANG%"><input type="hidden" name="slot" value="1"></form><div class="user-name-field"><span>User</span><input form="user_contest_form_2" name="filename" maxlength="8" value="%USER2_FILENAME%" placeholder="PRESET2" oninput="this.value=this.value.toUpperCase().replace(/[^A-Z0-9_-]/g,'')"><button form="user_contest_form_2" type="submit">%USER2_ACTION%</button></div></td>
<td><label><input form="user_contest_form_2" type="checkbox" name="dupe_separate" value="1" %USER2_DUPE_CHECKED% style="width:auto;min-width:0"> CWとPhoneを別交信として許可</label></td>
<td><input form="user_contest_form_2" name="f1" maxlength="30" value="%USER2_F1%"></td><td><input form="user_contest_form_2" name="f2" maxlength="30" value="%USER2_F2%"></td><td><input form="user_contest_form_2" name="f3" maxlength="30" value="%USER2_F3%"></td><td><input form="user_contest_form_2" name="f5" maxlength="30" value="%USER2_F5%"></td><td><input form="user_contest_form_2" name="exch" maxlength="17" value="%USER2_EXCH%"></td></tr>
</tbody></table></div>
<p class="note"><code>/FILENAME.MD</code> がない場合はマルチチェックなしで開始しますが、デュープチェックは有効です。ファイル名に使用できる文字は A-Z、0-9、_、- です。</p>

<h3>CWメッセージのマクロ</h3>
<p class="note"><strong>送出ナンバー</strong>欄には、実際に送る値（例：<code>11</code>、<code>1115</code>）を入力します。CWメッセージ中の <code>$W</code> がこの値へ展開されます。数字の短縮送信はDVPlogger本体のCW数字短縮設定に従います。</p>
<table class="help"><thead><tr><th>マクロ</th><th>展開内容</th></tr></thead><tbody>
<tr><td><code>$I</code></td><td>自局コールサイン</td></tr>
<tr><td><code>$C</code></td><td>相手局コールサイン</td></tr>
<tr><td><code>$U</code></td><td>CW／Digital時の <code>CQ</code></td></tr>
<tr><td><code>$T</code></td><td>CW／Digital時の <code>TEST</code></td></tr>
<tr><td><code>$A</code></td><td>CW／Digital時の <code>TU</code></td></tr>
<tr><td><code>$V</code></td><td>送信RST。CWでは通常 <code>5NN</code> に短縮</td></tr>
<tr><td><code>$W</code></td><td>この表の「送出ナンバー」欄</td></tr>
<tr><td><code>$P</code></td><td>バンドごとの電力コード</td></tr>
<tr><td><code>$J</code></td><td>本体設定の自局JCC／JCG番号</td></tr>
<tr><td><code>$S</code></td><td>現在の通しQSO番号</td></tr>
<tr><td><code>$Q</code></td><td>バンド別の次の連番（3桁）</td></tr>
<tr><td><code>$N</code></td><td>オペレータ名</td></tr>
</tbody></table>

<h3>設定例</h3>
<table class="help examples"><thead><tr><th>キー</th><th>入力例</th><th>送信例</th></tr></thead><tbody>
<tr><td>F1</td><td><code>$U $T DE $I $I $T</code></td><td><code>CQ TEST DE JK1DVP JK1DVP TEST</code></td></tr>
<tr><td>F2</td><td><code>$C $V $W</code></td><td><code>JA1ABC 5NN 1115</code></td></tr>
<tr><td>F3</td><td><code>$A $I $T</code></td><td><code>TU JK1DVP TEST</code></td></tr>
<tr><td>F5</td><td><code>$C $V$W$P</code></td><td><code>JA1ABC 5NN1115M</code></td></tr>
</tbody></table>
<p class="note">メッセージ中の空白は語間として送信されます。<code>$V$W$P</code> のように、マクロを空白なしで連結することもできます。</p>
</body></html>
)rawliteral";


static const char contests_page_header_en[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DVPlogger Contest Settings</title>
<style>
body{font-family:sans-serif;margin:18px;max-width:1250px}table{border-collapse:collapse;width:100%;margin:12px 0 24px}th,td{border:1px solid #aaa;padding:6px;text-align:left;vertical-align:middle}tr.current{font-weight:bold;background:#e8f3ff}input{box-sizing:border-box;padding:5px;font-size:.95em;width:100%;min-width:9em}button{padding:5px 10px;white-space:nowrap}.dupe-ok{color:#075f16;font-weight:bold}.dupe-ng{color:#9b1c1c;font-weight:bold}#status{min-height:1.4em;font-weight:bold}.note{font-size:.9em}.name{white-space:nowrap}.contest-wrap{overflow-x:auto;width:100%}.contest-table{table-layout:fixed;min-width:1180px}.contest-table .col-id{width:38px}.contest-table .col-name{width:220px}.contest-table .col-dupe{width:80px}.contest-table .col-f1{width:175px}.contest-table .col-f2,.contest-table .col-f3{width:135px}.contest-table .col-f5{width:160px}.contest-table .col-exch{width:145px}.contest-name-field{display:flex;align-items:center;gap:8px;white-space:nowrap}.contest-name-field .name-text{flex:1;min-width:0}.nav-links{display:flex;gap:1rem;align-items:center;margin:.2rem 0 1rem}.nav-links a{white-space:nowrap}.user-table{table-layout:fixed;min-width:1200px}.user-table .col-user-name{width:220px}.user-table .col-msg{width:175px}.user-table .col-exch{width:150px}.user-table .col-dupe{width:110px}.user-name-field{display:flex;align-items:center;gap:4px;white-space:nowrap}.user-name-field input{min-width:0;flex:1}.guide{max-width:1000px;border:1px solid #bbb;border-radius:8px;padding:12px 16px;margin:12px 0;background:#fafafa}.guide h3{margin:.4rem 0}.guide ul{margin:.5rem 0;padding-left:1.4rem}.warn{background:#fff4d6;border-left:5px solid #d29a00;padding:8px 12px;margin:10px 0}.help{max-width:900px}.help th:first-child,.help td:first-child{white-space:nowrap}.examples code{white-space:nowrap}
</style></head><body><h2>Contest Settings</h2>
<nav class="nav-links"><a href="/">Home</a><a href="/op">Operation</a><a href="/bandmap">Band map</a><a href="/contests?lang=ja">日本語</a></nav>
<p>Current contest: <strong>%CURRENT_CONTEST%</strong></p>
<p class="note"><strong>%SD_STATUS%</strong><br>Last action: %LAST_STATUS%</p><p id="status"></p>
<div class="guide"><h3>How to use this page</h3><ul>
<li>Edit the CW messages and sent exchange in the desired contest row when necessary.</li>
<li>Press “Select &amp; Save” to save the row to <code>/CONTEST.TXT</code> on the SD card and immediately switch contests.</li>
<li>When switching, DVPlogger rebuilds dupe, multiplier and serial-number information from QSOs tagged with that contest name. You can move back and forth between several contests.</li>
<li><strong>Ctrl-2</strong>: cycle through registered contests.</li>
<li><strong>Ctrl-Shift-2</strong>: toggle between the current and previously used contest; useful for operating two contests in parallel.</li></ul>
<p class="note"><strong>Web and keyboard settings are shared.</strong> Either method restores the contest-specific CW messages, sent exchange and serial number, then rebuilds dupe and multiplier information.</p>
<div class="warn"><strong>Contest / non-contest QSOs:</strong> Enter <code>OFFCONTEST</code> in the callsign field to exclude ordinary QSOs from contest scoring. Enter <code>ONCONTEST</code> to return to the active contest. OFFCONTEST QSOs are marked in Remarks and excluded from contest-specific dupe counting.</div>
<p class="note"><strong>Selection hint:</strong> <code>NOMULTI</code> is suitable for ordinary QSOs without multipliers. Select a built-in contest with a similar exchange, or use a User-defined contest below.</p></div>
<div class="contest-wrap"><table class="contest-table"><colgroup><col class="col-id"><col class="col-name"><col class="col-dupe"><col class="col-f1"><col class="col-f2"><col class="col-f3"><col class="col-f5"><col class="col-exch"></colgroup>
<thead><tr><th>ID</th><th>Contest / Select</th><th>Dupe rule</th><th>CW F1 (CQ)</th><th>CW F2</th><th>CW F3</th><th>CW F5</th><th>Sent exchange</th></tr></thead><tbody>
)rawliteral";

static const char contests_page_footer_en[] PROGMEM = R"rawliteral(
</tbody></table></div><h3>User-defined contests (.MD)</h3>
<div class="guide"><p>Upload a CTESTWIN-compatible MD definition file from File Upload on the home page. Store it on the SD card as an 8.3 filename such as <code>FILENAME.MD</code>.</p><ul>
<li>Enter only the part between <code>User</code> and <code>.MD</code>. Example: enter <code>TOKYO</code> for <code>TOKYO.MD</code>.</li>
<li>The two rows are independent presets. Each can store a different MD file and CW messages for one-button switching.</li>
<li>If the MD file is missing, DVPlogger can still start the contest without multiplier checking. Set the dupe rule using the checkbox.</li></ul></div>
<div class="contest-wrap"><table class="user-table"><colgroup><col class="col-user-name"><col class="col-dupe"><col class="col-msg"><col class="col-msg"><col class="col-msg"><col class="col-msg"><col class="col-exch"></colgroup>
<thead><tr><th>MD filename / Select</th><th>CW/Phone dupe</th><th>CW F1 (CQ)</th><th>CW F2</th><th>CW F3</th><th>CW F5</th><th>Sent exchange</th></tr></thead><tbody>
<tr%USER1_CLASS%><td><form id="user_contest_form_1" method="GET" action="/select_user_contest"><input type="hidden" name="lang" value="%LANG%"><input type="hidden" name="slot" value="0"></form><div class="user-name-field"><span>User</span><input form="user_contest_form_1" name="filename" maxlength="8" value="%USER1_FILENAME%" placeholder="PRESET1" oninput="this.value=this.value.toUpperCase().replace(/[^A-Z0-9_-]/g,'')"><button form="user_contest_form_1" type="submit">%USER1_ACTION%</button></div></td><td><label><input form="user_contest_form_1" type="checkbox" name="dupe_separate" value="1" %USER1_DUPE_CHECKED% style="width:auto;min-width:0"> Allow separate CW and Phone QSOs</label></td><td><input form="user_contest_form_1" name="f1" maxlength="30" value="%USER1_F1%"></td><td><input form="user_contest_form_1" name="f2" maxlength="30" value="%USER1_F2%"></td><td><input form="user_contest_form_1" name="f3" maxlength="30" value="%USER1_F3%"></td><td><input form="user_contest_form_1" name="f5" maxlength="30" value="%USER1_F5%"></td><td><input form="user_contest_form_1" name="exch" maxlength="17" value="%USER1_EXCH%"></td></tr>
<tr%USER2_CLASS%><td><form id="user_contest_form_2" method="GET" action="/select_user_contest"><input type="hidden" name="lang" value="%LANG%"><input type="hidden" name="slot" value="1"></form><div class="user-name-field"><span>User</span><input form="user_contest_form_2" name="filename" maxlength="8" value="%USER2_FILENAME%" placeholder="PRESET2" oninput="this.value=this.value.toUpperCase().replace(/[^A-Z0-9_-]/g,'')"><button form="user_contest_form_2" type="submit">%USER2_ACTION%</button></div></td><td><label><input form="user_contest_form_2" type="checkbox" name="dupe_separate" value="1" %USER2_DUPE_CHECKED% style="width:auto;min-width:0"> Allow separate CW and Phone QSOs</label></td><td><input form="user_contest_form_2" name="f1" maxlength="30" value="%USER2_F1%"></td><td><input form="user_contest_form_2" name="f2" maxlength="30" value="%USER2_F2%"></td><td><input form="user_contest_form_2" name="f3" maxlength="30" value="%USER2_F3%"></td><td><input form="user_contest_form_2" name="f5" maxlength="30" value="%USER2_F5%"></td><td><input form="user_contest_form_2" name="exch" maxlength="17" value="%USER2_EXCH%"></td></tr></tbody></table></div>
<p class="note">If <code>/FILENAME.MD</code> is not found, the contest starts without multiplier checking, while dupe checking remains enabled. Valid filename characters are A-Z, 0-9, _ and -.</p>
<h3>CW message macros</h3><p class="note">Enter the value actually sent (for example <code>11</code> or <code>1115</code>) in “Sent exchange”. <code>$W</code> expands to this value. Abbreviated CW numerals follow the DVPlogger CW-number setting.</p>
<table class="help"><thead><tr><th>Macro</th><th>Expansion</th></tr></thead><tbody>
<tr><td><code>$I</code></td><td>Your callsign</td></tr><tr><td><code>$C</code></td><td>Other station's callsign</td></tr><tr><td><code>$U</code></td><td><code>CQ</code> in CW/Digital modes</td></tr><tr><td><code>$T</code></td><td><code>TEST</code> in CW/Digital modes</td></tr><tr><td><code>$A</code></td><td><code>TU</code> in CW/Digital modes</td></tr><tr><td><code>$V</code></td><td>Sent RST; normally shortened to <code>5NN</code> in CW</td></tr><tr><td><code>$W</code></td><td>Sent exchange in this table</td></tr><tr><td><code>$P</code></td><td>Band-specific power code</td></tr><tr><td><code>$J</code></td><td>Your JCC/JCG number from settings</td></tr><tr><td><code>$S</code></td><td>Current overall QSO serial number</td></tr><tr><td><code>$Q</code></td><td>Next band-specific serial number (3 digits)</td></tr><tr><td><code>$N</code></td><td>Operator name</td></tr></tbody></table>
<h3>Examples</h3><table class="help examples"><thead><tr><th>Key</th><th>Input</th><th>Sent text</th></tr></thead><tbody><tr><td>F1</td><td><code>$U $T DE $I $I $T</code></td><td><code>CQ TEST DE JK1DVP JK1DVP TEST</code></td></tr><tr><td>F2</td><td><code>$C $V $W</code></td><td><code>JA1ABC 5NN 1115</code></td></tr><tr><td>F3</td><td><code>$A $I $T</code></td><td><code>TU JK1DVP TEST</code></td></tr><tr><td>F5</td><td><code>$C $V$W$P</code></td><td><code>JA1ABC 5NN1115M</code></td></tr></tbody></table>
<p class="note">Spaces in a message are sent as word spaces. Macros may also be concatenated without spaces, as in <code>$V$W$P</code>.</p></body></html>
)rawliteral";

struct ContestWebPreset {
  bool used;
  char name[LEN_CONTEST_NAME + 1];
  char f1[LEN_CWMSG_WINDOW + 1];
  char f2[LEN_CWMSG_WINDOW + 1];
  char f3[LEN_CWMSG_WINDOW + 1];
  char f5[LEN_CWMSG_WINDOW + 1];
  char exch[LEN_SENT_EXCH_WINDOW + 1];
  bool dupe_separate;
};

static ContestWebPreset contest_web_scratch;
static bool contest_web_presets_loaded = false;
static constexpr int N_USER_CONTEST_SLOTS = 2;
static char contest_web_user_slot[N_USER_CONTEST_SLOTS][9];
static const char *CONTEST_PRESET_FILE = "/CONTEST.TXT";
static const char *CONTEST_PRESET_VFS_FILE = "/sd/CONTEST.TXT";
static String contest_web_last_status = "No contest action has been received since boot.";
static bool contest_web_file_loaded = false;
static size_t contest_web_file_size = 0;

static bool valid_web_user_md_basename(const String &filename);

static void set_contest_web_status(const String &message) {
  contest_web_last_status = message;
  if (console) console->printf("WEB CONTEST: %s\n", message.c_str());
  Serial.printf("WEB CONTEST: %s\n", message.c_str());
}

static String contest_web_sd_status() {
  String s = "microSD: ";
  if (SD.cardType() == CARD_NONE) return s + "not mounted / no card";
  s += "mounted, preset=";
  if (SD.exists(CONTEST_PRESET_FILE)) {
    File f = SD.open(CONTEST_PRESET_FILE, FILE_READ);
    if (f) {
      s += CONTEST_PRESET_FILE;
      s += " (";
      s += String(f.size());
      s += " bytes)";
      f.close();
    } else {
      s += "exists but cannot open";
    }
  } else {
    s += "not created yet";
  }
  return s;
}

static void copy_web_value(char *dst, size_t dst_size, const String &src) {
  if (!dst || dst_size == 0) return;
  size_t n = 0;
  while (n + 1 < dst_size && n < src.length()) {
    char c = src.charAt(n);
    dst[n] = (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
    ++n;
  }
  dst[n] = '\0';
}

static String html_attr_escape(const char *src) {
  String out;
  if (!src) return out;
  while (*src) {
    switch (*src) {
      case '&': out += F("&amp;"); break;
      case '"': out += F("&quot;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      default: out += *src; break;
    }
    ++src;
  }
  return out;
}

static String json_string_escape(const char *src) {
  String out;
  if (!src) return out;
  while (*src) {
    switch (*src) {
      case '\\': out += F("\\\\"); break;
      case '"': out += F("\\\""); break;
      case '\r': out += F("\\r"); break;
      case '\n': out += F("\\n"); break;
      case '\t': out += F("\\t"); break;
      default: out += *src; break;
    }
    ++src;
  }
  return out;
}

static bool parse_contest_preset_line(const String &line, const char *wanted_name,
                                      ContestWebPreset *out) {
  int p1 = line.indexOf('\t');
  int p2 = p1 < 0 ? -1 : line.indexOf('\t', p1 + 1);
  int p3 = p2 < 0 ? -1 : line.indexOf('\t', p2 + 1);
  int p4 = p3 < 0 ? -1 : line.indexOf('\t', p3 + 1);
  int p5 = p4 < 0 ? -1 : line.indexOf('\t', p4 + 1);
  int p6 = p5 < 0 ? -1 : line.indexOf('\t', p5 + 1);
  if (p1 < 1 || p2 < 0 || p3 < 0) return false;
  String name = line.substring(0, p1);
  if (!name.equalsIgnoreCase(wanted_name)) return false;

  memset(out, 0, sizeof(*out));
  out->used = true;
  strlcpy(out->name, name.c_str(), sizeof(out->name));
  copy_web_value(out->f1, sizeof(out->f1), line.substring(p1 + 1, p2));
  if (p4 >= 0 && p5 >= 0) {
    copy_web_value(out->f2, sizeof(out->f2), line.substring(p2 + 1, p3));
    copy_web_value(out->f3, sizeof(out->f3), line.substring(p3 + 1, p4));
    copy_web_value(out->f5, sizeof(out->f5), line.substring(p4 + 1, p5));
    if (p6 >= 0) {
      copy_web_value(out->exch, sizeof(out->exch), line.substring(p5 + 1, p6));
      out->dupe_separate = line.substring(p6 + 1).toInt() != 0;
    } else {
      copy_web_value(out->exch, sizeof(out->exch), line.substring(p5 + 1));
    }
  } else {
    copy_web_value(out->f3, sizeof(out->f3), line.substring(p2 + 1, p3));
    copy_web_value(out->exch, sizeof(out->exch), line.substring(p3 + 1));
  }
  return true;
}

static ContestWebPreset *find_contest_web_preset(const char *name, bool create) {
  if (!name || !*name) return NULL;
  bool found = false;
  File f = SD.open(CONTEST_PRESET_FILE, FILE_READ);
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      if (line.endsWith("\r")) line.remove(line.length() - 1);
      // Keep scanning: the last occurrence is the newest appended value.
      if (parse_contest_preset_line(line, name, &contest_web_scratch)) found = true;
    }
    f.close();
  }
  if (found) return &contest_web_scratch;
  if (!create) return NULL;
  memset(&contest_web_scratch, 0, sizeof(contest_web_scratch));
  contest_web_scratch.used = true;
  strlcpy(contest_web_scratch.name, name, sizeof(contest_web_scratch.name));
  return &contest_web_scratch;
}

static void initialize_user_contest_slot_defaults() {
  static const char *default_filename[N_USER_CONTEST_SLOTS] = {
    "PRESET1", "PRESET2"
  };
  for (int i = 0; i < N_USER_CONTEST_SLOTS; ++i) {
    if (!contest_web_user_slot[i][0]) {
      strlcpy(contest_web_user_slot[i], default_filename[i],
              sizeof(contest_web_user_slot[i]));
    }
  }
}

static void load_contest_web_presets() {
  if (contest_web_presets_loaded) return;
  contest_web_presets_loaded = true;
  memset(contest_web_user_slot, 0, sizeof(contest_web_user_slot));
  File f = SD.open(CONTEST_PRESET_FILE, FILE_READ);
  if (!f) {
    contest_web_file_loaded = false;
    contest_web_file_size = 0;
    initialize_user_contest_slot_defaults();
    set_contest_web_status(String("preset file not found at ") + CONTEST_PRESET_FILE + "; using default User presets");
    return;
  }
  contest_web_file_loaded = true;
  contest_web_file_size = f.size();
  set_contest_web_status(String("loaded ") + CONTEST_PRESET_FILE + " (" + String(contest_web_file_size) + " bytes)");
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.endsWith("\r")) line.remove(line.length() - 1);
    for (int i = 0; i < N_USER_CONTEST_SLOTS; ++i) {
      String prefix = String("#USER_SLOT") + String(i + 1) + "=";
      if (!line.startsWith(prefix)) continue;
      String filename = line.substring(prefix.length());
      filename.trim(); filename.toUpperCase();
      if (valid_web_user_md_basename(filename))
        strlcpy(contest_web_user_slot[i], filename.c_str(), sizeof(contest_web_user_slot[i]));
    }
  }
  f.close();
  initialize_user_contest_slot_defaults();
}

static bool save_contest_web_presets() {
  if (SD.cardType() == CARD_NONE) {
    set_contest_web_status("save failed: microSD is not mounted");
    return false;
  }
  FILE *fp = fopen(CONTEST_PRESET_VFS_FILE, "a");
  if (!fp) {
    set_contest_web_status(String("save failed: fopen(") + CONTEST_PRESET_VFS_FILE + ",a) failed, errno=" + String(errno));
    return false;
  }
  int n = fprintf(fp, "#USER_SLOT1=%s\n#USER_SLOT2=%s\n",
                  contest_web_user_slot[0], contest_web_user_slot[1]);
  if (n >= 0 && contest_web_scratch.used) {
    n = fprintf(fp, "%s\t%s\t%s\t%s\t%s\t%s\t%d\n",
                contest_web_scratch.name, contest_web_scratch.f1,
                contest_web_scratch.f2, contest_web_scratch.f3,
                contest_web_scratch.f5, contest_web_scratch.exch,
                contest_web_scratch.dupe_separate ? 1 : 0);
  }
  const bool write_failed = n < 0;
  const bool flush_failed = fflush(fp) != 0;
  const bool close_failed = fclose(fp) != 0;
  if (write_failed || flush_failed || close_failed) {
    set_contest_web_status(String("save failed while appending ") + CONTEST_PRESET_FILE + ", errno=" + String(errno));
    return false;
  }
  File verify = SD.open(CONTEST_PRESET_FILE, FILE_READ);
  if (!verify) {
    set_contest_web_status(String("save failed: cannot reopen ") + CONTEST_PRESET_FILE);
    return false;
  }
  contest_web_file_size = verify.size();
  verify.close();
  contest_web_file_loaded = true;
  set_contest_web_status(String("saved ") + CONTEST_PRESET_FILE + " (" + String(contest_web_file_size) + " bytes)");
  return true;
}

bool save_contest_runtime_preset(const char *contest_name) {
  if (!contest_name || !*contest_name || !plogw) return false;
  load_contest_web_presets();
  ContestWebPreset *p = find_contest_web_preset(contest_name, true);
  if (!p) return false;

  copy_web_value(p->f1, sizeof(p->f1), String(plogw->cw_msg[0] + 2));
  copy_web_value(p->f2, sizeof(p->f2), String(plogw->cw_msg[1] + 2));
  copy_web_value(p->f3, sizeof(p->f3), String(plogw->cw_msg[2] + 2));
  copy_web_value(p->f5, sizeof(p->f5), String(plogw->cw_msg[4] + 2));
  copy_web_value(p->exch, sizeof(p->exch), String(plogw->sent_exch + 2));
  p->dupe_separate = plogw->mask == CW_PH_DUPE_OK;
  return save_contest_web_presets();
}

static void set_current_contest_messages(const ContestWebPreset &p) {
  strlcpy(plogw->cw_msg[0] + 2, p.f1, LEN_CWMSG_WINDOW + 1);
  plogw->cw_msg[0][1] = strlen(plogw->cw_msg[0] + 2);
  strlcpy(plogw->cw_msg[1] + 2, p.f2, LEN_CWMSG_WINDOW + 1);
  plogw->cw_msg[1][1] = strlen(plogw->cw_msg[1] + 2);
  strlcpy(plogw->cw_msg[2] + 2, p.f3, LEN_CWMSG_WINDOW + 1);
  plogw->cw_msg[2][1] = strlen(plogw->cw_msg[2] + 2);
  strlcpy(plogw->cw_msg[4] + 2, p.f5, LEN_CWMSG_WINDOW + 1);
  plogw->cw_msg[4][1] = strlen(plogw->cw_msg[4] + 2);
  strlcpy(plogw->sent_exch + 2, p.exch, LEN_SENT_EXCH_WINDOW + 1);
  plogw->sent_exch[1] = strlen(plogw->sent_exch + 2);
}

bool apply_contest_runtime_preset(const char *contest_name) {
  if (!contest_name || !*contest_name || !plogw) return false;
  load_contest_web_presets();
  ContestWebPreset *p = find_contest_web_preset(contest_name, false);
  if (!p) return false;
  set_current_contest_messages(*p);
  return true;
}

bool get_contest_runtime_sent_exch(const char *contest_name,
                                   char *out, size_t out_size) {
  if (out && out_size) out[0] = '\0';
  if (!contest_name || !*contest_name || !out || out_size == 0) return false;
  load_contest_web_presets();
  ContestWebPreset *p = find_contest_web_preset(contest_name, false);
  if (!p) return false;
  strlcpy(out, p->exch, out_size);
  return true;
}

static AsyncWebParameter *contest_request_param(AsyncWebServerRequest *request, const char *name) {
  if (request->hasParam(name, true)) return request->getParam(name, true);
  if (request->hasParam(name)) return request->getParam(name);
  return NULL;
}

static bool update_preset_from_request(AsyncWebServerRequest *request, const char *name, ContestWebPreset **result) {
  AsyncWebParameter *f1 = contest_request_param(request, "f1");
  AsyncWebParameter *f2 = contest_request_param(request, "f2");
  AsyncWebParameter *f3 = contest_request_param(request, "f3");
  AsyncWebParameter *f5 = contest_request_param(request, "f5");
  AsyncWebParameter *exch = contest_request_param(request, "exch");
  AsyncWebParameter *dupe_separate = contest_request_param(request, "dupe_separate");
  if (!f1 || !f2 || !f3 || !f5 || !exch) return false;
  ContestWebPreset *p = find_contest_web_preset(name, true);
  if (!p) return false;
  copy_web_value(p->f1, sizeof(p->f1), f1->value());
  copy_web_value(p->f2, sizeof(p->f2), f2->value());
  copy_web_value(p->f3, sizeof(p->f3), f3->value());
  copy_web_value(p->f5, sizeof(p->f5), f5->value());
  copy_web_value(p->exch, sizeof(p->exch), exch->value());
  p->dupe_separate = dupe_separate && dupe_separate->value() == "1";
  if (result) *result = p;
  return true;
}

static bool valid_web_user_md_basename(const String &filename) {
  if (filename.length() < 1 || filename.length() > 8) return false;
  for (size_t i = 0; i < filename.length(); ++i) {
    const char c = filename.charAt(i);
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return true;
}

static void setupContestPageHandler() {
  load_contest_web_presets();

  web_server.on("/contests", HTTP_GET, [](AsyncWebServerRequest *request) {
    const bool japanese = request->hasParam("lang") && request->getParam("lang")->value().equalsIgnoreCase("ja");
    struct State { enum Stage:uint8_t {Header,Entry,Footer,Done} stage=Header; size_t offset=0,length=0; int index=0; bool japanese=false; char text[6144]; };
    std::shared_ptr<State> state = std::make_shared<State>();
    if (state) state->japanese = japanese;
    if (!state) {
      request->send(503, "text/plain", "Not enough memory to build contest page");
      return;
    }
    AsyncWebServerResponse *response=request->beginChunkedResponse("text/html",
      [state](uint8_t *buffer,size_t maxLen,size_t chunkIndex) mutable -> size_t {
        (void)chunkIndex; size_t written=0;
        auto prepare=[&](const char *source,bool footer){
          String text=FPSTR(source);
          if (!footer) {
            text.replace("%CURRENT_CONTEST%",html_attr_escape(plogw->contest_name+2));
            text.replace("%SD_STATUS%", html_attr_escape(contest_web_sd_status().c_str()));
            text.replace("%LAST_STATUS%", html_attr_escape(contest_web_last_status.c_str()));
          }
          else {
            text.replace("%LANG%", state->japanese ? "ja" : "en");
            for (int i = 0; i < N_USER_CONTEST_SLOTS; ++i) {
              String filename = contest_web_user_slot[i];
              if (!filename.length() && i == 0 && !contest_web_user_slot[1][0] &&
                  is_user_md_contest_name(plogw->contest_name + 2)) {
                filename = plogw->contest_name + 6;
              }

              String contest_name;
              if (filename.length()) contest_name = String("User") + filename;
              bool current = contest_name.length() &&
                             strcasecmp(plogw->contest_name + 2, contest_name.c_str()) == 0;
              ContestWebPreset *p = contest_name.length()
                                      ? find_contest_web_preset(contest_name.c_str(), false)
                                      : NULL;
              const char *f1 = p ? p->f1 : (current ? plogw->cw_msg[0] + 2 : "");
              const char *f2 = p ? p->f2 : (current ? plogw->cw_msg[1] + 2 : "");
              const char *f3 = p ? p->f3 : (current ? plogw->cw_msg[2] + 2 : "");
              const char *f5 = p ? p->f5 : (current ? plogw->cw_msg[4] + 2 : "");
              const char *ex = p ? p->exch : (current ? plogw->sent_exch + 2 : "");
              String tag = String("%USER") + String(i + 1);

              text.replace(tag + "_CLASS%", current ? " class=\"current\"" : "");
              text.replace(tag + "_FILENAME%", html_attr_escape(filename.c_str()));
              text.replace(tag + "_F1%", html_attr_escape(f1));
              text.replace(tag + "_F2%", html_attr_escape(f2));
              text.replace(tag + "_F3%", html_attr_escape(f3));
              text.replace(tag + "_F5%", html_attr_escape(f5));
              text.replace(tag + "_EXCH%", html_attr_escape(ex));
              text.replace(tag + "_DUPE_CHECKED%", (p && p->dupe_separate) ? "checked" : "");
              text.replace(tag + "_ACTION%", state->japanese ? (current ? "保存して再選択" : "選択して保存") : (current ? "Save & re-select" : "Select & save"));
            }
          }
          text.toCharArray(state->text,sizeof(state->text)); state->length=strnlen(state->text,sizeof(state->text)); state->offset=0;
        };
        auto copy=[&]()->bool { size_t remain=state->length-state->offset,room=maxLen-written,n=remain<room?remain:room; if(n){memcpy(buffer+written,state->text+state->offset,n);written+=n;state->offset+=n;} if(state->offset==state->length){state->offset=0;state->length=0;return true;} return false; };
        while(written<maxLen && state->stage!=State::Done){
          switch(state->stage){
          case State::Header:
            if (!state->length) prepare(state->japanese ? contests_page_header : contests_page_header_en, false);
            if (copy()) state->stage = State::Entry;
            break;
          case State::Entry:
            if(state->index>=contest_definition_count()){state->stage=State::Footer;break;}
            if(!state->length){
              int id=contest_definition_id(state->index); const char *name=contest_definition_name(state->index);
              bool current=plogw->contest_id==id && strcasecmp(plogw->contest_name+2,name)==0;
              ContestWebPreset *p=find_contest_web_preset(name,false);
              const char *f1=p?p->f1:(current?plogw->cw_msg[0]+2:plogw->cw_msg[0]+2);
              const char *f2=p?p->f2:(current?plogw->cw_msg[1]+2:plogw->cw_msg[1]+2);
              const char *f3=p?p->f3:(current?plogw->cw_msg[2]+2:plogw->cw_msg[2]+2);
              const char *f5=p?p->f5:(current?plogw->cw_msg[4]+2:plogw->cw_msg[4]+2);
              const char *ex=p?p->exch:(current?plogw->sent_exch+2:plogw->sent_exch+2);
              bool dupe_ok=contest_definition_mask(state->index)==CW_PH_DUPE_OK;
              String form_id = String("contest_form_") + String(state->index);
              String action_label = state->japanese ? (current ? "保存して再選択" : "選択して保存") : (current ? "Save & re-select" : "Select & save");
              String row=String("<tr")+(current?" class=\"current\"":"")+"><td><form id=\""+form_id+"\" method=\"GET\" action=\"/select_contest\"><input type=\"hidden\" name=\"lang\" value=\""+(state->japanese?"ja":"en")+"\"><input type=\"hidden\" name=\"id\" value=\""+String(id)+"\"></form>"+String(id)+"</td><td class=\"name\"><div class=\"contest-name-field\"><span class=\"name-text\">"+html_attr_escape(name)+"</span><button form=\""+form_id+"\" type=\"submit\">"+action_label+"</button></div></td><td class=\""+(dupe_ok?"dupe-ok":"dupe-ng")+"\">"+(state->japanese ? (dupe_ok?"CW/Phone別":"モード共通") : (dupe_ok?"CW/Phone separate":"All modes"))+"</td>";
              row += "<td><input form=\""+form_id+"\" name=\"f1\" maxlength=\"30\" value=\""+html_attr_escape(f1)+"\"></td>";
              row += "<td><input form=\""+form_id+"\" name=\"f2\" maxlength=\"30\" value=\""+html_attr_escape(f2)+"\"></td>";
              row += "<td><input form=\""+form_id+"\" name=\"f3\" maxlength=\"30\" value=\""+html_attr_escape(f3)+"\"></td>";
              row += "<td><input form=\""+form_id+"\" name=\"f5\" maxlength=\"30\" value=\""+html_attr_escape(f5)+"\"></td>";
              row += "<td><input form=\""+form_id+"\" name=\"exch\" maxlength=\"17\" value=\""+html_attr_escape(ex)+"\"></td>";
              row += "</tr>\n";
              row.toCharArray(state->text,sizeof(state->text)); state->length=strnlen(state->text,sizeof(state->text)); state->offset=0;
            }
            if (copy()) ++state->index;
            break;
          case State::Footer:
            if (!state->length) prepare(state->japanese ? contests_page_footer : contests_page_footer_en, true);
            if (copy()) state->stage = State::Done;
            break;
          case State::Done: break;
          }
        }
        return written;
      });
    response->addHeader("Cache-Control","no-store"); request->send(response);
  });

  web_server.on("/contest_preset", HTTP_GET, [](AsyncWebServerRequest *request) {
    if(!request->hasParam("name")){request->send(400,"text/plain","Missing name");return;}
    ContestWebPreset *p=find_contest_web_preset(request->getParam("name")->value().c_str(),false);
    String json="{\"f1\":\""; json += json_string_escape(p ? p->f1 : ""); json += "\",\"f2\":\""; json += json_string_escape(p ? p->f2 : "");
    json += "\",\"f3\":\""; json += json_string_escape(p ? p->f3 : ""); json += "\",\"f5\":\""; json += json_string_escape(p ? p->f5 : "");
    json += "\",\"exch\":\""; json += json_string_escape(p ? p->exch : ""); json += "\"}";
    request->send(200,"application/json",json);
  });

  web_server.on("/select_contest", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.printf("WEB CONTEST: request /select_contest params=%u\n", (unsigned)request->params());
    AsyncWebParameter *id_param=contest_request_param(request,"id");
    if(!id_param){set_contest_web_status("request rejected: missing contest id");request->send(400,"text/plain",contest_web_last_status);return;}
    int id=id_param->value().toInt(); int index=-1;
    for(int i=0;i<contest_definition_count();++i) if(contest_definition_id(i)==id){index=i;break;}
    if(index<0){set_contest_web_status(String("request rejected: invalid contest id ")+String(id));request->send(400,"text/plain",contest_web_last_status);return;}
    Serial.printf("WEB CONTEST: built-in id=%d name=%s\n", id, contest_definition_name(index));
    ContestWebPreset *p=NULL;
    if(!update_preset_from_request(request,contest_definition_name(index),&p)){set_contest_web_status(String("request rejected: missing preset values for ")+contest_definition_name(index));request->send(400,"text/plain",contest_web_last_status);return;}
    Serial.println("WEB CONTEST: preset values received; saving");
    if(!save_contest_web_presets()){request->send(500,"text/plain",contest_web_last_status);return;}
    Serial.println("WEB CONTEST: preset saved; applying contest");
    plogw->contest_id=id; set_contest_id(); set_current_contest_messages(*p);
    upd_display_info_contest_settings(so2r.radio_selected());
    set_contest_web_status(String("selected ") + (plogw->contest_name+2) + ", F2=\"" + (plogw->cw_msg[1]+2) + "\", EXCH=\"" + (plogw->sent_exch+2) + "\"");
    request->redirect(request->hasParam("lang") && request->getParam("lang")->value().equalsIgnoreCase("ja") ? "/contests?lang=ja" : "/contests?lang=en");
  });

  web_server.on("/select_user_contest", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.printf("WEB CONTEST: request /select_user_contest params=%u\n", (unsigned)request->params());
    AsyncWebParameter *filename_param=contest_request_param(request,"filename");
    AsyncWebParameter *slot_param=contest_request_param(request,"slot");
    if(!filename_param){set_contest_web_status("User request rejected: missing filename");request->send(400,"text/plain",contest_web_last_status);return;}
    if(!slot_param){set_contest_web_status("User request rejected: missing slot");request->send(400,"text/plain",contest_web_last_status);return;}
    int slot=slot_param->value().toInt();
    if(slot<0 || slot>=N_USER_CONTEST_SLOTS){set_contest_web_status(String("User request rejected: invalid slot ")+slot_param->value());request->send(400,"text/plain",contest_web_last_status);return;}
    String filename=filename_param->value(); filename.toUpperCase();
    if(!valid_web_user_md_basename(filename)){set_contest_web_status(String("User request rejected: invalid filename ")+filename);request->send(400,"text/plain",contest_web_last_status);return;}
    if(user_md_contest_loading()){request->send(409,"text/plain","Another User contest is still loading");return;}
    String contestName=String("User")+filename;
    ContestWebPreset *p=NULL;
    if(!update_preset_from_request(request,contestName.c_str(),&p)){request->send(400,"text/plain","Missing or invalid preset values");return;}
    strlcpy(contest_web_user_slot[slot],filename.c_str(),sizeof(contest_web_user_slot[slot]));
    if(!save_contest_web_presets()){request->send(500,"text/plain",contest_web_last_status);return;}
    strncpy(plogw->contest_name+2,contestName.c_str(),LEN_CONTEST_NAME); plogw->contest_name[2+LEN_CONTEST_NAME]='\0';
    set_current_contest_messages(*p);
    upd_display_info_contest_settings(so2r.radio_selected());
    set_user_md_fallback_dupe_mask(p->dupe_separate ? CW_PH_DUPE_OK : CW_PH_DUPE_NG);
    if(!start_user_md_contest(plogw->contest_name+2)){set_contest_web_status(String("saved preset, but failed to start loading /")+filename+".MD");request->send(400,"text/plain",contest_web_last_status);return;}
    set_contest_web_status(String("saved preset and started loading /")+filename+".MD, F2=\""+(plogw->cw_msg[1]+2)+"\", EXCH=\""+(plogw->sent_exch+2)+"\"");
    request->redirect(request->hasParam("lang") && request->getParam("lang")->value().equalsIgnoreCase("ja") ? "/contests?lang=ja" : "/contests?lang=en");
  });
}

//AsyncWebServer web_server(80);

void setupSettingsPageHandler() {
  web_server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request) {
    // The old implementation built two large Arduino Strings (the complete
    // HTML page plus a separately concatenated input-list) before send().
    // On HWVER=1 this can require several large contiguous INTERNAL-RAM
    // allocations and causes severe heap fragmentation / latency.  Stream the
    // same page incrementally instead, as /rigs and /contests already do.
    struct SettingsPageState {
      enum Stage : uint8_t {
        Prefix, Lifetime, Middle, Inputs, Suffix, Done
      } stage = Prefix;
      size_t offset = 0;
      size_t length = 0;
      size_t display_pos = 0;
      char line[384];
      char lifetime[16];
    };

    std::shared_ptr<SettingsPageState> state =
        std::make_shared<SettingsPageState>();
    if (!state) {
      request->send(503, "text/plain", "Not enough memory for Settings page");
      return;
    }
    snprintf(state->lifetime, sizeof(state->lifetime), "%d",
             bandmap_lifetime_minutes);

    const size_t free_internal =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest_internal =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (plogw && plogw->ostream) {
      plogw->ostream->printf(
          "[WEB] settings streaming begin internal_free=%u largest=%u\n",
          (unsigned)free_internal, (unsigned)largest_internal);
    }

    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "text/html",
      [state](uint8_t *buffer, size_t maxLen, size_t index) mutable -> size_t {
        (void)index;
        size_t written = 0;

        static const uint8_t display_order[] = {
          0, 1, 2, 3, 4, 5, 6, 27,
          7, 8, 20, 21, 22, 23, 24, 25, 26,
          9, 10, 11, 12,
          13, 14, 15, 16, 17, 18, 19
        };
        static const char lifetime_marker[] = "%BANDMAP_LIFETIME%";
        static const char inputs_marker[] = "%SETTINGS_INPUTS%";

        const char *life = strstr(settings_page_html, lifetime_marker);
        const char *inputs = strstr(settings_page_html, inputs_marker);
        if (!life || !inputs || life >= inputs) {
          state->stage = SettingsPageState::Done;
          return 0;
        }

        auto copy_range = [&](const char *src, size_t len) -> bool {
          if (state->offset > len) state->offset = len;
          const size_t remain = len - state->offset;
          const size_t room = maxLen - written;
          const size_t n = remain < room ? remain : room;
          if (n) {
            memcpy(buffer + written, src + state->offset, n);
            written += n;
            state->offset += n;
          }
          if (state->offset == len) {
            state->offset = 0;
            return true;
          }
          return false;
        };

        auto copy_cstr = [&](const char *src) -> bool {
          return copy_range(src, strlen(src));
        };

        while (written < maxLen && state->stage != SettingsPageState::Done) {
          switch (state->stage) {
          case SettingsPageState::Prefix:
            if (copy_range(settings_page_html,
                           (size_t)(life - settings_page_html)))
              state->stage = SettingsPageState::Lifetime;
            break;

          case SettingsPageState::Lifetime:
            if (copy_cstr(state->lifetime))
              state->stage = SettingsPageState::Middle;
            break;

          case SettingsPageState::Middle: {
            const char *mid = life + strlen(lifetime_marker);
            if (copy_range(mid, (size_t)(inputs - mid)))
              state->stage = SettingsPageState::Inputs;
            break;
          }

          case SettingsPageState::Inputs:
            if (state->length == 0) {
              while (state->display_pos < sizeof(display_order)) {
                const int i = display_order[state->display_pos++];
                if (i >= N_EDITWIN || pwin_index(i) == NULL) continue;
                const char *attr = "";
                switch (pwin_type_index(i)) {
                case Allowall: attr = ""; break;
                case Callsign: attr = pattern_both; break;
                case Nospace: attr = pattern_no_space; break;
                }
                const int n = snprintf(state->line, sizeof(state->line),
                                       example_input_html,
                                       i, pwin_name_index(i), i, i,
                                       pwin_index(i) + 2,
                                       pwin_index(i)[0] - 1, attr, i);
                state->length = n > 0
                    ? ((size_t)n < sizeof(state->line)
                       ? (size_t)n : sizeof(state->line) - 1)
                    : 0;
                state->offset = 0;
                if (state->length) break;
              }
              if (state->display_pos >= sizeof(display_order) &&
                  state->length == 0) {
                state->stage = SettingsPageState::Suffix;
                break;
              }
            }
            if (state->length && copy_range(state->line, state->length)) {
              state->length = 0;
              if (state->display_pos >= sizeof(display_order))
                state->stage = SettingsPageState::Suffix;
            }
            break;

          case SettingsPageState::Suffix: {
            const char *tail = inputs + strlen(inputs_marker);
            if (copy_cstr(tail)) state->stage = SettingsPageState::Done;
            break;
          }

          case SettingsPageState::Done:
            break;
          }
        }
        return written;
      });

    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });


  web_server.on("/rigs", HTTP_GET, [](AsyncWebServerRequest *request) {
    struct RigsPageState {
      enum Stage : uint8_t { Header, RigEntry, Footer, Done } stage = Header;
      size_t offset = 0;
      size_t line_length = 0;
      int rig_index = 0;
      char line[640];
    };

    RigsPageState state;
    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "text/html",
      [state](uint8_t *buffer, size_t maxLen, size_t index) mutable -> size_t {
        (void)index;
        size_t written = 0;

        // Copy as much as possible from a NUL-terminated source while
        // remembering the current offset between chunk callbacks.
        auto copy_text = [&](const char *src) -> bool {
          const size_t length = strlen(src);
          const size_t remain = length - state.offset;
          const size_t ncopy = (remain < (maxLen - written))
                               ? remain : (maxLen - written);
          if (ncopy > 0) {
            memcpy(buffer + written, src + state.offset, ncopy);
            written += ncopy;
            state.offset += ncopy;
          }
          if (state.offset == length) {
            state.offset = 0;
            return true;
          }
          return false;
        };

        while (written < maxLen && state.stage != RigsPageState::Done) {
          switch (state.stage) {
          case RigsPageState::Header:
            if (copy_text(rigs_page_header)) {
              state.stage = RigsPageState::RigEntry;
            }
            break;

          case RigsPageState::RigEntry:
            if (state.line_length == 0) {
              while (state.rig_index < N_RIG &&
                     *rig_spec[state.rig_index].name == '\0') {
                state.rig_index = N_RIG;
              }

              if (state.rig_index >= N_RIG) {
                state.stage = RigsPageState::Footer;
                break;
              }

              char spec_buf[300];
              spec_buf[0] = '\0';
              print_rig_spec_str(state.rig_index, spec_buf);
              spec_buf[sizeof(spec_buf) - 1] = '\0';

              char rig_label[40];
              snprintf(rig_label, sizeof(rig_label), "RIG %d: %s",
                       state.rig_index,
                       rig_spec[state.rig_index].name);

              const int len = snprintf(state.line, sizeof(state.line),
                                       example_input_html,
                                       state.rig_index,
                                       rig_label,
                                       state.rig_index,
                                       state.rig_index,
                                       spec_buf,
                                       300,
                                       "",
                                       state.rig_index);
              if (len < 0) {
                state.line[0] = '\0';
                state.line_length = 0;
                ++state.rig_index;
                break;
              }

              state.line_length = strnlen(state.line, sizeof(state.line));
              state.offset = 0;
            }

            {
              const size_t remain = state.line_length - state.offset;
              const size_t ncopy = (remain < (maxLen - written))
                                   ? remain : (maxLen - written);
              if (ncopy > 0) {
                memcpy(buffer + written, state.line + state.offset, ncopy);
                written += ncopy;
                state.offset += ncopy;
              }
              if (state.offset == state.line_length) {
                state.offset = 0;
                state.line_length = 0;
                ++state.rig_index;
              }
            }
            break;

          case RigsPageState::Footer:
            if (copy_text(rigs_page_footer)) {
              state.stage = RigsPageState::Done;
            }
            break;

          case RigsPageState::Done:
            break;
          }
        }

        return written;
      });

    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  web_server.on("/set_bandmap_lifetime", HTTP_GET,
                [](AsyncWebServerRequest *request) {
    if (!request->hasParam("minutes")) {
      request->send(400, "text/plain", "minutes is required");
      return;
    }
    const String value = request->getParam("minutes")->value();
    char *endp = nullptr;
    const long minutes = strtol(value.c_str(), &endp, 10);
    if (endp == value.c_str() || *endp != '\0' ||
        minutes < 1 || minutes > 1440) {
      request->send(400, "text/plain",
                    "Bandmap lifetime must be 1..1440 minutes");
      return;
    }
    bandmap_lifetime_minutes = (int)minutes;
    save_settings("");
    request->send(200, "text/plain",
                  String("Bandmap lifetime saved: ") +
                  bandmap_lifetime_minutes + " minutes");
  });

  web_server.on("/save_settings", HTTP_GET, [](AsyncWebServerRequest *request){
    release_memory();
    save_settings(""); 
    request->send(200, "text/plain", "Settings saved");
  });
  
  web_server.on("/load_settings", HTTP_GET, [](AsyncWebServerRequest *request){
    load_settings(""); 
    request->send(200, "text/plain", "Settings loaded");
  });

  web_server.on("/clock_display_mode", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", String(clock_display_mode));
  });

  web_server.on("/set_clock_display_mode", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->hasParam("mode")) {
      request->send(400, "text/plain", "Missing mode");
      return;
    }
    int mode = request->getParam("mode")->value().toInt();
    if (mode != 0 && mode != 1) {
      request->send(400, "text/plain", "Invalid mode");
      return;
    }
    clock_display_mode = mode;
    save_settings("");
    request->send(200, "text/plain", mode == 1 ? "Clock display: UTC (saved)" : "Clock display: JST (saved)");
  });


  web_server.on("/save_rigs", HTTP_GET, [](AsyncWebServerRequest *request){
    save_rigs("RIGS"); 
    request->send(200, "text/plain", "RIG Settings saved");
  });
  
  web_server.on("/load_rigs", HTTP_GET, [](AsyncWebServerRequest *request){
    load_rigs("RIGS");
    request->send(200, "text/plain", "RIG settings loaded from /RIGS.txt");
  });
  

  // 設定更新ハンドラ
  web_server.on("/set_edit", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("index") && request->hasParam("value")) {
      int index = request->getParam("index")->value().toInt();
      String value = request->getParam("value")->value();
      if (index >= 0 && index < N_EDITWIN) {
        if (pwin_index(index) != NULL) {
          if (index == 27) {
            char hostname[LEN_HOST_NAME + 1];
            strlcpy(hostname, value.c_str(), sizeof(hostname));
            if (!normalize_web_mdns_hostname(hostname)) {
              request->send(400, "text/plain",
                            "Invalid hostname. Use letters, digits and '-' only; "
                            "'-' cannot be first or last.");
              return;
            }
            strlcpy(plogw->hostname + 2, hostname, LEN_HOST_NAME + 1);
            plogw->hostname[1] = strlen(plogw->hostname + 2);
            request->send(
                200, "text/plain",
                String("Hostname updated to ") + plogw->hostname + 2 +
                ".local in RAM. Press Save, then restart for mDNS.");
            return;
          }

          strncpy(pwin_index(index) + 2, value.c_str(),
                  pwin_index(index)[0] - 1);
          (pwin_index(index) + 2)[pwin_index(index)[0] - 1] = '\0';
        }
        request->send(200, "text/plain", "Updated setting.");
        return;
      }
    }
    request->send(400, "text/plain", "Invalid parameters.");
  });



  // RIG設定更新ハンドラ
  web_server.on("/rig_edit", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("index") || !request->hasParam("value")) {
      request->send(400, "text/plain", "Missing index or value");
      return;
    }

    const int index = request->getParam("index")->value().toInt();
    const String value = request->getParam("value")->value();
    if (index < 0 || index >= N_RIG) {
      request->send(400, "text/plain", "RIG index out of range");
      return;
    }
    if (value.length() == 0 || value.length() >= 300) {
      request->send(400, "text/plain", "RIG specification must be 1-299 characters");
      return;
    }

    char rig_update_err[96];
    if (!update_rig_spec(index, value.c_str(),
                         rig_update_err, sizeof(rig_update_err))) {
      request->send(409, "text/plain", rig_update_err);
      return;
    }

    char spec_buf[300];
    spec_buf[0] = '\0';
    print_rig_spec_str(index, spec_buf);
    spec_buf[sizeof(spec_buf) - 1] = '\0';
    request->send(200, "text/plain", spec_buf);
  });
}

struct QsoDumpState {
  File file;
  size_t pos = 0;
  bool isCurrent = false;
  String fname;
  bool finished = false;
  union qso_union_tag qso;
  int type = 0; // 0: dump 1:txt 2;adif 3:csv
  String pendingLine ="";
  char dumpbuf[1024];
  bool headerWritten = false;  // added
  String park;
  String summit;
  
};

size_t readQsoChunk( struct QsoDumpState& state, uint8_t* buffer, size_t maxLen) {

#define RECORD_SIZE sizeof(state.qso.all)
  size_t bytesWritten = 0;
  //  char raw[RECORD_SIZE + 1];  // 1レコード分
  //  raw[RECORD_SIZE] = '\0';

  //  char linebuf[2048];

  // 1. ヘッダー未出力なら先に出す
  if (!state.headerWritten) {
    String header;
    if (state.type == 4) {
      // show the leading part of html      
      header += "<body onload=\"document.getElementById('form').submit()\"><form id=\"form\" method=\"POST\" action=\"https://contest.jarl.org/cgi-bin/logsheetform.cgi\"> <input type=\"hidden\" name=\"command\" value=\"load\" />    <textarea name=\"logsheet_file\" cols=\"80\" rows=\"10\">\r\nDVPlogger text log follows; time:";
      header +=plogw->tm;
      header +="\r\n";
    } else if (state.type==2) {
      // adif header
      header+="<eoh>\n";
    } else {
      header = "";
    }

    if (header.length() <= maxLen) {
      memcpy(buffer, header.c_str(), header.length());
      bytesWritten += header.length();
      state.headerWritten = true;
      //      webLog.println("header written");
    } else {
      // ヘッダーすら入らない → ダミー送信
      memcpy(buffer, "\n", 1);
      return 1;
    }
  }

  // 2. pendingLine の処理（前回入らなかった行）
  if (!state.pendingLine.isEmpty()) {
    size_t len = state.pendingLine.length();
    if (bytesWritten + len <= maxLen) {
      memcpy(buffer + bytesWritten, state.pendingLine.c_str(), len);
      bytesWritten += len;
      state.pendingLine = "";
      webLog.println("pending line write");      
    } else {
      // まだ入らない → 1バイトだけ送る
      memcpy(buffer + bytesWritten, "\n", 1);
      webLog.println("dummy write +");
      return bytesWritten + 1;
    }
  }

  // 2. ファイル終端チェック
  if (!state.file || state.finished || state.pos >= state.file.size()) {
    state.finished = true;
    webLog.print("state.file:");    webLog.print(state.file);
    webLog.print(" state.finished:");    webLog.println(state.finished);    
    return 0;
  }

  // main qso read & dump loop
  webLog.print("maxLen:");      webLog.println(maxLen);
  while ((bytesWritten < maxLen) && (bytesWritten < 2048) ) {
    //  while ((bytesWritten < maxLen)) {
    if (state.pos >= state.file.size()) {
      state.finished = true;
      webLog.print("chk state.pos:");      webLog.print(state.pos);
      webLog.print(" state.file.size():");      webLog.println(state.file.size());      
      break;
    }

    state.file.seek(state.pos);
    size_t n = state.file.read((uint8_t*)state.qso.all, RECORD_SIZE);
    if (n < RECORD_SIZE) {
      state.finished = true;
      webLog.println("n<record_size");
      break;
    }
    state.pos += n;

    // 整形処理
    state.dumpbuf[0]='\0';
    if (state.type==0) {
      // dump
      strcat(state.dumpbuf,(char *)state.qso.all1);
    } else {
      reformat_qso_entry(&state.qso);

      // check park number
      char *p1;
      if (!state.park.isEmpty()) {
	if ((p1=strstr(state.qso.entry.remarks,"POTA_MY:"))!=NULL) { // my park information in POTA activation
	  char tmpbuf1[100];
	  strcpy(tmpbuf1,p1+8);
	  p1=strtok(tmpbuf1," ");
	  if (p1!=NULL) {
	    if (!state.park.equals(p1)) {
	      // match
	      continue;
	    }
	  } else {
	    continue;
	  }
	} else {
	  continue;
	}
      }

      if (!state.summit.isEmpty()) {
	if ((p1=strstr(state.qso.entry.remarks,"SOTA_MY:"))!=NULL) { // my park information in SOTA activation
	  char tmpbuf1[100];
	  strcpy(tmpbuf1,p1+8);
	  p1=strtok(tmpbuf1," ");
	  if (p1!=NULL) {
	    if (!state.park.equals(p1)) {
	      // match
	      continue;
	    }
	  } else {
	    continue;
	  }
	} else {
	  continue;
	}
      }
      
      if (state.type==1) {
	// txt
	sprint_qso_entry(state.dumpbuf,&state.qso);
      } else if (state.type==2) {
	// adif
	sprint_qso_entry_adif(state.dumpbuf,&state.qso);
      } else if (state.type==3) {
	// hamlogcsv
	sprint_qso_entry_hamlogcsv(state.dumpbuf,&state.qso);	
      } else if (state.type==4) {
	// jarllog
	sprint_qso_entry(state.dumpbuf,&state.qso);	
      } else {
	// error
	webLog.print("errortic state.type=");webLog.println(state.type);
	state.finished = true;
	break;
      }
    }
    String line;
    line = String(state.dumpbuf) ;

    /*    size_t lineLen = line.length();
    if (bytesWritten + lineLen > maxLen) {
      // 次回にまわす
      state.pos -= RECORD_SIZE;  // 読み戻す
      break;
    }
    */
    
    size_t lineLen = line.length();
    if (bytesWritten+lineLen <= maxLen) {
      memcpy(buffer + bytesWritten, line.c_str(), lineLen);
      bytesWritten += lineLen;
      webLog.print("bytesWritten:");webLog.print(bytesWritten);
      webLog.print(" pos:");webLog.println(state.pos);    
    } else {
      // 5. 今回は出力できない → キャッシュに入れて、最小限の出力
      state.pendingLine = line;
      memcpy(buffer+bytesWritten, "\n", 1);  // ダミーでも返す
      webLog.println("pending line  +cache + dummy write");      
      return bytesWritten+1;
    }
  }

  // 終了処理
  if (state.finished) {
    webLog.println("state.finished reached");
    String footer = "";
    if (state.type!=2 && state.type!=3) { // not adif
      footer += "---- end of file ";
      footer += state.isCurrent ? "(current)" : state.fname;
      footer += " -----\n";
    }
    if (state.type==4) { // jarllog
      footer+="</textarea> <br /><input type=\"submit\" value=\"send to logsheetform\" /></form></body>";
    }
    size_t footerLen = std::min(maxLen - bytesWritten, (size_t)footer.length());
    memcpy(buffer + bytesWritten, footer.c_str(), footerLen);
    bytesWritten += footerLen;
    
    if (!state.isCurrent) state.file.close();
  } else {
    if (bytesWritten==0) {
      // dummy write here
      webLog.println("dummy write");
      memcpy(buffer, "\n", 1);
      return 1;
    }
  }
  webLog.print("returning bytesWritten:");webLog.println(bytesWritten);
  return bytesWritten;
}

void handleQsoLogDump(AsyncWebServerRequest* request, const String& numstr, int type) {
    struct QsoDumpState state;
    state.type = type;
    state.qso.all1[sizeof(state.qso.all)]='\0';
    state.park = request->hasParam("park") ? request->getParam("park")->value() : "";
    state.summit = request->hasParam("summit") ? request->getParam("summit")->value() : "";    
    
    if (numstr.isEmpty()) {
      // 現在ログ (共通ファイル)
      state.file = qsologf;
      state.isCurrent = true;
      state.fname = "";
    } else {
      state.fname = "/qsobak." + numstr;
      if (!SD.exists(state.fname)) {
	request->send(404, "text/plain", "Log file not found.");
	return;
      }
      state.file = SD.open(state.fname, "r");
    }

    if (!state.file) {
      request->send(500, "text/plain", "Failed to open log file.");
      return;
    }

    AsyncWebServerResponse* response = request->beginChunkedResponse(type == 4 ? "text/html" : "text/plain",
								     [state](uint8_t* buffer, size_t maxLen, size_t index) mutable -> size_t {
								       return readQsoChunk(state, buffer, maxLen);
								     });

    response->addHeader("Server", "ESP Async Web Server");
    request->send(response);
  }



const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html lang="en">
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta charset="UTF-8">
</head>
<body>
  <p><h1>DVPlogger Usage</h1></p>
<p><a href="/jarllog">/jarllog</a> to send log to JARL Log Maker.</p>
<p><a href="/readqso">/readqso</a> to read QSO data in text.</p>
<p><a href="/csv">/csv</a> to read QSO data in HAMLOG csv.</p>
<p><a href="/adif">/adif</a> to read QSO data in ADIF format.</p>
<p><a href="/dumpqso">/dumpqso</a> to dump backup qso data\n(*) </p>
<p>jarllog,readqso,dumpqso,csv,adif?num=QSOFILENUM(001,...) to process backup QSO files.</p>
<p>?park=PARK# でPARK#からQRVしたログ(Remarks にPOTA_MY:PARK#)のみ出力します。</p>
<p>?summit=SUMMIT# でSOTA SUMMIT#からQRVしたログ(Remarks にSOTA_MY:SUMMIT#)のみ出力します。</p>
<p><a href="/potahelp?lang=ja">POTA helper (jp)</a> <a href="/potahelp?lang=en">(en)</a> Nearest-park search / ADIF export</p>
<p><a href="/sotahelp?lang=ja">SOTA helper (jp)</a> <a href="/sotahelp?lang=en">(en)</a> Nearest-summit search / ADIF export</p>
<p><a href="/settings">/settings</a> View/Edit Logger Settings</p>
<p><a href="/status">/status</a> DVPlogger Status</p>
<p><a href="/contests?lang=ja">Contest settings (jp)</a> <a href="/contests?lang=en">(en)</a></p>
<p><a href="/rigs">/rigs</a> View/Edit RIG Settings</p>
<p><a href="/bandmap">/bandmap</a> Multi-band Bandmap</p>
<p><a href="https://github.com/JK1DVP/dvplogger/blob/main/DVPlogger_manual_260827.pdf">Manual DVPlogger_manual_260827.pdf</a></p>
<p><a href="/op">/op</a> Web Opeartion Window</p>
<p><a href="/sat">/sat</a> Satellite Operation Helper</p>

  <p><h1>File Upload</h1></p>
  <p>SD Free: %FREESPIFFS% | SD Used: %USEDSPIFFS% | SD Total: %TOTALSPIFFS%</p>
  <form method="POST" action="/upload" enctype="multipart/form-data"><input type="file" name="data"/><input type="submit" name="upload" value="Upload" title="Upload File"></form>
<p>パーシャルチェックのファイルはname.pck (8.3形式)でアップロードしてください。</p>
<p>CALLHISTnameとコマンドを入力すると、name.pckを読み込みます。</p>
  <p>ファイルアップロードの開始終了は表示されませんので、ファイルリスト更新までお待ちください。</p>
<p>After clicking upload it will take some time for the file to firstly upload and then be written to the SD card, there is no indicator that the upload began.  Please be patient.</p>
  <p>Once uploaded the page will refresh and the newly uploaded file will appear in the file list.</p>
  <p>If a file does not appear, it will be because the file was too big, or had unusual characters in the file name (like spaces).</p>
  <p>You can see the progress of the upload by watching the serial output.</p>
  <div id="filelist">Loading SD file list...</div>
<script>
fetch('/filelist', {cache:'no-store'})
  .then(function(r){ if(!r.ok) throw new Error('HTTP '+r.status); return r.text(); })
  .then(function(html){ document.getElementById('filelist').innerHTML=html; })
  .catch(function(e){ document.getElementById('filelist').textContent='SD file list error: '+e; });
</script>
</body>
</html>
)rawliteral";



static String selectedParkCode  = "";
static String selectedParkName  = "";
static String selectedGrid      = ""; // pota variables

static String selSotaCode="", selSotaName="", selGrid=""; // sota variables

// replace target char * for the given *param_name with *param_value , delimiter *delim (allow single delimiter)
void replace_string(char *target,const char *param_name, const char *param_value,const char *delim)
{
  char *p;char tmpbuf[200];int replaced=0;
  *tmpbuf='\0';
  p=strtok(target,delim);  
  while (p!=NULL) {
    // check if start with param_name
    if (strncmp(p,param_name,strlen(param_name))==0) {
      // this arg start with param_name
      if (!replaced) {
	strcat(tmpbuf,param_name);
	strcat(tmpbuf,param_value);
	strcat(tmpbuf,delim);
	replaced=1;
      }
    } else {
      // other param
      strcat(tmpbuf,p);
      strcat(tmpbuf,delim);
    }
    // next arg
    p=strtok(NULL,delim);
  }
  if (!replaced) {
    // add param and value
    strcat(tmpbuf,param_name);
    strcat(tmpbuf,param_value);
  }
  // copy back to the target
  strcpy(target,tmpbuf);
}



// HIDキーコードに対応するマッピング（JavaScriptのキーコードをUSB HIDキーコードに変換）
const std::map<int, int> keycodeToHid = {
  {8, 0x2A},   // Backspace
  {9, 0x2B},   // Tab
  {13, 0x28},  // Enter
  {27, 0x29},  // Escape
  {32, 0x2C},  // Space
  {65, 0x04},  // 'A'
  {66, 0x05},  // 'B'
  {67, 0x06},  // 'C'
  {68, 0x07},  // 'D'
  {69, 0x08},  // 'E'
  {70, 0x09},  // 'F'
  {71, 0x0A},  // 'G'
  {72, 0x0B},  // 'H'
  {73, 0x0C},  // 'I'
  {74, 0x0D},  // 'J'
  {75, 0x0E},  // 'K'
  {76, 0x0F},  // 'L'
  {77, 0x10},  // 'M'
  {78, 0x11},  // 'N'
  {79, 0x12},  // 'O'
  {80, 0x13},  // 'P'
  {81, 0x14},  // 'Q'
  {82, 0x15},  // 'R'
  {83, 0x16},  // 'S'
  {84, 0x17},  // 'T'
  {85, 0x18},  // 'U'
  {86, 0x19},  // 'V'
  {87, 0x1A},  // 'W'
  {88, 0x1B},  // 'X'
  {89, 0x1C},  // 'Y'
  {90, 0x1D},  // 'Z'
  {48, 0x27},  // '0'
  {49, 0x1E},  // '1'
  {50, 0x1F},  // '2'
  {51, 0x20},  // '3'
  {52, 0x21},  // '4'
  {53, 0x22},  // '5'
  {54, 0x23},  // '6'
  {55, 0x24},  // '7'
  {56, 0x25},  // '8'
  {57, 0x26},  // '9'
  {189, 0x2D}, // '-' (minus)
  {187, 0x3D}, // '=' (equals)
  {192, 0x35}, // '`' (backtick)
  {219, 0x2F}, // '[' (left bracket)
  {221, 0x30}, // ']' (right bracket)
  {220, 0x31}, // '\' (backslash)
  {188, 0x36}, // ',' (comma)
  {190, 0x37}, // '.' (period)
  {191, 0x38}, // '/' (slash)
  {38, 0x52},  // Arrow Up
  {40, 0x51},  // Arrow Down
  {37, 0x50},  // Arrow Left
  {39, 0x4F},  // Arrow Right
};


const char antenna_page_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DVPlogger Antenna Settings</title>
<style>body{font-family:sans-serif;margin:16px}table{border-collapse:collapse}th,td{border:1px solid #aaa;padding:5px}input{font-size:15px} .wide{width:360px;max-width:80vw}</style>
</head><body><h2>Antenna Settings</h2>
<p>DVPlogger allocates antennas in radio-number order. Each preference string follows the DVPlogger band order: 1.8, 3.5, 7, 14, 21, 28, 50, 144, 430, 1200, 2400, 5600, 10G, 10, 18, 24 MHz. 0 means no antenna.</p>
<form id="f">
<table><tr><th>Enable</th><td><input id="enable" type="checkbox"></td></tr>
<tr><th>OTRSP host</th><td><input id="host" class="wide"></td></tr><tr><th>Port</th><td><input id="port" type="number"></td></tr>
<tr><th>1st preference</th><td><input id="pref1" class="wide" maxlength="16"></td></tr>
<tr><th>2nd preference</th><td><input id="pref2" class="wide" maxlength="16"></td></tr>
<tr><th>3rd preference</th><td><input id="pref3" class="wide" maxlength="16"></td></tr></table>
<h3>Antenna names</h3><table><thead><tr><th>ID</th><th>Name</th></tr></thead><tbody id="names"></tbody></table>
<p><button type="button" onclick="saveConfig()">Save</button> <span id="msg"></span></p></form>
<p><a href="/op">Back to Operation</a></p>
<script>
async function loadConfig(){const r=await fetch('/antenna_config');const d=await r.json();enable.checked=d.enable;host.value=d.host;port.value=d.port;pref1.value=d.pref[0];pref2.value=d.pref[1];pref3.value=d.pref[2];names.innerHTML=d.names.map((n,i)=>`<tr><td>${i+1}</td><td><input id="name${i+1}" class="wide" maxlength="23" value="${n.replace(/&/g,'&amp;').replace(/"/g,'&quot;')}"></td></tr>`).join('');}
async function saveConfig(){const q=new URLSearchParams();q.set('enable',enable.checked?'1':'0');q.set('host',host.value);q.set('port',port.value);q.set('pref1',pref1.value);q.set('pref2',pref2.value);q.set('pref3',pref3.value);for(let i=1;i<=9;i++)q.set('name'+i,document.getElementById('name'+i).value);const r=await fetch('/antenna_config?'+q.toString(),{method:'POST'});msg.textContent=await r.text();}
loadConfig();
</script></body></html>
)rawliteral";


const char sat_page_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="ja"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DVPlogger Satellite Helper</title>
<style>body{font-family:sans-serif;margin:16px;max-width:1100px}nav a{margin-right:12px}.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px}.card{border:1px solid #aaa;border-radius:8px;padding:12px}table{border-collapse:collapse;width:100%}th,td{border:1px solid #bbb;padding:5px;text-align:left}button,select,input{font-size:1rem;padding:6px}.mono{font-family:monospace}.status-large{font-family:monospace;font-size:1.15rem;line-height:1.55}.ok{color:#075;font-weight:bold}.warn{color:#a50;font-weight:bold}.err{color:#b00;font-weight:bold}.sat-on{display:inline-block;padding:4px 10px;border:2px solid #080;font-weight:bold}.sat-off{display:inline-block;padding:4px 10px;border:2px solid #900;font-weight:bold}
.passrow{cursor:pointer}.passrow:hover{background:#eee}.passrow.sel{font-weight:bold;background:#eef}
.pass-detail{margin-top:10px;padding:10px;border:1px solid #999;border-radius:8px}
.pass-grid{display:grid;grid-template-columns:minmax(280px,380px) 1fr;gap:14px;align-items:start}
.pass-grid>div:nth-child(2){font-size:1.15rem;line-height:1.55}
.pass-grid>div:nth-child(2) h3{font-size:1.5rem;margin-top:0;margin-bottom:.8rem}
.pass-grid>div:nth-child(2) .mono{font-size:1em;line-height:1.55}
#passcanvas{width:100%;max-width:360px;height:auto;background:#111;border:1px solid #666}
@media(max-width:700px){.pass-grid{grid-template-columns:1fr}}</style>
</head><body><nav><a href="/">ホーム</a><a href="/op">運用画面</a><a href="/sat">衛星</a></nav>
<h1>衛星運用ヘルパー</h1>
<p><small>衛星位置・Doppler追尾計算はDVPlogger本体で約500 ms周期で更新されます。</small></p>
<div class="cards"><section class="card"><h2>衛星選択</h2><p>現在: <span id="satmode" class="sat-off">SAT OFF</span></p><select id="sat"></select> <button onclick="selectSat()">選択・開始</button> <button onclick="satOn()">Satellite mode ON</button> <button onclick="satOff()">OFF</button><p id="selectmsg"></p></section>
<section class="card"><h2>現在の追尾状態</h2><div id="status" class="status-large">読み込み中...</div></section>
</div>
<section class="card"><h2>次回パス</h2><p><button onclick="recalcAos()">AOS再計算</button> <span id="aosstate"></span></p>
<table><thead><tr><th>衛星</th><th>AOS</th><th>LOS</th><th>最大仰角</th></tr></thead><tbody id="aos"></tbody></table>
<div id="passdetail" class="pass-detail" hidden>
<div class="pass-grid"><div><canvas id="passcanvas" width="360" height="360"></canvas></div>
<div><h3 id="passname"></h3><div id="passtimes" class="mono"></div><hr>
<div id="passnow" class="mono"></div><div id="passfreq" class="mono"></div>
<p><button onclick="closePass()">閉じる</button></p></div></div>
</div></section>
<div class="cards">
<section class="card"><h2>運用制御</h2>
<p>追尾: <select id="tracking"><option value="0">RX FIX</option><option value="1">TX FIX</option><option value="2">SAT FIX</option><option value="3">NO TRACK</option></select> <button onclick="setTracking()">設定</button></p>
<p>VFO: <select id="vfomode"><option value="0">Single: TX=A</option><option value="1">Single: RX=A</option><option value="2">TX=Radio0 / RX=Radio1</option><option value="3">TX=Radio1 / RX=Radio0</option></select> <button onclick="setVfo()">設定</button> <button onclick="autoVfo()">Auto VFO</button></p><p id="autovfomsg" class="mono"></p>
<p><button onclick="action('center')">Center</button> <button onclick="action('beacon')">Beacon</button></p>
<p>Offset: <button onclick="offset(-100)">-100</button> <button onclick="offset(-10)">-10</button> <button onclick="offset(10)">+10</button> <button onclick="offset(100)">+100</button> Hz</p>
<p id="opmsg"></p></section>
<section class="card"><h2>現在位置・時刻</h2>
<p>Grid Locator: <input id="gridloc" maxlength="8" size="10"> <button onclick="saveLocation()">設定・保存</button></p>
<div id="locstatus" class="status-large">読み込み中...</div>
<div id="clockstatus" class="status-large">読み込み中...</div>
<p><small>標高は現状の衛星計算実装に合わせて50 m固定です。</small></p>
</section></div>
<section class="card"><h2>衛星データ管理</h2>
<p><small>Name はTLE内の衛星名と一致させてください。周波数は MHz、Offset は Hz です。最大26エントリ。</small></p>
<div style="overflow-x:auto"><table><thead><tr><th>Name</th><th>Up MHz</th><th>Mode</th><th>Down MHz</th><th>Mode</th><th>Beacon MHz</th><th>Offset Hz</th><th>TLE</th><th></th></tr></thead><tbody id="satdbbody"></tbody></table></div>
<h3 id="satdbtitle">新規追加</h3>
<input type="hidden" id="satdbindex" value="-1">
<p>Name <input id="satdbname" size="16" maxlength="19">
 Up <input id="satup0" size="9"> - <input id="satup1" size="9"> MHz
 <input id="satupmode" size="5" maxlength="9" placeholder="LSB"></p>
<p>Down <input id="satdn0" size="9"> - <input id="satdn1" size="9"> MHz
 <input id="satdnmode" size="5" maxlength="9" placeholder="USB">
 Beacon <input id="satbeacon" size="9"> MHz
 Offset <input id="satoffset" size="7"> Hz</p>
<p><button onclick="newSatDb()">新規</button> <button onclick="saveSatDb()">保存</button> <span id="satdbmsg"></span></p>
</section>
<section class="card"><h2>TLE管理</h2>
<p><small>「TLE更新」はネットワークから取得してSDへ保存し、成功後にTLEを再読込します。HTTP 200は取得成功です。</small></p>
<label>URL <input id="tleurl" style="width:80%"></label>
<p><button onclick="saveTleUrl()">URL保存</button> <button onclick="updateTle()">TLE更新</button> <span id="tlestate"></span></p></section>
<script>
const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const satEl=document.getElementById('sat');
const statusEl=document.getElementById('status');
const selectMsgEl=document.getElementById('selectmsg');
const satModeEl=document.getElementById('satmode');
const trackingEl=document.getElementById('tracking');
const vfoModeEl=document.getElementById('vfomode');
const opMsgEl=document.getElementById('opmsg');
const autoVfoMsgEl=document.getElementById('autovfomsg');
const gridLocEl=document.getElementById('gridloc');
const locStatusEl=document.getElementById('locstatus');
const clockStatusEl=document.getElementById('clockstatus');
const tleUrlEl=document.getElementById('tleurl');
const tleStateEl=document.getElementById('tlestate');
const aosStateEl=document.getElementById('aosstate');
const aosEl=document.getElementById('aos');
const passDetailEl=document.getElementById('passdetail');
const passCanvasEl=document.getElementById('passcanvas');
const passNameEl=document.getElementById('passname');
const passTimesEl=document.getElementById('passtimes');
const passNowEl=document.getElementById('passnow');
const passFreqEl=document.getElementById('passfreq');
let expandedPassIndex=-1, expandedPassData=null;
let trackingDirty=false, vfoDirty=false;
let lastTleTime=null;
trackingEl.addEventListener('change',()=>trackingDirty=true);
vfoModeEl.addEventListener('change',()=>vfoDirty=true);
async function loadList(){const d=await (await fetch('/api/sat/list',{cache:'no-store'})).json();satEl.innerHTML=d.satellites.map(x=>`<option value="${x.index}" ${x.selected?'selected':''}>${esc(x.name)}</option>`).join('');}
async function loadStatus(){try{const r=await fetch('/api/sat/status',{cache:'no-store'});if(!r.ok)throw new Error('HTTP '+r.status);const d=await r.json();satModeEl.textContent=d.enabled?'SAT ON':'SAT OFF';satModeEl.className=d.enabled?'sat-on':'sat-off';statusEl.innerHTML=`衛星: <b>${esc(d.name)}</b><br>衛星運用: ${d.enabled?'ON':'OFF'}<br>方位: ${Number(d.az).toFixed(1)}°<br>仰角: ${Number(d.el).toFixed(1)}°<br>距離速度: ${Number(d.rr).toFixed(3)} km/s<br>追尾モード: ${esc(d.tracking_name)}<br>VFOモード: ${esc(d.vfo_name)}<br>Uplink: ${d.up_hz} Hz<br>Downlink: ${d.down_hz} Hz<br>Sat Uplink: ${d.sat_up_hz} Hz<br>Sat Downlink: ${d.sat_down_hz} Hz<br>Offset: ${d.offset_hz} Hz`;if(!trackingDirty)trackingEl.value=String(d.tracking_mode);if(!vfoDirty)vfoModeEl.value=String(d.vfo_mode);if(document.activeElement!==gridLocEl)gridLocEl.value=d.grid;locStatusEl.textContent=`Lat: ${Number(d.lat).toFixed(5)}°  Lon: ${Number(d.lon).toFixed(5)}°`;clockStatusEl.textContent=`JST: ${d.jst}   UTC: ${d.utc}`;}catch(e){statusEl.textContent='状態取得失敗: '+e.message;}}
async function loadAos(){try{const r=await fetch('/api/sat/aos',{cache:'no-store'});if(!r.ok)throw new Error('HTTP '+r.status);const d=await r.json();aosStateEl.textContent=d.calculating?`計算中 ${d.completed}/${d.total}: ${d.current_name} (${d.state_name})`:(d.passes.length?`計算完了 (${d.total} satellites / showing ${d.passes.length})`:'有効な計算結果はありません');aosEl.innerHTML=d.passes.map(x=>`<tr class="passrow ${x.index===expandedPassIndex?'sel':''}" onclick="togglePass(${x.index})" title="クリックしてパス詳細を表示"><td>${esc(x.name)}</td><td>${esc(x.aos)}</td><td>${esc(x.los)}</td><td>${Number(x.max_el).toFixed(0)}°</td></tr>`).join('');if(expandedPassIndex>=0&&!d.passes.some(x=>x.index===expandedPassIndex))closePass();}catch(e){aosStateEl.textContent='AOS状態取得失敗: '+e.message;}}

function skyXY(az,el,R,cx,cy){const rr=R*Math.max(0,Math.min(1,(90-el)/90));const a=az*Math.PI/180;return [cx+rr*Math.sin(a),cy-rr*Math.cos(a)];}
function drawSky(points,nowp){
  const c=passCanvasEl,ctx=c.getContext('2d'),w=c.width,h=c.height,cx=w/2,cy=h/2,R=Math.min(w,h)/2-26;
  ctx.clearRect(0,0,w,h);ctx.fillStyle='#111';ctx.fillRect(0,0,w,h);
  ctx.strokeStyle='#aaa';ctx.lineWidth=1;
  [0,30,60].forEach(el=>{ctx.beginPath();ctx.arc(cx,cy,R*(90-el)/90,0,Math.PI*2);ctx.stroke();});
  ctx.beginPath();ctx.moveTo(cx-R,cy);ctx.lineTo(cx+R,cy);ctx.moveTo(cx,cy-R);ctx.lineTo(cx,cy+R);ctx.stroke();
  ctx.fillStyle='#ddd';ctx.font='14px sans-serif';ctx.fillText('N',cx-5,16);ctx.fillText('E',w-17,cy+5);ctx.fillText('S',cx-5,h-8);ctx.fillText('W',5,cy+5);
  if(points&&points.length){
    ctx.strokeStyle='#f33';ctx.lineWidth=3;ctx.beginPath();
    points.forEach((p,k)=>{const q=skyXY(Number(p.az),Number(p.el),R,cx,cy);if(k===0)ctx.moveTo(q[0],q[1]);else ctx.lineTo(q[0],q[1]);});ctx.stroke();
    const qa=skyXY(Number(points[0].az),Number(points[0].el),R,cx,cy),ql=skyXY(Number(points[points.length-1].az),Number(points[points.length-1].el),R,cx,cy);
    ctx.fillStyle='#6cf';ctx.beginPath();ctx.arc(qa[0],qa[1],5,0,Math.PI*2);ctx.fill();
    ctx.fillStyle='#fc6';ctx.beginPath();ctx.arc(ql[0],ql[1],5,0,Math.PI*2);ctx.fill();
  }
  if(nowp&&Number(nowp.el)>=0){
    const q=skyXY(Number(nowp.az),Number(nowp.el),R,cx,cy);ctx.fillStyle='#0ff';ctx.beginPath();ctx.arc(q[0],q[1],7,0,Math.PI*2);ctx.fill();ctx.strokeStyle='#fff';ctx.lineWidth=1;ctx.stroke();
  }
}
function closePass(){expandedPassIndex=-1;expandedPassData=null;passDetailEl.hidden=true;passNowEl.textContent='';document.querySelectorAll('.passrow').forEach(r=>r.classList.remove('sel'));}
async function togglePass(idx){
  if(expandedPassIndex===idx){closePass();return;}
  expandedPassIndex=idx;expandedPassData=null;passDetailEl.hidden=false;passNameEl.textContent='読み込み中...';passTimesEl.textContent='';passNowEl.textContent='';passFreqEl.textContent='';
  try{
    const r=await fetch('/api/sat/pass?index='+idx,{cache:'no-store'});if(!r.ok)throw new Error(await r.text());
    const d=await r.json();expandedPassData=d;passNameEl.textContent=d.name;
    passTimesEl.innerHTML=`AOS: ${esc(d.aos)} &nbsp; AZ ${Number(d.aos_az).toFixed(0)}°<br>MEL: ${esc(d.mel)} &nbsp; AZ ${Number(d.mel_az).toFixed(0)}° &nbsp; EL ${Number(d.mel_el).toFixed(1)}°<br>LOS: ${esc(d.los)} &nbsp; AZ ${Number(d.los_az).toFixed(0)}°`;
    passFreqEl.innerHTML=`UP: ${Number(d.up0/1e6).toFixed(5)}-${Number(d.up1/1e6).toFixed(5)} MHz ${esc(d.upmode)}<br>DN: ${Number(d.dn0/1e6).toFixed(5)}-${Number(d.dn1/1e6).toFixed(5)} MHz ${esc(d.dnmode)}${d.beacon?`<br>Beacon: ${Number(d.beacon/1e6).toFixed(5)} MHz`:''}`;
    drawSky(d.points,null);await updateExpandedNow();
  }catch(e){passNameEl.textContent='パス詳細取得失敗';passTimesEl.textContent=String(e);}
}
async function updateExpandedNow(){
  if(expandedPassIndex<0||!expandedPassData)return;
  try{
    const r=await fetch('/api/sat/nowpos?index='+expandedPassIndex,{cache:'no-store'});if(!r.ok)return;
    const d=await r.json();
    passNowEl.innerHTML=`NOW ${esc(d.jst)}<br>AZ: ${Number(d.az).toFixed(1)}° / EL: ${Number(d.el).toFixed(1)}°${d.active?' &nbsp; <b>PASS ACTIVE</b>':''}${d.selected?`<br>Rig UP: ${d.up_hz} Hz / DN: ${d.down_hz} Hz`:''}`;
    drawSky(expandedPassData.points,d);
  }catch(e){}
}
async function selectSat(){const q=new URLSearchParams({index:satEl.value});const r=await fetch('/api/sat/select?'+q,{method:'POST'});selectMsgEl.textContent=await r.text();trackingDirty=false;vfoDirty=false;await loadStatus();await loadList();}
async function satOn(){const r=await fetch('/api/sat/enable?enabled=1',{method:'POST'});const msg=await r.text();selectMsgEl.textContent=r.ok?msg:`SAT ON失敗 (HTTP ${r.status}): ${msg}`;trackingDirty=false;vfoDirty=false;if(r.ok){await loadList();await loadSatDb();await loadAos();}await loadStatus();}
async function satOff(){const r=await fetch('/api/sat/enable?enabled=0',{method:'POST'});selectMsgEl.textContent=await r.text();await loadStatus();}
async function setTracking(){const r=await fetch('/api/sat/tracking?mode='+trackingEl.value,{method:'POST'});opMsgEl.textContent=await r.text();if(r.ok)trackingDirty=false;await loadStatus();}
async function setVfo(){const r=await fetch('/api/sat/vfo?mode='+vfoModeEl.value,{method:'POST'});opMsgEl.textContent=await r.text();if(r.ok)vfoDirty=false;await loadStatus();}
async function autoVfo(){const r=await fetch('/api/sat/vfo/auto',{method:'POST'});autoVfoMsgEl.textContent=await r.text();if(r.ok)vfoDirty=false;await loadStatus();}
async function action(name){const r=await fetch('/api/sat/action?name='+encodeURIComponent(name),{method:'POST'});opMsgEl.textContent=await r.text();await loadStatus();}
async function offset(hz){const r=await fetch('/api/sat/offset?hz='+hz,{method:'POST'});opMsgEl.textContent=await r.text();await loadStatus();}
async function saveLocation(){const q=new URLSearchParams({grid:gridLocEl.value.trim().toUpperCase()});const r=await fetch('/api/sat/location?'+q,{method:'POST'});locStatusEl.textContent=await r.text();if(r.ok)await loadStatus();}
async function recalcAos(){await fetch('/api/sat/aos/recalculate',{method:'POST'});loadAos();}
let satDbRows=[];
function mhz(v){return (Number(v)/1000000).toFixed(6);}
async function loadSatDb(){try{const d=await (await fetch('/api/sat/db',{cache:'no-store'})).json();satDbRows=d.satellites;satdbbody.innerHTML=d.satellites.map(x=>`<tr><td>${esc(x.name)}</td><td>${mhz(x.up0)}-${mhz(x.up1)}</td><td>${esc(x.upmode)}</td><td>${mhz(x.dn0)}-${mhz(x.dn1)}</td><td>${esc(x.dnmode)}</td><td>${mhz(x.beacon)}</td><td>${x.offset}</td><td>${x.tle?'OK':'--'}</td><td><button onclick="editSatDb(${x.index})">編集</button> <button onclick="deleteSatDb(${x.index})">削除</button></td></tr>`).join('');}catch(e){satdbmsg.textContent='一覧取得失敗: '+e.message;}}
function newSatDb(){satdbindex.value='-1';satdbtitle.textContent='新規追加';satdbname.value='';satup0.value='0';satup1.value='0';satupmode.value='';satdn0.value='0';satdn1.value='0';satdnmode.value='';satbeacon.value='0';satoffset.value='0';}
function editSatDb(idx){const x=satDbRows.find(v=>v.index===idx);if(!x)return;satdbindex.value=String(idx);satdbtitle.textContent='編集: '+x.name;satdbname.value=x.name;satup0.value=mhz(x.up0);satup1.value=mhz(x.up1);satupmode.value=x.upmode;satdn0.value=mhz(x.dn0);satdn1.value=mhz(x.dn1);satdnmode.value=x.dnmode;satbeacon.value=mhz(x.beacon);satoffset.value=String(x.offset);}
async function saveSatDb(){const q=new URLSearchParams({index:satdbindex.value,name:satdbname.value.trim(),up0:satup0.value,up1:satup1.value,upmode:satupmode.value.trim().toUpperCase(),dn0:satdn0.value,dn1:satdn1.value,dnmode:satdnmode.value.trim().toUpperCase(),beacon:satbeacon.value,offset:satoffset.value});const r=await fetch('/api/sat/db/save?'+q,{method:'POST'});satdbmsg.textContent=await r.text();if(r.ok){newSatDb();await loadSatDb();await loadList();}}
async function deleteSatDb(idx){const x=satDbRows.find(v=>v.index===idx);if(!x||!confirm('削除しますか: '+x.name+' ?'))return;const r=await fetch('/api/sat/db/delete?index='+idx,{method:'POST'});satdbmsg.textContent=await r.text();if(r.ok){newSatDb();await loadSatDb();await loadList();}}

async function loadTle(){try{const r=await fetch('/api/sat/tle/status',{cache:'no-store'});if(!r.ok)throw new Error('HTTP '+r.status);const d=await r.json();if(document.activeElement!==tleUrlEl)tleUrlEl.value=d.url;if(d.in_progress){tleStateEl.className='warn';tleStateEl.textContent='TLE更新中...';}else if(d.requested){tleStateEl.className='warn';tleStateEl.textContent='TLE更新待機中...';}else if(d.last_result===200){tleStateEl.className='ok';tleStateEl.textContent=`TLE更新成功 (HTTP 200 / 読込 ${d.valid_satellites}衛星)`;}else if(d.last_result){tleStateEl.className='err';tleStateEl.textContent=d.last_result>0?`TLE更新失敗 (HTTP ${d.last_result})`:`TLE更新失敗 (${d.last_result})`;}else{tleStateEl.className='';tleStateEl.textContent=d.tle_time?`SD上のTLE読込済 (${d.valid_satellites}衛星)`:'TLE未読込';}if(lastTleTime===null){lastTleTime=d.tle_time;}else if(d.tle_time!==lastTleTime){lastTleTime=d.tle_time;await loadList();await loadSatDb();await loadAos();}}catch(e){tleStateEl.className='err';tleStateEl.textContent='TLE状態取得失敗: '+e.message;}}
async function saveTleUrl(){const q=new URLSearchParams({url:tleUrlEl.value});const r=await fetch('/api/sat/tle/config?'+q,{method:'POST'});const msg=await r.text();tleStateEl.className=r.ok?'ok':'err';tleStateEl.textContent=r.ok?'TLE URL保存完了':`URL保存失敗 (HTTP ${r.status}): ${msg}`;}
async function updateTle(){const r=await fetch('/api/sat/tle/update',{method:'POST'});const msg=await r.text();if(r.status===202){tleStateEl.className='warn';tleStateEl.textContent='TLE更新要求を受け付けました';}else{tleStateEl.className='err';tleStateEl.textContent=`TLE更新開始失敗 (HTTP ${r.status}): ${msg}`;}await loadTle();}
async function refresh(){await Promise.all([loadStatus(),loadAos(),loadTle()]);await updateExpandedNow();}
loadList();loadSatDb();refresh();setInterval(refresh,1000);
</script></body></html>
)rawliteral";

const char oppage_html[] PROGMEM =R"rawliteral(
<!DOCTYPE html>
<!-- saved from url=(0021)http://192.168.1.2/op -->
<html lang="en"><head><meta http-equiv="Content-Type" content="text/html; charset=UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  
 <style>
  div {
      margin-bottom: 3px; /* div要素の下に10pxの隙間を追加 */
      padding: 5px;
      background-color: #ffffff;
      border: 1px solid #ccc;
    }
.button-container {
  width: 100vw;              /* 画面の横幅にフィット */
  display: flex;             /* 横並びにする */
  flex-wrap: wrap;           /* 折り返しを許可 */
  gap: 8px;                  /* ボタン間のすき間（任意） */
  padding: 8px;              /* 画面端との余白（任意） */
  box-sizing: border-box;    /* paddingも含めて100vwとする */
}

.button-container button {
  flex: 0 1 auto;            /* 自然な大きさ、必要に応じて縮む */
  min-width: 80px;           /* 必要に応じて指定（任意） */
  font-size: 16px;           /* スマホでも押しやすく */
  background-color: #a0a0a0;
  color: black;
}

    button {
      padding: 10px 20px;
      font-size: 16px;
      background-color: #a0a0a0;
      color: black;
      border: none;
      border-radius: 4px;
      cursor: pointer;
    }

 .form-container {
  width: 100vw;              /* 画面の横幅にフィット */
  display: flex;             /* 横並びにする */
  flex-wrap: wrap;           /* 折り返しを許可 */
  gap: 8px;                  /* ボタン間のすき間（任意） */
  padding: 8px;              /* 画面端との余白（任意） */
  box-sizing: border-box;    /* paddingも含めて100vwとする */
    }

    label {
      font-size: 14px;
      font-weight: bold;
    }

    input#edit_2 {
      padding: 10px;
      font-size: 14px;
      border: 1px solid #ccc;
      border-radius: 4px;
      width: 50px;
    }


    input#edit_0 {
      padding: 10px;
      font-size: 16px;
      border: 1px solid #ccc;
      border-radius: 4px;
      width: 200px;
    }


    input#edit_1 {
      padding: 10px;
      font-size: 16px;
      border: 1px solid #ccc;
      border-radius: 4px;
      width: 200px;
    }

    button {
      padding: 10px 20px;
      font-size: 16px;
      background-color: #a0a0a0;
      color: black;
      border: none;
      border-radius: 4px;
      cursor: pointer;
    }

    button:hover {
      background-color: #a0a0a0;
    }
  
  .ant-section { padding: 8px; }
  .ant-section table { border-collapse: collapse; width: 100%; max-width: 900px; }
  .ant-section th, .ant-section td { border: 1px solid #aaa; padding: 5px 7px; text-align: left; }
  .ant-ready { background: #d9f2d9; }
  .ant-waiting { background: #fff2b3; }
  .ant-tx, .ant-disconnected { background: #f7cccc; }
  .ant-disabled { background: #eeeeee; }
</style>
</head>

<body>
  <h3>DVPlogger Operation</h3>

  <form id="settingsForm" onsubmit="return false;">
    
  <div class="button-container">
 <label "="">Radio:</label>
    <button id="b_radio_0" type="button" onclick="selectRadio(0)" style="background-color: green;">0</button>
   <input type="text" id="edit_10" data-index="10" size="6" enterkeyhint="done" autocomplete="off" autocapitalize="characters" spellcheck="false">
   <button type="button" onclick="sendEnter(10)">Set</button> <!-- radio_name 0 -->
    <button id="b_radio_1" type="button" onclick="selectRadio(1)" style="background-color: gray;">1</button>
   <input type="text" id="edit_11" data-index="11" size="6" enterkeyhint="done" autocomplete="off" autocapitalize="characters" spellcheck="false">
   <button type="button" onclick="sendEnter(11)">Set</button> <!-- radio_name 1 -->
    <button id="b_radio_2" type="button" onclick="selectRadio(2)" style="background-color: gray;">2</button>
   <input type="text" id="edit_12" data-index="12" size="6" enterkeyhint="done" autocomplete="off" autocapitalize="characters" spellcheck="false">
   <button type="button" onclick="sendEnter(12)">Set</button> <!-- radio_name 2 -->

</div>
<div class="button-container">
  <label>Operation:</label>
  <button id="b_radio_mode_0" type="button" onclick="selectRadioMode(0)" style="background-color:gray">SO1R</button>
  <button id="b_radio_mode_1" type="button" onclick="selectRadioMode(1)" style="background-color:gray">SAT</button>
  <button id="b_radio_mode_2" type="button" onclick="selectRadioMode(2)" style="background-color:gray">SO2R</button>
  <button id="b_cqsp" type="button" onclick="toggleCqSp()" title="Toggle CQ / S&amp;P" style="background-color:gray">CQ/S&amp;P</button>
  <span id="radioModeMessage"></span>
</div>
  <div class="button-container">
 <label "="">Mode:</label>
    <button id="b_mode_CW" type="button" onclick="selectMode(&#39;CW&#39;)" style="background-color: gray;">CW</button>
    <button id="b_mode_USB" type="button" onclick="selectMode(&#39;USB&#39;)" style="background-color: green;">USB</button>
    <button id="b_mode_LSB" type="button" onclick="selectMode(&#39;LSB&#39;)" style="background-color: gray;">LSB</button>
    <button id="b_mode_FM" type="button" onclick="selectMode(&#39;FM&#39;)" style="background-color: gray;">FM</button>
</div>
  <div class="button-container">
 <label "="">Band:</label>
    <button id="b_band_1" type="button" onclick="selectBand(1)" style="background-color: green;">1.9</button>
    <button id="b_band_2" type="button" onclick="selectBand(2)" style="background-color: gray;">3.5</button>
    <button id="b_band_3" type="button" onclick="selectBand(3)" style="background-color: gray;">7</button>
    <button id="b_band_4" type="button" onclick="selectBand(4)" style="background-color: gray;">14</button>
    <button id="b_band_5" type="button" onclick="selectBand(5)" style="background-color: gray;">21</button>
    <button id="b_band_6" type="button" onclick="selectBand(6)" style="background-color: gray;">28</button>
    <button id="b_band_7" type="button" onclick="selectBand(7)" style="background-color: gray;">50</button>
    <button id="b_band_8" type="button" onclick="selectBand(8)" style="background-color: gray;">144</button>
    <button id="b_band_9" type="button" onclick="selectBand(9)" style="background-color: gray;">430</button>
    <button id="b_band_10" type="button" onclick="selectBand(10)" style="background-color: gray;">1200</button>
</div>

<div id="radioDisplay">10:08:07       Radio:0 S&amp;P Freq:   1.801.00 Hz Mode: USB</div> <!-- Radio 状態を表示する部分 -->

<div class="button-container">
    <button type="button" onclick="sendKeyCode(27)">ESC</button>
    <button type="button" onclick="sendKeyCode(112)">F1 CQ</button> <!-- F1キー -->
    <button type="button" onclick="sendKeyCode(113)">F2 Exch</button> <!-- F2キー -->
    <button type="button" onclick="sendKeyCode(114)">F3 TU </button> <!-- F3キー -->
    <button type="button" onclick="sendKeyCode(115)">F4 MyCALL</button> <!-- F4キー -->
    <button type="button" onclick="sendKeyCode(116)">F5 Call&amp;Exch</button> <!-- F5キー -->
</div>

<div class="form-container">
   <label for="edit_0">Call:</label>
   <input type="text" id="edit_0" data-index="0" size="11" enterkeyhint="go" autocomplete="off" autocapitalize="characters" spellcheck="false">
   <button type="button" onclick="sendEnter(0)"> ⏎ </button>
</div>
<div class="form-container">
   <label for="edit_1">Recv:</label>
   <input type="text" id="edit_2" data-index="2" size="3" inputmode="numeric" enterkeyhint="next"> <!-- received rst -->
   <input type="text" id="edit_1" data-index="1" size="11" enterkeyhint="go" autocomplete="off" autocapitalize="characters" spellcheck="false"> <!-- received exch -->
   <button type="button" onclick="sendEnter(1)"> ⏎ </button>
   <button type="button" onclick="sendEnter(1, true)">Log (No TX)</button>
</div>
<div class="form-container" style="display:flex;flex-wrap:wrap;gap:0.35em;align-items:center;">
   <label for="cw_message">CW / Voice:</label>
   <input type="text" id="cw_message" size="28" maxlength="99"
          enterkeyhint="send" autocomplete="off" autocapitalize="characters" spellcheck="false"
          placeholder="$C $V $W">
   <button type="button" onclick="sendCwMessage()">Send</button>
   <label for="cw_wpm">WPM:</label>
   <input type="number" id="cw_wpm" size="4" min="5" max="80" inputmode="numeric" enterkeyhint="done">
   <button type="button" onclick="setCwWpm()">Set</button>
   <button id="toneCwButton" type="button" onclick="toggleToneCw()" style="background-color:gray">Tone CW OFF</button>
</div>
<div class="form-container macro-help">
   CW: keyed as entered. PHONE: synthesized voice when VoiceMemory=3.<br>
   Tone CW ON sends CW while the rig remains in a PHONE mode.<br>
   Macros (CW/Voice): $I MyCall / $C Call / $V RST / $W Exch / $U CQ / $T TEST / $A TU / $P Power / $J JCC / $S Serial / $Q Band serial / $N Name
</div>
<div class="form-container">
   <label for="edit_5">MyCall:</label>
   <input type="text" id="edit_5" data-index="5" size="11" enterkeyhint="done" autocomplete="off" autocapitalize="characters" spellcheck="false">
   <button type="button" onclick="sendEnter(5)">Set</button> <!-- sent rst -->
   <label for="edit_4">Sent:</label>
   <input type="text" id="edit_3" data-index="3" size="3" inputmode="numeric" enterkeyhint="next"> <!-- sent rst -->
   <input type="text" id="edit_4" data-index="4" size="11" enterkeyhint="done" autocomplete="off" autocapitalize="characters" spellcheck="false">
   <button type="button" onclick="sendEnter(3); sendEnter(4)">Set Sent</button> <!-- sent number -->
</div>
<div class="form-container">
        <label>Contest:</label>
   <input type="text" id="edit_13" data-index="13" size="20" enterkeyhint="done" autocomplete="off" spellcheck="false">
   <button type="button" onclick="sendEnter(13)">Set</button> <!-- contest_name 0 -->
    </div>
<div class="form-container" id="cwkeyingDisplay"></div> <!-- CW keying ticker display -->
<div class="ant-section">
  <h4>SO2R active pair</h4>
  <p>The selected two radios always alternate. Keyboard in CALLSIGN: Ctrl+Enter = pair with next enabled radio, Ctrl+Shift+Enter = pair with previous enabled radio.</p>
  <table>
    <thead><tr><th>Active pair</th><th>Choose pair</th></tr></thead>
    <tbody id="so2rPairRows"><tr><td colspan="2">Loading...</td></tr></tbody>
  </table>
</div>
<div class="ant-section">
  <h4>Network services</h4>
  <p>Mode is saved independently of host/IP settings. AUTO reconnects in the background; OFF keeps the address but makes no connection attempts.</p>
  <table>
    <thead><tr><th>Service</th><th>Mode</th><th>Connection</th><th>Control</th></tr></thead>
    <tbody id="networkServiceRows"><tr><td colspan="4">Loading...</td></tr></tbody>
  </table>
  <p>CALLSIGN commands: <code>ZSERVERON/OFF</code>, <code>CLUSTER1ON/OFF</code>, <code>CLUSTER2ON/OFF</code>.</p>
</div>
<div class="ant-section">
  <h4>Antenna</h4>
  <table>
    <tbody>
      <tr><th>Controller</th><td id="antController">-</td><th>Connection</th><td id="antConnection">-</td></tr>
      <tr><th>Host</th><td id="antHost">-</td><th>State</th><td id="antState">-</td></tr>
      <tr><th>Reason</th><td id="antReason" colspan="3">-</td></tr>
    </tbody>
  </table>
  <table style="margin-top:6px">
    <thead><tr><th>Radio</th><th>Band</th><th>Current</th><th>Requested</th><th>Pref</th><th>Status</th></tr></thead>
    <tbody id="antRadioRows"><tr><td colspan="6">Loading...</td></tr></tbody>
  </table>
  <p><a href="/antenna">Antenna settings</a></p>
</div>
    <p><a href="/bandmap">Open Bandmap</a> | <a href="/contests">Contest settings</a> | <a href="/">go back to Home</a></p>

  </form>

<script>
function normalizeOpValue(index, value) {
  let v = String(value || '');
  if ([0,1,4,5,10,11,12].includes(index)) v = v.toUpperCase();
  if ([0,1].includes(index)) return v.replace(/[^A-Z0-9\/.]/g, '');
  if ([2,3].includes(index)) return v.replace(/[^0-9]/g, '');
  // Sent Exch (index 4) accepts the same visible ASCII macro characters
  // as CW message fields, including '$', '#', '%', '&', '*', '+', etc.
  if ([4,5,10,11,12,13].includes(index)) return v.replace(/[^\x21-\x7E]/g, '');
  return v.replace(/[^\x20-\x7E]/g, '');
}
function normalizeOpInput(input) {
  const index = Number(input.dataset.index);
  const normalized = normalizeOpValue(index, input.value);
  if (input.value !== normalized) input.value = normalized;
  return normalized;
}

document.addEventListener('DOMContentLoaded', () => {
  document.querySelectorAll('input[data-index]').forEach(input => {
    input.addEventListener('input', () => normalizeOpInput(input));
    input.addEventListener('change', () => normalizeOpInput(input));
  });
});

function setSelectedButton(prefix, value) {
  const selectedId = `${prefix}_${value}`;
  document.querySelectorAll(`[id^="${prefix}_"]`).forEach(el => {
    el.style.backgroundColor = (el.id === selectedId) ? "green" : "gray";
  });
}

async function sendControl(type, value, buttonPrefix) {
  requestFastOpPolling();
  // Reflect the user's operation immediately.  The periodic status poll later
  // corrects the display if the command is rejected or the rig reports a
  // different state.
  if (buttonPrefix) setSelectedButton(buttonPrefix, value);
  try {
    const response = await fetch(`/control?type=${encodeURIComponent(type)}&value=${encodeURIComponent(value)}`,
                                 { cache: 'no-store' });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    // Do not wait for the normal long polling cycle.  Give the main loop a
    // short opportunity to consume the queue, then reconcile this button.
    setTimeout(() => {
      const index = type === 'Radio' ? 7 : (type === 'Mode' ? 8 : 9);
      fetchStatus_button(index, buttonPrefix);
      fetchStatus(99, 'radioDisplay');
    }, 120);
  } catch (error) {
    console.error(`Control ${type} failed:`, error);
    // Restore the authoritative state promptly after an enqueue/network error.
    const index = type === 'Radio' ? 7 : (type === 'Mode' ? 8 : 9);
    fetchStatus_button(index, buttonPrefix);
  }
}

function selectRadio(name) {
  opSelectedRadio = Number(name);
  opRadioSelectionPendingUntil = Date.now() + 1500;
  sendControl('Radio', name, 'b_radio');
}
function selectMode(name) {
  sendControl('Mode', name, 'b_mode');
}
function selectBand(name) {
  sendControl('Band', name, 'b_band');
}
async function selectRadioMode(mode) {
  requestFastOpPolling(2000);
  selectStatusButton('b_radio_mode', mode);
  const message = document.getElementById('radioModeMessage');
  if (message) message.textContent = ' Queuing...';
  try {
    const response = await fetch(`/radio_mode?mode=${mode}`, {cache:'no-store'});
    const text = await response.text();
    if (!response.ok) throw new Error(text || `HTTP ${response.status}`);
    if (message) message.textContent = ' Saved';
    setTimeout(fetchOpStatus, 150);
  } catch (error) {
    if (message) message.textContent = ` ${error.message}`;
    setTimeout(fetchOpStatus, 150);
  }
}

function updateCqSpButtonFromRadioText(text) {
  const button = document.getElementById('b_cqsp');
  if (!button) return;
  const isSp = String(text || '').includes(' S&P Freq:');
  const isCq = String(text || '').includes(' CQ Freq:');
  button.textContent = isSp ? 'S&P' : (isCq ? 'CQ' : 'CQ/S&P');
  button.style.backgroundColor = (isSp || isCq) ? 'green' : 'gray';
}

async function toggleCqSp() {
  requestFastOpPolling(2000);
  try {
    const response = await fetch(`/rig_key?command=cqsp&radio=${encodeURIComponent(opSelectedRadio)}`,
                                 {cache:'no-store'});
    const text = await response.text();
    if (!response.ok) throw new Error(text || `HTTP ${response.status}`);
    setTimeout(fetchOpStatus, 120);
  } catch (error) {
    console.error('CQ/S&P toggle failed:', error);
    setTimeout(fetchOpStatus, 120);
  }
}

// /opの主要状態を1回のHTTP要求で取得する。
let forceOpInputSyncUntil = 0;
let qsoFieldSyncing = [false, false];
let suppressQsoBlurOnce = [false, false];
let lastSyncedQsoValue = ['', ''];
let opStatusFetching = false;
let opFastPollUntil = 0;
let opPollTimer = null;
let opSelectedRadio = 0;
let opRadioSelectionPendingUntil = 0;
const OP_DEBUG = false;

function opDebug(...args) {
  if (OP_DEBUG) console.log(...args);
}

function setTextIfChanged(id, value) {
  const el = document.getElementById(id);
  if (el && el.textContent !== value) el.textContent = value;
}

function requestFastOpPolling(durationMs = 1500) {
  opFastPollUntil = Math.max(opFastPollUntil, Date.now() + durationMs);
}

function selectStatusButton(prefix, value) {
  const selectedId = `${prefix}_${value}`;
  document.querySelectorAll(`[id^="${prefix}_"]`).forEach(el => {
    const color = el.id === selectedId ? "green" : "gray";
    if (el.style.backgroundColor !== color) el.style.backgroundColor = color;
  });
}

async function fetchOpStatus() {
  if (opStatusFetching) return;
  opStatusFetching = true;
  try {
    const response = await fetch('/op_status', {cache: 'no-store'});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const fields = (await response.text()).split('\t');
    if (fields.length < 19) return;
    const force = Date.now() < forceOpInputSyncUntil;
    const active = document.activeElement;
    const inputIndexes = [0,1,2,3,4,5,10,11,12,13];
    inputIndexes.forEach((idx, pos) => {
      const el = document.getElementById(`edit_${idx}`);
      const syncing = (idx === 0 || idx === 1) && qsoFieldSyncing[idx];
      if (idx === 0 || idx === 1) lastSyncedQsoValue[idx] = fields[pos];
      if (el && !syncing && (force || active !== el) && el.value !== fields[pos]) {
        el.value = fields[pos];
      }
    });
    setTextIfChanged('radioDisplay', fields[10]);
    updateCqSpButtonFromRadioText(fields[10]);
    setTextIfChanged('cwkeyingDisplay', fields[11]);
    selectStatusButton('b_radio', fields[12]);
    if (Date.now() >= opRadioSelectionPendingUntil) {
      opSelectedRadio = Number(fields[12]);
    }
    selectStatusButton('b_mode', fields[13]);
    selectStatusButton('b_band', fields[14]);
    selectStatusButton('b_radio_mode', fields[16]);
    const wpmInput = document.getElementById('cw_wpm');
    if (wpmInput && document.activeElement !== wpmInput && wpmInput.value !== fields[17]) {
      wpmInput.value = fields[17];
    }
    updateToneCwButton(fields[18]);
  } catch (error) {
    console.error('op status fetch failed:', error);
  } finally {
    opStatusFetching = false;
  }
}

function scheduleNextOpStatusPoll(delayMs) {
  if (opPollTimer !== null) clearTimeout(opPollTimer);
  opPollTimer = setTimeout(pollOpStatus, delayMs);
}

async function pollOpStatus() {
  await fetchOpStatus();
  const interval = document.hidden ? 5000 : (Date.now() < opFastPollUntil ? 500 : 1000);
  scheduleNextOpStatusPoll(interval);
}

async function fetchNetworkServices() {
  try {
    const response = await fetch('/network_services_status', {cache:'no-store'});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    const rows = [
      ['Zserver','zserver',data.zserver],
      ['Cluster 1','cluster1',data.cluster1],
      ['Cluster 2','cluster2',data.cluster2]
    ].map(([label,key,item]) => {
      const connected = item.connected ? 'CONNECTED' : item.state;
      const mode = item.auto ? 'AUTO' : 'OFF';
      const style = item.connected ? 'background-color:green;color:white' :
                    (item.auto ? 'background-color:#ddd' : 'background-color:#eee;color:#777');
      return `<tr><td>${label}</td><td>${mode}</td><td>${connected}</td><td><button type="button" style="${style}" onclick="setNetworkServiceMode('${key}',${item.auto ? 0 : 1})">${item.auto ? 'Set OFF' : 'Set AUTO'}</button></td></tr>`;
    });
    rows.push(`<tr><td>NTP</td><td>AUTO</td><td>${data.ntp.synced ? 'SYNCHRONIZED' : (data.ntp.started ? 'SYNCING' : 'OFFLINE / RETRY WAIT')}</td><td>-</td></tr>`);
    document.getElementById('networkServiceRows').innerHTML = rows.join('');
  } catch (error) {
    document.getElementById('networkServiceRows').innerHTML =
      '<tr><td colspan="4">Status unavailable</td></tr>';
  }
}

async function setNetworkServiceMode(service, autoMode) {
  try {
    const response = await fetch(`/network_service_mode?service=${encodeURIComponent(service)}&auto=${autoMode}`, {cache:'no-store'});
    if (!response.ok) throw new Error(await response.text());
    await fetchNetworkServices();
  } catch (error) {
    console.error('Network service mode update failed:', error);
  }
}

async function fetchAntennaStatus() {
  try {
    const response = await fetch('/antenna_status');
    const data = await response.json();
    document.getElementById('antController').textContent = data.controller;
    document.getElementById('antConnection').textContent = data.connection;
    document.getElementById('antHost').textContent = data.host;
    document.getElementById('antState').textContent = data.state + (data.pending ? ' (Pending)' : '');
    document.getElementById('antReason').textContent = data.reason;
    const rows = data.radios.map(r => {
      let pref = r.pref > 0 ? (r.pref === 1 ? '1st' : (r.pref === 2 ? '2nd' : '3rd')) : '-';
      if (r.blockedBy >= 0 && r.pref > 1) pref += ` (R${r.blockedBy} using earlier choice)`;
      const cls = 'ant-' + r.status.toLowerCase();
      const current = r.current > 0 ? `${r.current} / ${r.currentName}` : (r.current < 0 ? 'Unknown' : 'None');
      const requested = r.requested > 0 ? `${r.requested} / ${r.requestedName}` : 'None';
      return `<tr class="${cls}"><td>R${r.radio}</td><td>${r.band}</td><td>${current}</td><td>${requested}</td><td>${pref}</td><td>${r.status}</td></tr>`;
    }).join('');
    document.getElementById('antRadioRows').innerHTML = rows;
  } catch (error) {
    document.getElementById('antConnection').textContent = 'Status unavailable';
  }
}

async function fetchSo2rPairs() {
  try {
    const response = await fetch('/so2r_pair_status', {cache: 'no-store'});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    const buttons = [];
    // Always show every possible two-radio combination.  A pair is selectable
    // only when both radios are enabled; the active pair remains green.
    for (let a = 0; a < data.enabled.length; ++a) {
      for (let b = a + 1; b < data.enabled.length; ++b) {
        const usable = !!data.enabled[a] && !!data.enabled[b];
        const selected = (a === data.a && b === data.b) ||
                         (a === data.b && b === data.a);
        let style = selected ? 'background-color:green;color:white' :
                    (usable ? 'background-color:gray;color:white' :
                              'background-color:#ddd;color:#888');
        buttons.push(`<button type="button" style="${style}" ${usable ? '' : 'disabled'} onclick="setSo2rPair(${a},${b})">R${a}-R${b}</button>`);
      }
    }
    document.getElementById('so2rPairRows').innerHTML =
      `<tr><td>R${data.a} &harr; R${data.b}</td><td>${buttons.join(' ')}</td></tr>`;
  } catch (error) {
    document.getElementById('so2rPairRows').innerHTML =
      '<tr><td colspan="2">Status unavailable</td></tr>';
  }
}

async function setSo2rPair(tx, rx) {
  try {
    const response = await fetch(`/so2r_pair?tx=${tx}&rx=${rx}`, {cache: 'no-store'});
    if (!response.ok) throw new Error(await response.text());
    await fetchSo2rPairs();
  } catch (error) {
    console.error('SO2R pair update failed:', error);
  }
}

pollOpStatus();
fetchSo2rPairs();
fetchNetworkServices();
fetchAntennaStatus();
setInterval(fetchSo2rPairs, 3000);
setInterval(fetchNetworkServices, 3000);
setInterval(fetchAntennaStatus, 3000);
document.addEventListener('visibilitychange', () => {
  scheduleNextOpStatusPoll(document.hidden ? 5000 : 0);
});

// F1〜F5ボタンを押したときにキーコードを送信
function sendKeyCode(keyCode) {
  requestFastOpPolling();
  opDebug(`Sending key code: ${keyCode}`);
  fetch(`/rig_key?keycode=${keyCode}`)
    .then(response => response.text())
    .then(data => opDebug("Sent key code:", keyCode, "Response:", data))
    .catch(error => console.error("Error sending keycode:", error));
}

// フォーカス移動と入力内容送信
function sendEnter(inputIndex, noTx = false) {

  // inputIndex = 0  Call 1 Exch 6 Radio0 name  7 Radio1 name 8 Radio2 name
  requestFastOpPolling();
  opDebug('sendEnter called',inputIndex);
  if (inputIndex == 0 || inputIndex == 1 ) {
    // 入力内容を送信
    const input1 = normalizeOpInput(document.getElementById('edit_0'));
    const input2 = normalizeOpInput(document.getElementById('edit_1'));

    fetch(`/rig_key?keycode=13&input0=${encodeURIComponent(input1)}&input1=${encodeURIComponent(input2)}&index=${inputIndex}${noTx ? "&no_tx=1" : ""}`)
      .then(res => res.text())
      .then(msg => opDebug('→ rig_key response:', msg))
      .catch(err => console.error('fetch error:', err));
  } else {
    // set remote variable in general
    const value = normalizeOpInput(document.getElementById(`edit_${inputIndex}`));
    fetch(`/rig_key?command=set&index=${inputIndex}&value=${encodeURIComponent(value)}`)
      .then(res => res.text())
      .then(msg => opDebug('→ rig_key response to radio setting:', msg))
      .catch(err => console.error('fetch error:', err));
  }

  // フォーカスを次のinputに移動（ループ）
  if (inputIndex === 0) {  // callsign
    document.getElementById('edit_1').focus();
  } else if (inputIndex === 1) {  // recv exch
    document.getElementById('edit_0').focus();
  }
  if (inputIndex === 0 || inputIndex === 1) {
    forceOpInputSyncUntil = Date.now() + 1200;
    requestFastOpPolling();
    scheduleNextOpStatusPoll(100);
  }
}

function sendCwMessage() {
  const input = document.getElementById('cw_message');
  const message = String(input.value || '').replace(/[^\x20-\x7E]/g, '');
  if (!message) return;
  requestFastOpPolling();
  fetch(`/rig_key?command=cw_send&radio=${encodeURIComponent(opSelectedRadio)}&value=${encodeURIComponent(message)}`)
    .then(res => res.text().then(text => ({ok: res.ok, text})))
    .then(result => { if (!result.ok) throw new Error(result.text); })
    .catch(error => console.error('CW / Voice send failed:', error));
}

function setCwWpm() {
  const input = document.getElementById('cw_wpm');
  const value = Number(input.value);
  if (!Number.isFinite(value)) return;
  requestFastOpPolling(2000);
  fetch(`/rig_key?command=wpm&value=${encodeURIComponent(value)}`)
    .then(res => res.text().then(text => ({ok: res.ok, text})))
    .then(result => {
      if (!result.ok) throw new Error(result.text);
      setTimeout(fetchOpStatus, 100);
    })
    .catch(error => console.error('WPM update failed:', error));
}

function updateToneCwButton(enabled) {
  const button = document.getElementById('toneCwButton');
  if (!button) return;
  const on = Number(enabled) !== 0;
  button.textContent = on ? 'Tone CW ON' : 'Tone CW OFF';
  button.style.backgroundColor = on ? 'green' : 'gray';
  button.style.color = on ? 'white' : '';
}

function toggleToneCw() {
  requestFastOpPolling(2000);
  fetch(`/rig_key?command=tone_cw&radio=${encodeURIComponent(opSelectedRadio)}`)
    .then(res => res.text().then(text => ({ok: res.ok, text})))
    .then(result => {
      if (!result.ok) throw new Error(result.text);
      setTimeout(fetchOpStatus, 100);
    })
    .catch(error => console.error('Tone CW update failed:', error));
}

function clearInputs() {
  document.getElementById('edit_0').value = '';  // callsignをクリア
  document.getElementById('edit_1').value = '';  // recv exchをクリア
  document.getElementById('edit_2').value = '';  // recv rstをクリア
}

async function syncQsoFieldWithoutEnter(index) {
  index = Number(index);
  if (index !== 0 && index !== 1) return;

  if (suppressQsoBlurOnce[index]) {
    suppressQsoBlurOnce[index] = false;
    return;
  }

  const input = document.getElementById(`edit_${index}`);
  if (!input) return;

  const value = normalizeOpInput(input);
  if (value === lastSyncedQsoValue[index]) return;

  qsoFieldSyncing[index] = true;
  requestFastOpPolling();
  try {
    const response = await fetch(
      `/rig_key?command=set&index=${index}&value=${encodeURIComponent(value)}`,
      { cache: 'no-store' });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    lastSyncedQsoValue[index] = value;
  } catch (error) {
    console.error(`QSO field ${index} blur sync failed:`, error);
  } finally {
    // The response means queued, not yet applied by the main loop.  Prevent
    // /op_status from restoring the old value during that short interval.
    setTimeout(() => {
      qsoFieldSyncing[index] = false;
      fetchOpStatus();
    }, 300);
  }
}

// keydownイベントの監視
document.addEventListener("DOMContentLoaded", () => {
  const callInput = document.getElementById('edit_0');
  const exchInput = document.getElementById('edit_1');
  if (callInput) callInput.addEventListener('blur', () => syncQsoFieldWithoutEnter(0));
  if (exchInput) exchInput.addEventListener('blur', () => syncQsoFieldWithoutEnter(1));
  const cwMessage = document.getElementById('cw_message');
  const cwWpm = document.getElementById('cw_wpm');
  if (cwMessage) cwMessage.addEventListener('keydown', event => {
    if (event.key === 'Enter') { event.preventDefault(); sendCwMessage(); }
  });
  if (cwWpm) cwWpm.addEventListener('keydown', event => {
    if (event.key === 'Enter') { event.preventDefault(); setCwWpm(); }
  });

  document.addEventListener("keydown", event => {
    const key = event.key;
    const code = event.keyCode || event.which;
    const focused = document.activeElement;
    const idx = focused && focused.dataset ? focused.dataset.index || "" : "";
    if (focused && (focused.id === 'cw_message' || focused.id === 'cw_wpm')) return;

    opDebug('keydown idx=', idx, 'key=', key);

    // Shiftキーが押されたとき
    if (key === "Shift") {
      handleShiftKey(event);
    }

    // TabキーまたはSpaceキーが押された場合にデフォルト動作を防ぐ
    if (key === "Tab" || key === " ") {
      event.preventDefault();  // フォーカス移動を防止
      if (idx === "0") {
        document.getElementById('edit_1').focus();
      } else if (idx === "1") {
        document.getElementById('edit_0').focus();
      }
    }

    // Enterキーが押された場合、両方のinput欄の内容を送信
    if (key === "Enter") {
      event.preventDefault();  // フォーム送信を防ぐ

      // sendEnter() already commits both QSO fields and runs the normal
      // Call/EXCH Enter operation.  Suppress the blur-only update caused by
      // the focus move performed inside sendEnter().
      if (idx === "0" || idx === "1") suppressQsoBlurOnce[Number(idx)] = true;
      sendEnter(Number(idx));
      return;  // Enter is already queued by sendEnter(); do not send it twice.
    }

    // fetchでキー送信（Space、Tabも含む）
    fetch(`/rig_key?keycode=${code}${idx !== "" ? "&index=" + idx : ""}`)
      .then(res => res.text())
      .then(msg => opDebug('→ rig_key response:', msg))
      .catch(err => console.error('fetch error:', err));
  });
});

function handleShiftKey(event) {
  if (event.location === 0 && !shiftLeftPressed) {  // 左Shift
    shiftLeftPressed = true;
    fetch(`/rig_key?shiftState=pressed&shiftKey=left`);
  } else if (event.location === 1 && !shiftRightPressed) {  // 右Shift
    shiftRightPressed = true;
    fetch(`/rig_key?shiftState=pressed&shiftKey=right`);
  }
}

</script>
</body></html>
)rawliteral";

static void web_heap_point(const char *tag)
{
  if (!lowmem_trace) return;
  webLog.printf("[MEM] %-22s internal=%u largest=%u min=%u psram=%u\n",
                  tag,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}



namespace {
constexpr int WEB_BANDMAP_BANDS = N_BAND - 1;
constexpr int WEB_BANDMAP_MAX_ENTRIES = 200;
constexpr int WEB_BANDMAP_NO_PSRAM_MAX_ENTRIES = 20;
constexpr int WEB_BANDMAP_COLUMNS = 4;
constexpr uint32_t WEB_BANDMAP_REFRESH_MS = 1000;
constexpr uint8_t WEB_BANDMAP_COMMAND_QUEUE_LEN = 8;

struct WebBandmapEntry {
  uint32_t freq;
  int32_t time;
  char station[LEN_CALLSIGN + 1];
  uint8_t mode;
  uint8_t flag;
};

struct WebBandmapSnapshot {
  uint16_t count[WEB_BANDMAP_BANDS];
  WebBandmapEntry *entry;
  uint16_t capacity_per_band;
  uint32_t band_generation[WEB_BANDMAP_BANDS];
  uint32_t generation;
  uint8_t sort_type;
};

enum WebBandmapCommandType : uint8_t {
  WEB_BANDMAP_COMMAND_SELECT = 0,
  WEB_BANDMAP_COMMAND_DELETE,
  WEB_BANDMAP_COMMAND_CORRECT
};

struct WebBandmapCommand {
  WebBandmapCommandType type;
  uint8_t bandid;
  uint8_t mode;
  uint32_t freq;
  int32_t time;
  char station[LEN_CALLSIGN + 1];
  char new_station[LEN_CALLSIGN + 1];
};

static WebBandmapSnapshot *web_bandmap_snapshots[2] = {nullptr, nullptr};
static uint8_t web_bandmap_snapshot_count = 0;
static bool web_bandmap_has_psram = false;
static bool web_bandmap_memory_mode_logged = false;
static volatile uint8_t web_bandmap_active_snapshot = 0;
static volatile uint32_t web_bandmap_published_generation = 0;
static volatile bool web_bandmap_snapshot_ready = false;
static SemaphoreHandle_t web_bandmap_snapshot_mutex = nullptr;
static uint32_t web_bandmap_next_refresh_ms = 0;

static WebBandmapCommand web_bandmap_command_queue[WEB_BANDMAP_COMMAND_QUEUE_LEN];
static volatile uint8_t web_bandmap_command_head = 0;
static volatile uint8_t web_bandmap_command_tail = 0;
static portMUX_TYPE web_bandmap_command_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t web_bandmap_hash_mix(uint32_t hash, uint32_t value) {
  hash ^= value;
  hash *= 16777619UL;
  return hash;
}

static bool web_bandmap_entry_less(const WebBandmapEntry &a,
                                   const WebBandmapEntry &b,
                                   uint8_t sort_type) {
  if (sort_type == 1) {
    if (a.freq != b.freq) return a.freq < b.freq;
    return a.time > b.time;
  }

  const int32_t dt = b.time - a.time;
  if (abs(dt) < 60) {
    const bool a_multi = (a.flag & BANDMAP_ENTRY_FLAG_NEWMULTI) != 0;
    const bool b_multi = (b.flag & BANDMAP_ENTRY_FLAG_NEWMULTI) != 0;
    if (a_multi != b_multi) return a_multi;
  }
  if (a.time != b.time) return a.time > b.time;
  return a.freq < b.freq;
}

static WebBandmapEntry *web_bandmap_entry_at(WebBandmapSnapshot *snapshot,
                                                int band_index,
                                                int entry_index) {
  return snapshot->entry +
    static_cast<size_t>(band_index) * snapshot->capacity_per_band + entry_index;
}

static void *web_bandmap_alloc(size_t size, bool prefer_psram) {
  if (prefer_psram) {
    return heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  return heap_caps_calloc(1, size, MALLOC_CAP_8BIT);
}

static void web_bandmap_heap_trace(const char *tag) {
  if (!lowmem_trace) return;
  #if BANDMAP_TRACE
  webLog.printf("[BANDMAPTRACE] web %-22s free=%u largest=%u min=%u snapshots=%u\n",
                tag,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)web_bandmap_snapshot_count);
  #endif
}

static void free_web_bandmap_snapshots() {
  for (uint8_t i = 0; i < web_bandmap_snapshot_count; ++i) {
    if (!web_bandmap_snapshots[i]) continue;
    free(web_bandmap_snapshots[i]->entry);
    free(web_bandmap_snapshots[i]);
    web_bandmap_snapshots[i] = nullptr;
  }
  web_bandmap_snapshot_count = 0;
}

static bool allocate_web_bandmap_snapshots(bool use_psram,
                                            uint16_t capacity) {
  constexpr uint8_t required_count = 2;
  for (uint8_t i = 0; i < required_count; ++i) {
    WebBandmapSnapshot *snapshot = static_cast<WebBandmapSnapshot *>(
      web_bandmap_alloc(sizeof(WebBandmapSnapshot), use_psram));
    web_bandmap_heap_trace("after snapshot alloc");
    if (!snapshot) break;

    const size_t entries_size = sizeof(WebBandmapEntry) *
      static_cast<size_t>(WEB_BANDMAP_BANDS) * capacity;
    snapshot->entry = static_cast<WebBandmapEntry *>(
      web_bandmap_alloc(entries_size, use_psram));
    web_bandmap_heap_trace("after entries alloc");
    if (!snapshot->entry) {
      free(snapshot);
      break;
    }

    snapshot->capacity_per_band = capacity;
    web_bandmap_snapshots[i] = snapshot;
    ++web_bandmap_snapshot_count;
  }

  if (web_bandmap_snapshot_count == required_count) return true;
  free_web_bandmap_snapshots();
  return false;
}

static bool ensure_web_bandmap_snapshots() {
  web_bandmap_heap_trace("ensure enter");
  if (!web_bandmap_snapshot_mutex) {
    web_bandmap_snapshot_mutex = xSemaphoreCreateMutex();
    web_bandmap_heap_trace("after mutex create");
    if (!web_bandmap_snapshot_mutex) return false;
  }
  if (web_bandmap_snapshot_count == 2) {
    web_bandmap_heap_trace("ensure already ready");
    return true;
  }

  // ESP.getPsramSize() can report zero even when ESP-IDF has already added
  // PSRAM to the capability heap.  The heap capability is the authoritative
  // test for allocations made with MALLOC_CAP_SPIRAM.
  const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  web_bandmap_has_psram = psram_total > 0;

  if (lowmem_trace) {
    #if BANDMAP_TRACE
    webLog.printf("[BANDMAPTRACE] layout snapshot=%u entry=%u bands=%u psram_total=%u psram_free=%u\n",
                  (unsigned)sizeof(WebBandmapSnapshot),
                  (unsigned)sizeof(WebBandmapEntry),
                  (unsigned)WEB_BANDMAP_BANDS,
                  (unsigned)psram_total,
                  (unsigned)psram_free);
    #endif
  }

  bool allocated = false;
  uint16_t capacity = WEB_BANDMAP_NO_PSRAM_MAX_ENTRIES;
  const char *memory_name = "internal RAM";

  if (web_bandmap_has_psram) {
    capacity = WEB_BANDMAP_MAX_ENTRIES;
    allocated = allocate_web_bandmap_snapshots(true, capacity);
    if (allocated) memory_name = "PSRAM";
  }

  // The time-sliced builder requires an inactive snapshot.  If the large
  // PSRAM pair cannot be allocated, retain functionality with two compact
  // internal-RAM snapshots instead of falling back to one unusable snapshot.
  if (!allocated) {
    capacity = WEB_BANDMAP_NO_PSRAM_MAX_ENTRIES;
    allocated = allocate_web_bandmap_snapshots(false, capacity);
    memory_name = "internal RAM";
    web_bandmap_has_psram = false;
  }

  if (!allocated) {
    static uint32_t last_error_ms = 0;
    const uint32_t now = millis();
    if (now - last_error_ms >= 5000U) {
      last_error_ms = now;
      webLog.printf("bandmap: two-snapshot allocation failed psram_total=%u psram_free=%u free_internal=%u largest_internal=%u\n",
                    (unsigned)psram_total,
                    (unsigned)psram_free,
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                      MALLOC_CAP_8BIT),
                    (unsigned)heap_caps_get_largest_free_block(
                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    return false;
  }

  if (!web_bandmap_memory_mode_logged) {
    web_bandmap_memory_mode_logged = true;
    webLog.printf("bandmap: snapshots=%u entries_per_band=%u memory=%s\n",
                  web_bandmap_snapshot_count, capacity, memory_name);
  }
  web_bandmap_heap_trace("ensure success");
  return true;
}

static constexpr uint32_t WEB_BANDMAP_JOB_BUDGET_US = 3000U;
static constexpr uint8_t WEB_BANDMAP_JOB_MAX_ENTRIES = 4;

enum WebBandmapJobState : uint8_t {
  WEB_BANDMAP_JOB_IDLE = 0,
  WEB_BANDMAP_JOB_PREPARE,
  WEB_BANDMAP_JOB_SCAN,
  WEB_BANDMAP_JOB_WAIT_DUPE,
  WEB_BANDMAP_JOB_FINISH_BAND,
  WEB_BANDMAP_JOB_PUBLISH
};

struct WebBandmapBuildJob {
  WebBandmapJobState state = WEB_BANDMAP_JOB_IDLE;
  uint8_t snapshot_index = 0;
  uint8_t band_index = 0;
  uint16_t source_index = 0;
  uint16_t output_count = 0;
  uint32_t hash = 2166136261UL;
  WebBandmapEntry pending{};
  uint16_t pending_source_index = 0;
};

static WebBandmapBuildJob web_bandmap_job;

static bool web_bandmap_source_still_matches(int band_index, int source_index,
                                              const WebBandmapEntry &entry) {
  if (band_index < 0 || band_index >= WEB_BANDMAP_BANDS) return false;
  if (source_index < 0 || source_index >= bandmap[band_index].nentry)
    return false;
  const struct bandmap_entry *source = bandmap[band_index].entry + source_index;
  return source->freq == entry.freq && source->time == entry.time &&
         source->mode == entry.mode &&
         strcasecmp(source->station, entry.station) == 0;
}

static void web_bandmap_store_pending(bool worked) {
  WebBandmapSnapshot *snapshot =
      web_bandmap_snapshots[web_bandmap_job.snapshot_index];
  if (web_bandmap_source_still_matches(web_bandmap_job.band_index,
                                        web_bandmap_job.pending_source_index,
                                        web_bandmap_job.pending)) {
    struct bandmap_entry *source =
        bandmap[web_bandmap_job.band_index].entry +
        web_bandmap_job.pending_source_index;
    if (worked) source->flag |= BANDMAP_ENTRY_FLAG_WORKED;
    else source->flag &= ~BANDMAP_ENTRY_FLAG_WORKED;
  }
  if (worked || web_bandmap_job.output_count >= snapshot->capacity_per_band)
    return;

  web_bandmap_job.pending.flag &= ~BANDMAP_ENTRY_FLAG_WORKED;
  *web_bandmap_entry_at(snapshot, web_bandmap_job.band_index,
                        web_bandmap_job.output_count++) =
      web_bandmap_job.pending;
}

static void web_bandmap_finish_current_band() {
  WebBandmapSnapshot *snapshot =
      web_bandmap_snapshots[web_bandmap_job.snapshot_index];
  const int bandid = web_bandmap_job.band_index + 1;
  snapshot->count[web_bandmap_job.band_index] = web_bandmap_job.output_count;
  WebBandmapEntry *entries =
      web_bandmap_entry_at(snapshot, web_bandmap_job.band_index, 0);
  std::sort(entries, entries + web_bandmap_job.output_count,
            [snapshot](const WebBandmapEntry &a, const WebBandmapEntry &b) {
              return web_bandmap_entry_less(a, b, snapshot->sort_type);
            });

  uint32_t band_hash = 2166136261UL;
  band_hash = web_bandmap_hash_mix(band_hash, snapshot->sort_type);
  band_hash = web_bandmap_hash_mix(band_hash, bandid);
  band_hash = web_bandmap_hash_mix(band_hash, web_bandmap_job.output_count);
  for (uint16_t i = 0; i < web_bandmap_job.output_count; ++i) {
    const WebBandmapEntry &entry = entries[i];
    band_hash = web_bandmap_hash_mix(band_hash, entry.freq);
    band_hash = web_bandmap_hash_mix(band_hash,
                                     static_cast<uint32_t>(entry.time));
    band_hash = web_bandmap_hash_mix(band_hash, entry.mode);
    band_hash = web_bandmap_hash_mix(band_hash, entry.flag);
    for (const char *q = entry.station; *q; ++q)
      band_hash = web_bandmap_hash_mix(band_hash,
                                       static_cast<uint8_t>(*q));
  }
  snapshot->band_generation[web_bandmap_job.band_index] = band_hash;
  web_bandmap_job.hash = web_bandmap_hash_mix(web_bandmap_job.hash, band_hash);

  ++web_bandmap_job.band_index;
  web_bandmap_job.source_index = 0;
  web_bandmap_job.output_count = 0;
  web_bandmap_job.state = web_bandmap_job.band_index < WEB_BANDMAP_BANDS
      ? WEB_BANDMAP_JOB_SCAN : WEB_BANDMAP_JOB_PUBLISH;
}

static bool start_web_bandmap_snapshot_job() {
  web_bandmap_heap_trace("job start");
  if (!ensure_web_bandmap_snapshots()) return false;

  // Time-sliced asynchronous DUPE checking requires an inactive snapshot.
  // The normal PSRAM configuration has two snapshots.  Retain the old
  // synchronous rebuild as a fallback would defeat the loop-time guarantee,
  // so low-memory/single-snapshot configurations simply defer the refresh.
  if (web_bandmap_snapshot_count < 2) {
    static uint32_t last_warning_ms = 0;
    if (millis() - last_warning_ms >= 5000U) {
      last_warning_ms = millis();
      webLog.println("bandmap: time-sliced rebuild requires two snapshots");
    }
    return false;
  }

  web_bandmap_job = WebBandmapBuildJob{};
  web_bandmap_job.state = WEB_BANDMAP_JOB_PREPARE;
  web_bandmap_job.snapshot_index = web_bandmap_active_snapshot ^ 1U;
  web_bandmap_job.hash = 2166136261UL;
  return true;
}

static void process_web_bandmap_snapshot_job() {
  if (web_bandmap_job.state == WEB_BANDMAP_JOB_IDLE) return;
  const uint32_t started_us = micros();
  uint8_t entries_processed = 0;

  while (web_bandmap_job.state != WEB_BANDMAP_JOB_IDLE &&
         entries_processed < WEB_BANDMAP_JOB_MAX_ENTRIES &&
         (uint32_t)(micros() - started_us) < WEB_BANDMAP_JOB_BUDGET_US) {
    WebBandmapSnapshot *snapshot =
        web_bandmap_snapshots[web_bandmap_job.snapshot_index];

    switch (web_bandmap_job.state) {
    case WEB_BANDMAP_JOB_PREPARE:
      memset(snapshot->count, 0, sizeof(snapshot->count));
      memset(snapshot->band_generation, 0, sizeof(snapshot->band_generation));
      snapshot->generation = 0;
      snapshot->sort_type = bandmap_disp.sort_type;
      web_bandmap_job.hash = web_bandmap_hash_mix(
          2166136261UL, snapshot->sort_type);
      web_bandmap_job.state = WEB_BANDMAP_JOB_SCAN;
      break;

    case WEB_BANDMAP_JOB_SCAN: {
      if (web_bandmap_job.band_index >= WEB_BANDMAP_BANDS) {
        web_bandmap_job.state = WEB_BANDMAP_JOB_PUBLISH;
        break;
      }
      if (web_bandmap_job.source_index >=
              bandmap[web_bandmap_job.band_index].nentry ||
          web_bandmap_job.output_count >= snapshot->capacity_per_band) {
        web_bandmap_job.state = WEB_BANDMAP_JOB_FINISH_BAND;
        break;
      }

      const uint16_t source_index = web_bandmap_job.source_index++;
      struct bandmap_entry *source =
          bandmap[web_bandmap_job.band_index].entry + source_index;
      ++entries_processed;
      if (source->station[0] == '\0' || source->mode >= NMODEID) break;

      web_bandmap_job.pending = WebBandmapEntry{};
      web_bandmap_job.pending.freq = source->freq;
      web_bandmap_job.pending.time = source->time;
      strlcpy(web_bandmap_job.pending.station, source->station,
              sizeof(web_bandmap_job.pending.station));
      web_bandmap_job.pending.mode = source->mode;
      web_bandmap_job.pending.flag = source->flag;
      web_bandmap_job.pending_source_index = source_index;

      const int bandid = web_bandmap_job.band_index + 1;
      const byte bm = bandmode_param(bandid, modetype[source->mode]);
      if (dupechk->dupechk_at == 1) {
        if (!dupechk_background_exact_start(source->station, bm, plogw->mask)) {
          // The single query slot belongs to operator work.  Retry this same
          // entry on a later loop rather than skipping its DUPE result.
          --web_bandmap_job.source_index;
          return;
        }
        web_bandmap_job.state = WEB_BANDMAP_JOB_WAIT_DUPE;
        return;
      }

      web_bandmap_store_pending(
          dupe_check_nocallhist(source->station, bm, plogw->mask));
      break;
    }

    case WEB_BANDMAP_JOB_WAIT_DUPE: {
      bool confirmed = false;
      bool worked = false;
      if (!dupechk_background_exact_poll(&confirmed, &worked)) return;
      // On timeout or operator preemption, retry the same entry in the next
      // refresh.  Keep it visible now instead of stalling the current build.
      web_bandmap_store_pending(confirmed && worked);
      web_bandmap_job.state = WEB_BANDMAP_JOB_SCAN;
      break;
    }

    case WEB_BANDMAP_JOB_FINISH_BAND:
      web_bandmap_finish_current_band();
      break;

    case WEB_BANDMAP_JOB_PUBLISH:
      snapshot->generation = web_bandmap_job.hash;
      if (xSemaphoreTake(web_bandmap_snapshot_mutex,
                         pdMS_TO_TICKS(5)) != pdTRUE)
        return;
      web_bandmap_active_snapshot = web_bandmap_job.snapshot_index;
      web_bandmap_published_generation = snapshot->generation;
      web_bandmap_snapshot_ready = true;
      xSemaphoreGive(web_bandmap_snapshot_mutex);
      web_bandmap_job.state = WEB_BANDMAP_JOB_IDLE;
      web_bandmap_heap_trace("job publish");
      break;

    default:
      dupechk_background_exact_cancel();
      web_bandmap_job.state = WEB_BANDMAP_JOB_IDLE;
      break;
    }
  }
}

static bool enqueue_web_bandmap_command(const WebBandmapCommand &command) {
  bool queued = false;
  portENTER_CRITICAL(&web_bandmap_command_mux);
  const uint8_t next = static_cast<uint8_t>(
    (web_bandmap_command_head + 1) % WEB_BANDMAP_COMMAND_QUEUE_LEN);
  if (next != web_bandmap_command_tail) {
    web_bandmap_command_queue[web_bandmap_command_head] = command;
    web_bandmap_command_head = next;
    queued = true;
  }
  portEXIT_CRITICAL(&web_bandmap_command_mux);
  return queued;
}

static bool dequeue_web_bandmap_command(WebBandmapCommand *command) {
  bool available = false;
  portENTER_CRITICAL(&web_bandmap_command_mux);
  if (web_bandmap_command_tail != web_bandmap_command_head) {
    *command = web_bandmap_command_queue[web_bandmap_command_tail];
    web_bandmap_command_tail = static_cast<uint8_t>(
      (web_bandmap_command_tail + 1) % WEB_BANDMAP_COMMAND_QUEUE_LEN);
    available = true;
  }
  portEXIT_CRITICAL(&web_bandmap_command_mux);
  return available;
}

static bool valid_web_bandmap_callsign(const char *station) {
  if (!station || !station[0]) return false;
  const size_t length = strlen(station);
  if (length > LEN_CALLSIGN) return false;
  bool has_alnum = false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(station[i]);
    if (isalnum(c)) has_alnum = true;
    else if (c != '/') return false;
  }
  return has_alnum;
}

static void update_web_bandmap_entry_flags(struct bandmap_entry *entry,
                                           uint8_t bandid) {
  if (!entry || entry->mode >= NMODEID) return;
  // WORKED is monotonic during a contest.  Preserve it across refreshes;
  // a transient DUPE-query timeout must not expose the spot again.
  entry->flag &= ~BANDMAP_ENTRY_FLAG_NEWMULTI;

  char *exch_history = nullptr;
  const int bandmode = bandmode_param(bandid, modetype[entry->mode]);
  if (dupe_callhist_check(entry->station, bandmode, plogw->mask, 1,
                          &exch_history)) {
    entry->flag |= BANDMAP_ENTRY_FLAG_WORKED;
  }
  if (exch_history) {
    const int multi = multi_check(exch_history, bandid);
    if (multi >= 0 && !multi_worked_get(&multi_list, bandid - 1, multi)) {
      entry->flag |= BANDMAP_ENTRY_FLAG_NEWMULTI;
    }
  }
}

static struct bandmap_entry *find_web_bandmap_entry(
    const WebBandmapCommand &command) {
  if (command.bandid < 1 || command.bandid >= N_BAND) return nullptr;
  const int band_index = command.bandid - 1;
  for (int i = 0; i < bandmap[band_index].nentry; ++i) {
    struct bandmap_entry *entry = bandmap[band_index].entry + i;
    if (entry->station[0] == '\0') continue;
    if (entry->freq == command.freq &&
        entry->mode == command.mode &&
        entry->time == command.time &&
        strcasecmp(entry->station, command.station) == 0) {
      return entry;
    }
  }
  return nullptr;
}

static void process_web_bandmap_command_queue() {
  WebBandmapCommand command;
  while (dequeue_web_bandmap_command(&command)) {
    struct bandmap_entry *entry = find_web_bandmap_entry(command);
    if (!entry) {
      webLog.printf("bandmap: requested spot no longer exists: %s\n",
                    command.station);
      continue;
    }

    if (command.type == WEB_BANDMAP_COMMAND_DELETE) {
      char old_station[LEN_CALLSIGN + 1];
      strlcpy(old_station, entry->station, sizeof(old_station));
      entry->station[0] = '\0';
      webLog.printf("bandmap: deleted %s %lu\n", old_station,
                    static_cast<unsigned long>(entry->freq));
      web_bandmap_next_refresh_ms = 0;
      continue;
    }

    if (command.type == WEB_BANDMAP_COMMAND_CORRECT) {
      if (!valid_web_bandmap_callsign(command.new_station)) {
        webLog.println("bandmap: invalid corrected callsign");
        continue;
      }
      char old_station[LEN_CALLSIGN + 1];
      strlcpy(old_station, entry->station, sizeof(old_station));
      strlcpy(entry->station, command.new_station, sizeof(entry->station));
      update_web_bandmap_entry_flags(entry, command.bandid);
      webLog.printf("bandmap: corrected %s -> %s\n", old_station,
                    entry->station);
      web_bandmap_next_refresh_ms = 0;
      continue;
    }

    update_web_bandmap_entry_flags(entry, command.bandid);
    if (entry->flag & BANDMAP_ENTRY_FLAG_WORKED) {
      webLog.printf("bandmap: %s is already worked\n", entry->station);
      web_bandmap_next_refresh_ms = 0;
      continue;
    }
    if (!select_appropriate_radio(command.bandid)) {
      webLog.printf("bandmap: no appropriate radio for band %u\n",
                    command.bandid);
      continue;
    }

    struct radio *radio = so2r.radio_selected();
    set_station_entry(radio, entry->station, entry->freq,
                      mode_str[entry->mode]);
    webLog.printf("bandmap: selected %s %lu\n", entry->station,
                  static_cast<unsigned long>(entry->freq));
  }
}

static const char web_bandmap_page[] PROGMEM = R"rawliteral(
<!doctype html><html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DVPlogger Bandmap</title>
<style>
body{font-family:sans-serif;margin:10px;background:#f4f4f4;color:#111}
h1{font-size:1.35rem;margin:.3rem 0 .35rem}.nav-links{display:flex;gap:1rem;align-items:center;margin:0 0 .8rem}.nav-links a{white-space:nowrap}.toolbar{display:flex;gap:.5rem;flex-wrap:wrap;margin-bottom:.7rem}
select,button{font-size:1rem;padding:.35rem}.maps{display:flex;gap:10px;overflow-x:auto;align-items:flex-start;padding-bottom:8px}
.band{flex:0 0 285px;background:#fff;border:1px solid #bbb;border-radius:5px;max-height:78vh;overflow-y:auto}
.band h2{position:sticky;top:0;background:#e8e8e8;margin:0;padding:.45rem;font-size:1.05rem;border-bottom:1px solid #bbb;z-index:1}
.spot{display:grid;grid-template-columns:78px 1fr 42px 34px 30px;gap:4px;padding:.28rem .4rem;border-bottom:1px solid #eee;cursor:pointer}
.spot:hover{background:#fff4c4}.spot.newmulti{background:#fff3a8;border-left:5px solid #d07800;padding-left:calc(.4rem - 5px)}
.spot.newmulti:hover{background:#ffe57a}.freq{font-family:monospace}.call{font-weight:bold}.multi{color:#a00018;font-weight:bold;text-align:center}.age{text-align:right;color:#555}.empty{padding:.8rem;color:#777}
#status{font-size:.85rem;color:#555;margin-left:.3rem}.more{border:0;background:transparent;padding:0;font-size:1.25rem;line-height:1;cursor:pointer}.menu{position:fixed;display:none;z-index:20;background:#fff;border:1px solid #999;border-radius:5px;box-shadow:0 3px 14px #5558;min-width:170px}.menu button{display:block;width:100%;border:0;background:#fff;text-align:left;padding:.65rem}.menu button:hover{background:#eee}.modal{display:none;position:fixed;inset:0;background:#0007;z-index:30;align-items:center;justify-content:center}.dialog{background:#fff;border-radius:7px;padding:1rem;min-width:min(310px,88vw);box-shadow:0 5px 20px #0008}.dialog h3{margin:.1rem 0 .8rem}.dialog input{box-sizing:border-box;width:100%;font-size:1.1rem;padding:.45rem;text-transform:uppercase}.actions{display:flex;justify-content:flex-end;gap:.6rem;margin-top:1rem}.danger{color:#a00018;font-weight:bold}
</style></head><body><h1>DVPlogger Bandmap</h1>
<nav class="nav-links"><a href="/">Home</a><a href="/contests">Contest</a></nav>
<div class="toolbar"><select id="b0"></select><select id="b1"></select><select id="b2"></select><select id="b3"></select>
<button id="reload">更新</button><span id="status"></span></div><div id="maps" class="maps"></div><div id="spotMenu" class="menu"><button id="menuDelete" class="danger">Delete Spot</button><button id="menuCorrect">Callsign correction</button></div><div id="spotModal" class="modal"><div class="dialog"><h3 id="modalTitle"></h3><div id="modalText"></div><input id="callInput" maxlength="12" autocomplete="off" autocapitalize="characters"><div class="actions"><button id="modalCancel">Cancel</button><button id="modalApply">Apply</button></div></div></div>
<script>
const defaults=[3,4,5,6];let bandGeneration={};let loadingBands=new Set();let menuSpot=null;let modalAction=null;
function esc(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
function selected(){return [0,1,2,3].map(i=>Number(document.getElementById('b'+i).value));}
function save(){localStorage.setItem('dvploggerBandmapBands',JSON.stringify(selected()));}
function sleep(ms){return new Promise(resolve=>setTimeout(resolve,ms));}
async function fetchJson(url,retry=true){try{const r=await fetch(url,{cache:'no-store'});const text=await r.text();
if(!r.ok)throw new Error('HTTP '+r.status+': '+text.slice(0,80));
try{return JSON.parse(text);}catch(e){console.error('bandmap JSON parse error',e,'length',text.length,'tail',text.slice(-160));throw new Error('JSON不正 len='+text.length);}}
catch(e){if(retry){await sleep(300);return fetchJson(url,false);}throw e;}}
function columnsForBand(bandid){const cols=[];for(let i=0;i<4;i++)if(Number(document.getElementById('b'+i).value)===Number(bandid))cols.push(i);return cols;}
function ensureColumns(){const maps=document.getElementById('maps');while(maps.children.length<4){const col=document.createElement('section');col.className='band';col.innerHTML='<h2>Loading...</h2>';maps.appendChild(col);}}
function renderBand(b,columns){ensureColumns();for(const column of columns){const col=document.getElementById('maps').children[column];const oldScroll=col.scrollTop;
let h='<h2>'+esc(b.label)+' ('+b.spots.length+')</h2>';if(!b.spots.length)h+='<div class="empty">未交信スポットなし</div>';
for(const s of b.spots){const cls=s.multi?'spot newmulti':'spot';h+='<div class="'+cls+'" data-band="'+b.id+'" data-freq="'+s.freq+'" data-mode="'+s.mode+'" data-time="'+s.time+'" data-call="'+esc(s.call)+'">'+
'<span class="freq">'+esc(s.freqText)+'</span><span class="call">'+esc(s.call)+'</span><span class="multi">'+(s.multi?'M':'')+'</span><span class="age">'+s.age+'m</span><button class="more" aria-label="Spot menu">⋮</button></div>';}
col.innerHTML=h;col.scrollTop=oldScroll;col.querySelectorAll('.spot').forEach(e=>{e.onclick=()=>selectSpot(e);const m=e.querySelector('.more');m.onclick=ev=>{ev.stopPropagation();openSpotMenu(e,m);};});}}
async function loadBand(bandid,force=false){bandid=Number(bandid);if(loadingBands.has(bandid))return;loadingBands.add(bandid);
try{const j=await fetchJson('/api/bandmap/data?band='+bandid);if(!j.bands||!j.bands.length)throw new Error('band data missing');
bandGeneration[bandid]=Number(j.bandGeneration);renderBand(j.bands[0],columnsForBand(bandid));}
catch(e){console.error('bandmap band load failed',bandid,e);document.getElementById('status').textContent='取得失敗: '+e.message;}finally{loadingBands.delete(bandid);}}
async function loadSelected(force=false){const unique=[...new Set(selected())];document.getElementById('status').textContent='更新中…';await Promise.all(unique.map(b=>loadBand(b,force)));document.getElementById('status').textContent='更新 '+new Date().toLocaleTimeString();}
async function loadBands(){const j=await fetchJson('/api/bandmap/bands');let saved;try{saved=JSON.parse(localStorage.getItem('dvploggerBandmapBands'));}catch(e){}
const valid=new Set(j.bands.map(b=>Number(b.id)));if(!Array.isArray(saved)||saved.length!==4)saved=defaults.slice();
saved=saved.map((v,n)=>{v=Number(v);return valid.has(v)?v:(valid.has(defaults[n])?defaults[n]:Number(j.bands[0].id));});
for(let n=0;n<4;n++){const s=document.getElementById('b'+n);s.innerHTML='';for(const b of j.bands){const o=document.createElement('option');o.value=String(b.id);o.textContent=b.label;s.appendChild(o);}s.value=String(saved[n]);s.onchange=()=>{save();loadBand(Number(s.value),true);};}save();ensureColumns();}
async function selectSpot(e){const p=new URLSearchParams({band:e.dataset.band,freq:e.dataset.freq,mode:e.dataset.mode,time:e.dataset.time,call:e.dataset.call});
const r=await fetch('/api/bandmap/select',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});document.getElementById('status').textContent=await r.text();}
function spotParams(e){return {band:e.dataset.band,freq:e.dataset.freq,mode:e.dataset.mode,time:e.dataset.time,call:e.dataset.call};}
function closeSpotMenu(){document.getElementById('spotMenu').style.display='none';menuSpot=null;}
function openSpotMenu(spot,button){menuSpot=spot;const menu=document.getElementById('spotMenu');const r=button.getBoundingClientRect();menu.style.left=Math.max(6,Math.min(window.innerWidth-180,r.right-170))+'px';menu.style.top=Math.min(window.innerHeight-110,r.bottom+3)+'px';menu.style.display='block';}
function closeModal(){document.getElementById('spotModal').style.display='none';modalAction=null;}
function showDeleteDialog(){if(!menuSpot)return;const p=spotParams(menuSpot);closeSpotMenu();menuSpot={dataset:p};modalAction='delete';document.getElementById('modalTitle').textContent='Delete Spot';document.getElementById('modalText').textContent=p.call+'  '+p.freq;document.getElementById('callInput').style.display='none';document.getElementById('modalApply').textContent='Delete';document.getElementById('modalApply').className='danger';document.getElementById('spotModal').style.display='flex';}
function showCorrectDialog(){if(!menuSpot)return;const p=spotParams(menuSpot);closeSpotMenu();menuSpot={dataset:p};modalAction='correct';document.getElementById('modalTitle').textContent='Callsign correction';document.getElementById('modalText').textContent='Current: '+p.call;const input=document.getElementById('callInput');input.style.display='block';input.value=p.call;document.getElementById('modalApply').textContent='Apply';document.getElementById('modalApply').className='';document.getElementById('spotModal').style.display='flex';setTimeout(()=>{input.focus();input.select();},0);}
async function postSpotAction(path,extra={}){if(!menuSpot)return;const p=new URLSearchParams({...spotParams(menuSpot),...extra});const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});const text=await r.text();if(!r.ok)throw new Error('HTTP '+r.status+': '+text);document.getElementById('status').textContent=text;}
async function applyModal(){try{if(modalAction==='delete')await postSpotAction('/api/bandmap/delete');else if(modalAction==='correct'){const v=document.getElementById('callInput').value.trim().toUpperCase();if(!/^[A-Z0-9/]+$/.test(v))throw new Error('Invalid callsign');await postSpotAction('/api/bandmap/correct',{newcall:v});}closeModal();}catch(e){document.getElementById('status').textContent='操作失敗: '+e.message;}}
document.getElementById('menuDelete').onclick=showDeleteDialog;document.getElementById('menuCorrect').onclick=showCorrectDialog;document.getElementById('modalCancel').onclick=closeModal;document.getElementById('modalApply').onclick=applyModal;document.getElementById('spotModal').onclick=e=>{if(e.target.id==='spotModal')closeModal();};document.addEventListener('click',e=>{if(!e.target.closest('#spotMenu')&&!e.target.closest('.more'))closeSpotMenu();});
async function checkVersion(){try{const j=await fetchJson('/api/bandmap/version',false);if(!j.ready)return;for(const b of [...new Set(selected())]){const next=Number(j.bands[String(b)]);if(Number.isFinite(next)&&bandGeneration[b]!==undefined&&next!==bandGeneration[b])loadBand(b);}}catch(e){console.debug('bandmap version check failed',e);}}
document.getElementById('reload').onclick=()=>loadSelected(true);(async()=>{await loadBands();await loadSelected(true);setInterval(checkVersion,1000);})();
</script></body></html>)rawliteral";

static constexpr uint16_t WEB_BANDMAP_MAX_DISPLAY_ENTRIES = 20;

struct WebBandmapApiState {
  struct BandData {
    uint8_t bandid;
    uint16_t count;
    WebBandmapEntry *entries;
  } band[WEB_BANDMAP_COLUMNS]{};
  uint8_t band_count = 0;
  uint8_t stage = 0;
  uint8_t band_index = 0;
  uint16_t entry_index = 0;
  size_t offset = 0;
  uint32_t generation = 0;
  char line[256];

  ~WebBandmapApiState() {
    for (int i = 0; i < WEB_BANDMAP_COLUMNS; ++i) free(band[i].entries);
  }
};

static uint8_t parse_web_bandmap_band(AsyncWebServerRequest *request) {
  int bandid = 3;
  if (request->hasParam("band")) {
    bandid = request->getParam("band")->value().toInt();
  }
  if (bandid < 1 || bandid >= N_BAND) bandid = 3;
  return static_cast<uint8_t>(bandid);
}

static WebBandmapApiState *make_web_bandmap_api_state(uint8_t bandid) {
  if (!ensure_web_bandmap_snapshots()) return nullptr;
  WebBandmapApiState *state = new (std::nothrow) WebBandmapApiState;
  if (!state) return nullptr;

  if (!web_bandmap_snapshot_ready ||
      xSemaphoreTake(web_bandmap_snapshot_mutex, portMAX_DELAY) != pdTRUE) {
    delete state;
    return nullptr;
  }
  const uint8_t active = web_bandmap_active_snapshot;
  WebBandmapSnapshot *snapshot = web_bandmap_snapshots[active];
  state->generation = snapshot->band_generation[bandid - 1];
  const uint16_t count = std::min<uint16_t>(
    snapshot->count[bandid - 1], WEB_BANDMAP_MAX_DISPLAY_ENTRIES);
  state->band[0].bandid = bandid;
  state->band[0].count = count;
  if (count) {
    state->band[0].entries = static_cast<WebBandmapEntry *>(
      heap_caps_malloc(sizeof(WebBandmapEntry) * count,
                       web_bandmap_has_psram
                         ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                         : MALLOC_CAP_8BIT));
    if (!state->band[0].entries) {
      xSemaphoreGive(web_bandmap_snapshot_mutex);
      delete state;
      return nullptr;
    }
    memcpy(state->band[0].entries,
           web_bandmap_entry_at(snapshot, bandid - 1, 0),
           sizeof(WebBandmapEntry) * count);
  }
  xSemaphoreGive(web_bandmap_snapshot_mutex);
  state->band_count = 1;
  return state;
}

static void web_bandmap_json_safe_copy(char *dest, size_t dest_size,
                                       const char *source) {
  if (!dest || dest_size == 0) return;
  size_t out = 0;
  if (!source) source = "";
  while (*source && out + 1 < dest_size) {
    const unsigned char c = static_cast<unsigned char>(*source++);
    // Callsigns and band labels should be printable ASCII.  Replace JSON
    // metacharacters/control bytes rather than allowing malformed JSON.
    if (c < 0x20 || c == '"' || c == '\\') dest[out++] = '?';
    else dest[out++] = static_cast<char>(c);
  }
  dest[out] = '\0';
}

static void add_bandmap_api_headers(AsyncWebServerResponse *response) {
  if (!response) return;
  response->addHeader("Cache-Control", "no-store");
  response->addHeader("Access-Control-Allow-Origin", "*");
}

static void send_bandmap_api_text(AsyncWebServerRequest *request, int code,
                                  const char *content_type,
                                  const String &body) {
  AsyncWebServerResponse *response =
    request->beginResponse(code, content_type, body);
  add_bandmap_api_headers(response);
  request->send(response);
}

static void setup_web_bandmap_handlers() {
  web_server.on("/bandmap", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse_P(
      200, "text/html; charset=utf-8", web_bandmap_page);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  web_server.on("/api/bandmap/bands", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{\"bands\":[";
    for (int bandid = 1; bandid < N_BAND; ++bandid) {
      if (bandid > 1) json += ',';
      json += "{\"id\":" + String(bandid) + ",\"label\":\"";
      String label = bandid_str[bandid - 1];
      label.trim();
      json += label;
      json += "\"}";
    }
    json += "]}";
    send_bandmap_api_text(request, 200, "application/json", json);
  });

  // Even Hub applications use the same compact band list.
  web_server.on("/api/g2/bands", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{\"bands\":[";
    for (int bandid = 1; bandid < N_BAND; ++bandid) {
      if (bandid > 1) json += ',';
      json += "{\"id\":" + String(bandid) + ",\"label\":\"";
      String label = bandid_str[bandid - 1];
      label.trim();
      json += label;
      json += "\"}";
    }
    json += "]}";
    send_bandmap_api_text(request, 200, "application/json", json);
  });

  web_server.on("/api/bandmap/version", HTTP_GET, [](AsyncWebServerRequest *request) {
    const bool ready = web_bandmap_snapshot_ready;
    char json[768];
    size_t used = snprintf(json, sizeof(json),
                           "{\"ready\":%s,\"bands\":{",
                           ready ? "true" : "false");
    if (ready &&
        xSemaphoreTake(web_bandmap_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
      const WebBandmapSnapshot *snapshot =
        web_bandmap_snapshots[web_bandmap_active_snapshot];
      for (int band_index = 0; band_index < WEB_BANDMAP_BANDS; ++band_index) {
        used += snprintf(json + used, sizeof(json) - used,
                         "%s\"%d\":%lu",
                         band_index ? "," : "", band_index + 1,
                         static_cast<unsigned long>(
                           snapshot->band_generation[band_index]));
      }
      xSemaphoreGive(web_bandmap_snapshot_mutex);
    }
    snprintf(json + used, sizeof(json) - used, "}}");
    send_bandmap_api_text(request, 200, "application/json", String(json));
  });

  web_server.on("/api/bandmap/data", HTTP_GET, [](AsyncWebServerRequest *request) {
    const uint8_t bandid = parse_web_bandmap_band(request);
    WebBandmapApiState *state = make_web_bandmap_api_state(bandid);
    if (!state) {
      request->send(503, "text/plain", "bandmap snapshot unavailable");
      return;
    }

    // A single band is limited to WEB_BANDMAP_MAX_DISPLAY_ENTRIES (20), so
    // build a complete JSON document and send it with Content-Length.  This
    // avoids truncated HTTP-200 JSON caused by the chunk generator state
    // machine while keeping peak memory bounded to a few kilobytes.
    String json;
    size_t reserve_size = 160U +
      static_cast<size_t>(state->band[0].count) * 144U;
    if (reserve_size < 1024U) reserve_size = 1024U;
    if (!json.reserve(reserve_size)) {
      delete state;
      request->send(503, "text/plain", "bandmap JSON allocation failed");
      return;
    }

    json += F("{\"bandGeneration\":");
    json += static_cast<unsigned long>(state->generation);
    json += F(",\"bands\":[{");
    json += F("\"id\":");
    json += state->band[0].bandid;
    json += F(",\"label\":\"");

    String label = bandid_str[state->band[0].bandid - 1];
    label.trim();
    char safe_label[32];
    web_bandmap_json_safe_copy(safe_label, sizeof(safe_label), label.c_str());
    json += safe_label;
    json += F("\",\"spots\":[");

    const int now = my_rtc.unixtime();
    for (uint16_t i = 0; i < state->band[0].count; ++i) {
      const WebBandmapEntry &entry = state->band[0].entries[i];
      const int age = max(0, (now - entry.time) / 60);
      const unsigned long hz =
        static_cast<unsigned long>(entry.freq) * FREQ_UNIT;
      char freq_text[24];
      snprintf(freq_text, sizeof(freq_text), "%lu.%01lu",
               hz / 1000UL, (hz % 1000UL) / 100UL);
      char safe_station[sizeof(entry.station)];
      web_bandmap_json_safe_copy(safe_station, sizeof(safe_station),
                                 entry.station);

      if (i) json += ',';
      json += F("{\"freq\":");
      json += static_cast<unsigned long>(entry.freq);
      json += F(",\"freqText\":\"");
      json += freq_text;
      json += F("\",\"call\":\"");
      json += safe_station;
      json += F("\",\"mode\":");
      json += entry.mode;
      json += F(",\"time\":");
      json += static_cast<long>(entry.time);
      json += F(",\"age\":");
      json += age;
      json += F(",\"multi\":");
      json += (entry.flag & BANDMAP_ENTRY_FLAG_NEWMULTI) ? F("true") : F("false");
      json += '}';
    }
    json += F("]}]}" );

    delete state;
    AsyncWebServerResponse *response =
      request->beginResponse(200, "application/json", json);
    add_bandmap_api_headers(response);
    request->send(response);
  });

  // /api/g2/bandmap is a lightweight alias intended for Even Hub clients.
  web_server.on("/api/g2/bandmap", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->redirect("/api/bandmap/data?band=" +
                      String(parse_web_bandmap_band(request)));
  });

  web_server.on("/api/bandmap/select", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      const char *required[] = {"band", "freq", "mode", "time", "call"};
      for (const char *name : required) {
        if (!request->hasParam(name, true)) {
          request->send(400, "text/plain", "missing parameter");
          return;
        }
      }

      WebBandmapCommand command{};
      command.type = WEB_BANDMAP_COMMAND_SELECT;
      command.bandid = request->getParam("band", true)->value().toInt();
      command.freq = request->getParam("freq", true)->value().toInt();
      command.mode = request->getParam("mode", true)->value().toInt();
      command.time = request->getParam("time", true)->value().toInt();
      strlcpy(command.station,
              request->getParam("call", true)->value().c_str(),
              sizeof(command.station));

      if (command.bandid < 1 || command.bandid >= N_BAND ||
          command.station[0] == '\0'  || command.mode >= NMODEID) {
        request->send(400, "text/plain", "invalid spot");
        return;
      }
      if (!enqueue_web_bandmap_command(command)) {
        request->send(503, "text/plain", "bandmap command queue full");
        return;
      }
      send_bandmap_api_text(request, 202, "text/plain",
                            String("spot selection queued"));
    });

  // G2 uses the same no-confirmation selection queue as the web bandmap.
  web_server.on("/api/g2/select", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      const char *required[] = {"band", "freq", "mode", "time", "call"};
      for (const char *name : required) {
        if (!request->hasParam(name, true)) {
          send_bandmap_api_text(request, 400, "text/plain",
                                String("missing parameter"));
          return;
        }
      }

      WebBandmapCommand command{};
      command.type = WEB_BANDMAP_COMMAND_SELECT;
      command.bandid = request->getParam("band", true)->value().toInt();
      command.freq = request->getParam("freq", true)->value().toInt();
      command.mode = request->getParam("mode", true)->value().toInt();
      command.time = request->getParam("time", true)->value().toInt();
      String call = request->getParam("call", true)->value();
      call.trim();
      call.toUpperCase();
      strlcpy(command.station, call.c_str(), sizeof(command.station));

      if (command.bandid < 1 || command.bandid >= N_BAND ||
          !valid_web_bandmap_callsign(command.station) ||
          command.mode >= NMODEID) {
        send_bandmap_api_text(request, 400, "text/plain",
                              String("invalid spot"));
        return;
      }
      if (!enqueue_web_bandmap_command(command)) {
        send_bandmap_api_text(request, 503, "text/plain",
                              String("bandmap command queue full"));
        return;
      }
      send_bandmap_api_text(request, 202, "text/plain",
                            String("spot selection queued"));
    });

  auto enqueue_edit_command = [](AsyncWebServerRequest *request,
                                 WebBandmapCommandType type) {
    const char *required[] = {"band", "freq", "mode", "time", "call"};
    for (const char *name : required) {
      if (!request->hasParam(name, true)) {
        request->send(400, "text/plain", "missing parameter");
        return;
      }
    }

    WebBandmapCommand command{};
    command.type = type;
    command.bandid = request->getParam("band", true)->value().toInt();
    command.freq = request->getParam("freq", true)->value().toInt();
    command.mode = request->getParam("mode", true)->value().toInt();
    command.time = request->getParam("time", true)->value().toInt();
    String old_call = request->getParam("call", true)->value();
    old_call.trim(); old_call.toUpperCase();
    strlcpy(command.station, old_call.c_str(), sizeof(command.station));

    if (type == WEB_BANDMAP_COMMAND_CORRECT) {
      if (!request->hasParam("newcall", true)) {
        request->send(400, "text/plain", "missing newcall");
        return;
      }
      String new_call = request->getParam("newcall", true)->value();
      new_call.trim(); new_call.toUpperCase();
      strlcpy(command.new_station, new_call.c_str(),
              sizeof(command.new_station));
      if (!valid_web_bandmap_callsign(command.new_station)) {
        request->send(400, "text/plain", "invalid callsign");
        return;
      }
    }

    if (command.bandid < 1 || command.bandid >= N_BAND ||
        command.station[0] == '\0' || command.mode >= NMODEID) {
      request->send(400, "text/plain", "invalid spot");
      return;
    }
    if (!enqueue_web_bandmap_command(command)) {
      request->send(503, "text/plain", "bandmap command queue full");
      return;
    }
    request->send(202, "text/plain",
                  type == WEB_BANDMAP_COMMAND_DELETE
                    ? "spot deletion queued"
                    : "callsign correction queued");
  };

  web_server.on("/api/bandmap/delete", HTTP_POST,
    [enqueue_edit_command](AsyncWebServerRequest *request) {
      enqueue_edit_command(request, WEB_BANDMAP_COMMAND_DELETE);
    });
  web_server.on("/api/bandmap/correct", HTTP_POST,
    [enqueue_edit_command](AsyncWebServerRequest *request) {
      enqueue_edit_command(request, WEB_BANDMAP_COMMAND_CORRECT);
    });
}
}

void process_web_bandmap() {
  if (f_low_memory_mode) return;

  // User commands are handled first.  Snapshot rebuilding then advances only
  // within its per-loop time/entry budget.
  process_web_bandmap_command_queue();
  process_web_bandmap_snapshot_job();

  const uint32_t now = millis();
  if (web_bandmap_job.state == WEB_BANDMAP_JOB_IDLE &&
      (int32_t)(now - web_bandmap_next_refresh_ms) >= 0) {
    if (start_web_bandmap_snapshot_job())
      web_bandmap_next_refresh_ms = now + WEB_BANDMAP_REFRESH_MS;
    else
      web_bandmap_next_refresh_ms = now + 1000U;
  }
}


static bool web_sat_calc_point(int idx, const DateTime &jst_time,
                               double *az, double *el, double *rr) {
  if (idx < 0 || idx >= N_SATELLITES || sat_info[idx].YEAR == 0) return false;
  Plan13 calc;
  calc.setElements(
    sat_info[idx].YEAR, sat_info[idx].EPOCH, sat_info[idx].INCLINATION,
    sat_info[idx].RAAN, sat_info[idx].ECCENTRICITY * ONEPPTM,
    sat_info[idx].ARGUMENT_PEDIGREE, sat_info[idx].MEAN_ANOMALY,
    sat_info[idx].MEAN_MOTION, sat_info[idx].TIME_MOTION_D,
    sat_info[idx].EPOCH_ORBIT, 180);
  calc.setLocation(mh2lon(plogw->grid_locator_set),
                   mh2lat(plogw->grid_locator_set), 50);
  const uint32_t jst_epoch = jst_time.unixtime();
  const uint32_t utc_epoch = jst_epoch >= 9UL * 3600UL
                               ? jst_epoch - 9UL * 3600UL : 0;
  DateTime utc_time(utc_epoch);
  calc.setTime(utc_time.year(), utc_time.month(), utc_time.day(),
               utc_time.hour(), utc_time.minute(), utc_time.second());
  calc.initSat();
  calc.satvec();
  calc.rangevec();
  if (az) *az = calc.AZ;
  if (el) *el = calc.EL;
  if (rr) *rr = calc.RR;
  return true;
}

static void web_sat_format_datetime(const DateTime &t, char *buf, size_t n) {
  snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d",
           t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second());
}

void init_webserver() {
  web_heap_point("before web handlers");

  setupSdFileListHandler();

  web_server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String logmessage = "Client:" + request->client()->remoteIP().toString() + + " " + request->url();
    webLog.println(logmessage);
    request->send_P(200, "text/html", index_html, processor);
    logmessage="";
  });


  web_server.on("/sat", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", sat_page_html);
  });

  web_server.on("/api/sat/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    const int idx = plogw->sat_idx_selected;
    const char *name = (idx >= 0 && idx < N_SATELLITES) ? sat_info[idx].name : "";
    char payload[768];
    const char *tracking_name = plogw->sat_freq_tracking_mode == SAT_RX_FIX ? "RX FIX" :
                                plogw->sat_freq_tracking_mode == SAT_TX_FIX ? "TX FIX" :
                                plogw->sat_freq_tracking_mode == SAT_SAT_FIX ? "SAT FIX" : "NO TRACK";
    const char *vfo_name = plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_TX ? "Single: TX=A" :
                           plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX ? "Single: RX=A" :
                           plogw->sat_vfo_mode == SAT_VFO_MULTI_TX_0 ? "TX=Radio0 / RX=Radio1" :
                           "TX=Radio1 / RX=Radio0";
    const int offset_hz = (idx >= 0 && idx < N_SATELLITES) ? sat_info[idx].offset_freq : 0;
    DateTime utc_time((uint32_t)(my_rtc.unixtime() - 9UL * 3600UL));
    char jst[80], utc[80];
    snprintf(jst,sizeof(jst),"%04d-%02d-%02d %02d:%02d:%02d",my_rtc.year(),my_rtc.month(),my_rtc.day(),my_rtc.hour(),my_rtc.minute(),my_rtc.second());
    snprintf(utc,sizeof(utc),"%04d-%02d-%02d %02d:%02d:%02d",utc_time.year(),utc_time.month(),utc_time.day(),utc_time.hour(),utc_time.minute(),utc_time.second());
    snprintf(payload, sizeof(payload),
             "{\"index\":%d,\"name\":\"%s\",\"enabled\":%s,"
             "\"az\":%.3f,\"el\":%.3f,\"rr\":%.5f,"
             "\"tracking_mode\":%d,\"tracking_name\":\"%s\","
             "\"vfo_mode\":%d,\"vfo_name\":\"%s\","
             "\"up_hz\":%d,\"down_hz\":%d,\"sat_up_hz\":%d,\"sat_down_hz\":%d,\"offset_hz\":%d,"
             "\"grid\":\"%s\",\"lat\":%.6f,\"lon\":%.6f,\"jst\":\"%s\",\"utc\":\"%s\"}",
             idx, name, plogw->sat ? "true" : "false",
             p13.AZ, p13.EL, p13.RR,
             plogw->sat_freq_tracking_mode, tracking_name, plogw->sat_vfo_mode, vfo_name,
             plogw->up_f, plogw->dn_f, plogw->satup_f, plogw->satdn_f, offset_hz,
             plogw->grid_locator_set, plogw->latitude, plogw->longitude, jst, utc);
    request->send(200, "application/json", payload);
  });

  web_server.on("/api/sat/db", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncResponseStream *res = request->beginResponseStream("application/json");
    res->print("{\"satellites\":[");
    bool first = true;
    for (int i = 0; i < N_SATELLITES; ++i) {
      if (sat_info[i].name[0] == '\0') continue;
      if (!first) res->print(',');
      first = false;
      res->printf("{\"index\":%d,\"name\":\"%s\",\"up0\":%d,\"up1\":%d,\"upmode\":\"%s\","
                  "\"dn0\":%d,\"dn1\":%d,\"dnmode\":\"%s\",\"beacon\":%d,\"offset\":%d,\"tle\":%s}",
                  i, sat_info[i].name, sat_info[i].up_f0, sat_info[i].up_f1, sat_info[i].up_mode,
                  sat_info[i].dn_f0, sat_info[i].dn_f1, sat_info[i].dn_mode,
                  sat_info[i].bc_f0, sat_info[i].offset_freq,
                  sat_info[i].YEAR ? "true" : "false");
    }
    res->print("]}");
    request->send(res);
  });

  web_server.on("/api/sat/db/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    const char *required[] = {"index","name","up0","up1","upmode","dn0","dn1","dnmode","beacon","offset"};
    for (const char *p : required) {
      if (!request->hasParam(p)) { request->send(400, "text/plain", String("Missing ") + p); return; }
    }

    int idx = request->getParam("index")->value().toInt();
    String name = request->getParam("name")->value();
    String upmode = request->getParam("upmode")->value();
    String dnmode = request->getParam("dnmode")->value();
    name.trim(); upmode.trim(); dnmode.trim();
    name.toUpperCase(); upmode.toUpperCase(); dnmode.toUpperCase();

    if (name.length() < 1 || name.length() >= sizeof(sat_info[0].name) ||
        upmode.length() >= sizeof(sat_info[0].up_mode) ||
        dnmode.length() >= sizeof(sat_info[0].dn_mode)) {
      request->send(400, "text/plain", "Name or mode is too long"); return;
    }
    for (size_t n=0; n<name.length(); ++n) {
      const char c=name[n];
      if (!(isalnum((unsigned char)c) || c==' ' || c=='-' || c=='_' || c=='/' ||
            c=='(' || c==')' || c=='.' || c=='+')) {
        request->send(400, "text/plain", "Invalid character in name"); return;
      }
    }

    const double up0m = request->getParam("up0")->value().toDouble();
    const double up1m = request->getParam("up1")->value().toDouble();
    const double dn0m = request->getParam("dn0")->value().toDouble();
    const double dn1m = request->getParam("dn1")->value().toDouble();
    const double bcm  = request->getParam("beacon")->value().toDouble();
    if (up0m < 0 || up1m < 0 || dn0m < 0 || dn1m < 0 || bcm < 0 ||
        up0m > 2147.0 || up1m > 2147.0 || dn0m > 2147.0 || dn1m > 2147.0 || bcm > 2147.0) {
      request->send(400, "text/plain", "Frequency out of range"); return;
    }

    if (idx < 0) {
      for (idx = 0; idx < N_SATELLITES; ++idx) if (sat_info[idx].name[0] == '\0') break;
      if (idx >= N_SATELLITES) { request->send(409, "text/plain", "Satellite database is full"); return; }
    } else if (idx >= N_SATELLITES || sat_info[idx].name[0] == '\0') {
      request->send(400, "text/plain", "Invalid index"); return;
    }

    for (int i=0;i<N_SATELLITES;++i) {
      if (i != idx && sat_info[i].name[0] && name.equalsIgnoreCase(sat_info[i].name)) {
        request->send(409, "text/plain", "Satellite name already exists"); return;
      }
    }

    const bool name_changed = strcmp(sat_info[idx].name, name.c_str()) != 0;
    strlcpy(sat_info[idx].name, name.c_str(), sizeof(sat_info[idx].name));
    sat_info[idx].up_f0 = (int)(up0m * 1000000.0 + 0.5);
    sat_info[idx].up_f1 = (int)(up1m * 1000000.0 + 0.5);
    sat_info[idx].dn_f0 = (int)(dn0m * 1000000.0 + 0.5);
    sat_info[idx].dn_f1 = (int)(dn1m * 1000000.0 + 0.5);
    sat_info[idx].bc_f0 = (int)(bcm * 1000000.0 + 0.5);
    sat_info[idx].offset_freq = request->getParam("offset")->value().toInt();
    strlcpy(sat_info[idx].up_mode, upmode.c_str(), sizeof(sat_info[idx].up_mode));
    strlcpy(sat_info[idx].dn_mode, dnmode.c_str(), sizeof(sat_info[idx].dn_mode));

    if (name_changed) {
      sat_info[idx].YEAR = 0;
      sat_info[idx].EPOCH = 0;
      sat_info[idx].nextaos = DateTime((uint32_t)0);
      sat_info[idx].nextlos = DateTime((uint32_t)0);
      sat_info[idx].maxel = 0;
    }
    save_satinfo();
    readtlefile();
    start_calc_nextaos();
    request->send(200, "text/plain", sat_info[idx].YEAR ? "Saved; TLE matched" : "Saved; no matching TLE yet");
  });

  web_server.on("/api/sat/db/delete", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("index")) { request->send(400, "text/plain", "Missing index"); return; }
    const int idx = request->getParam("index")->value().toInt();
    if (idx < 0 || idx >= N_SATELLITES || sat_info[idx].name[0] == '\0') {
      request->send(400, "text/plain", "Invalid index"); return;
    }
    if (plogw->sat_idx_selected == idx) {
      plogw->sat = 0;
      plogw->sat_idx_selected = -1;
      plogw->sat_name_set[0] = '\0';
    }
    struct sat_info empty_sat = {};
    sat_info[idx] = empty_sat;
    save_satinfo();
    start_calc_nextaos();
    request->send(200, "text/plain", "Deleted");
  });

  web_server.on("/api/sat/list", HTTP_GET, [](AsyncWebServerRequest *request) {
    bool have_valid_sat = false;
    for (int i = 0; i < N_SATELLITES; ++i) {
      if (sat_info[i].name[0] != '\0' && sat_info[i].YEAR != 0) {
        have_valid_sat = true;
        break;
      }
    }
    if (!have_valid_sat) {
      load_satinfo();
      readtlefile();
    }

    AsyncResponseStream *res = request->beginResponseStream("application/json");
    res->print("{\"satellites\":[");
    bool first = true;
    for (int i = 0; i < N_SATELLITES; ++i) {
      if (sat_info[i].name[0] == '\0' || sat_info[i].YEAR == 0) continue;
      if (!first) res->print(',');
      first = false;
      res->printf("{\"index\":%d,\"name\":\"%s\",\"selected\":%s}",
                  i, sat_info[i].name, i == plogw->sat_idx_selected ? "true" : "false");
    }
    res->print("]}");
    request->send(res);
  });

  web_server.on("/api/sat/select", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("index")) { request->send(400, "text/plain", "Missing index"); return; }
    const int idx = request->getParam("index")->value().toInt();
    if (idx < 0 || idx >= N_SATELLITES || sat_info[idx].name[0] == '\0' || sat_info[idx].YEAR == 0) {
      request->send(400, "text/plain", "Invalid satellite"); return;
    }
    plogw->sat_idx_selected = idx;
    strlcpy(plogw->sat_name + 2, sat_info[idx].name, LEN_SATNAME_WINDOW + 1);
    sat_name_entered();
    set_sat_info_calc();
    set_sat_freq_calc();
    request->send(200, "text/plain", "Selected and satellite operation started");
  });

  web_server.on("/api/sat/enable", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("enabled")) { request->send(400, "text/plain", "Missing enabled"); return; }
    if (request->getParam("enabled")->value().toInt()) {
      bool tle_loaded_now = false;
      if (plogw->tle_unixtime == 0) {
        readtlefile();
        tle_loaded_now = (plogw->tle_unixtime != 0);
      }
      if (plogw->sat_idx_selected < 0 || plogw->sat_idx_selected >= N_SATELLITES ||
          sat_info[plogw->sat_idx_selected].name[0] == '\0') {
        request->send(409, "text/plain", "Select a satellite first");
        return;
      }
      strlcpy(plogw->sat_name + 2, sat_info[plogw->sat_idx_selected].name, LEN_SATNAME_WINDOW + 1);
      sat_name_entered(); set_sat_info_calc(); set_sat_freq_calc();
      request->send(200, "text/plain",
                    tle_loaded_now ? "Satellite operation ON (TLE loaded from SD)"
                                   : "Satellite operation ON");
    } else {
      plogw->sat = 0;
      request->send(200, "text/plain", "Satellite operation OFF");
    }
  });

  web_server.on("/api/sat/tracking", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("mode")) { request->send(400, "text/plain", "Missing mode"); return; }
    const int mode = request->getParam("mode")->value().toInt();
    if (mode < SAT_RX_FIX || mode > SAT_NO_TRACK) { request->send(400, "text/plain", "Invalid tracking mode"); return; }
    plogw->sat_freq_tracking_mode = mode; set_sat_freq_calc();
    request->send(200, "text/plain", "Tracking mode changed");
  });

  web_server.on("/api/sat/vfo/auto", HTTP_POST, [](AsyncWebServerRequest *request) {
    char reason[192];
    const int mode = auto_select_sat_vfo_mode(reason, sizeof(reason));
    if (mode >= 0) {
      set_sat_freq_calc();
      request->send(200, "text/plain", String("Auto VFO: ") + reason);
    } else {
      request->send(409, "text/plain", String("Auto VFO: ") + reason);
    }
  });

  web_server.on("/api/sat/vfo", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("mode")) { request->send(400, "text/plain", "Missing mode"); return; }
    const int mode = request->getParam("mode")->value().toInt();
    if (mode < SAT_VFO_SINGLE_A_TX || mode > SAT_VFO_MULTI_TX_1) { request->send(400, "text/plain", "Invalid VFO mode"); return; }
    plogw->sat_vfo_mode = mode; set_sat_freq_calc();
    request->send(200, "text/plain", "VFO mode changed");
  });

  web_server.on("/api/sat/action", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("name")) { request->send(400, "text/plain", "Missing action"); return; }
    const String name = request->getParam("name")->value();
    if (name == "center") { set_sat_center_frequency(); request->send(200, "text/plain", "Center frequency selected"); }
    else if (name == "beacon") { set_sat_beacon_frequency(); request->send(200, "text/plain", "Beacon frequency selected"); }
    else request->send(400, "text/plain", "Invalid action");
  });

  web_server.on("/api/sat/offset", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("hz")) { request->send(400, "text/plain", "Missing hz"); return; }
    const int hz = request->getParam("hz")->value().toInt();
    if (!(hz == -100 || hz == -10 || hz == 10 || hz == 100)) { request->send(400, "text/plain", "Invalid offset"); return; }
    if (!plogw->sat) { request->send(409, "text/plain", "Satellite operation is OFF"); return; }
    adjust_sat_offset(hz);
    request->send(200, "text/plain", "Offset adjusted");
  });

  web_server.on("/api/sat/location", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("grid")) { request->send(400, "text/plain", "Missing grid"); return; }
    String grid = request->getParam("grid")->value();
    grid.trim(); grid.toUpperCase();
    if (grid.length() < 4 || grid.length() > LEN_GL) { request->send(400, "text/plain", "Grid must be 4-7 characters"); return; }
    for (size_t i=0;i<grid.length();++i) {
      const char c=grid[i];
      if (!isalnum((unsigned char)c)) { request->send(400, "text/plain", "Invalid grid"); return; }
    }
    strlcpy(plogw->grid_locator + 2, grid.c_str(), LEN_GL + 1);
    set_grid_locator_information();
    save_settings("");
    if (plogw->sat) {
      set_sat_info_calc();
      set_sat_freq_calc();
    }
    request->send(200, "text/plain", "Location updated and saved");
  });

  web_server.on("/api/sat/pass", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("index")) { request->send(400, "text/plain", "Missing index"); return; }
    const int idx = request->getParam("index")->value().toInt();
    if (idx < 0 || idx >= N_SATELLITES || sat_info[idx].name[0] == '\0' || sat_info[idx].YEAR == 0) {
      request->send(400, "text/plain", "Invalid satellite"); return;
    }
    const DateTime aos = sat_info[idx].nextaos;
    const DateTime los = sat_info[idx].nextlos;
    if (compare_datetime(los, aos) <= 0) {
      request->send(409, "text/plain", "No valid calculated pass"); return;
    }

    const uint32_t aos_epoch = aos.unixtime();
    const uint32_t los_epoch = los.unixtime();
    const uint32_t duration = los_epoch - aos_epoch;
    uint32_t step = duration / 36U;
    if (step < 15U) step = 15U;
    if (step > 60U) step = 60U;

    char aos_text[40], los_text[40], mel_text[40];
    web_sat_format_datetime(aos, aos_text, sizeof(aos_text));
    web_sat_format_datetime(los, los_text, sizeof(los_text));

    AsyncResponseStream *res = request->beginResponseStream("application/json");
    res->printf("{\"index\":%d,\"name\":\"%s\",\"aos\":\"%s\",\"los\":\"%s\","
                "\"up0\":%d,\"up1\":%d,\"upmode\":\"%s\",\"dn0\":%d,\"dn1\":%d,"
                "\"dnmode\":\"%s\",\"beacon\":%d,\"offset\":%d,\"points\":[",
                idx, sat_info[idx].name, aos_text, los_text,
                sat_info[idx].up_f0, sat_info[idx].up_f1, sat_info[idx].up_mode,
                sat_info[idx].dn_f0, sat_info[idx].dn_f1, sat_info[idx].dn_mode,
                sat_info[idx].bc_f0, sat_info[idx].offset_freq);

    bool first = true;
    double max_el = -90.0, mel_az = 0.0, aos_az = 0.0, los_az = 0.0;
    DateTime mel_time = aos;
    uint32_t last_off = 0;
    for (uint32_t off = 0; off <= duration; off += step) {
      const DateTime t(aos_epoch + off);
      double az = 0, el = 0, rr = 0;
      if (!web_sat_calc_point(idx, t, &az, &el, &rr)) continue;
      if (off == 0) aos_az = az;
      if (el > max_el) { max_el = el; mel_az = az; mel_time = t; }
      if (!first) res->print(',');
      first = false;
      res->printf("{\"t\":%lu,\"az\":%.3f,\"el\":%.3f}", (unsigned long)t.unixtime(), az, el);
      last_off = off;
      if (duration - off < step) break;
    }
    if (last_off != duration) {
      double az = 0, el = 0, rr = 0;
      if (web_sat_calc_point(idx, los, &az, &el, &rr)) {
        if (!first) res->print(',');
        res->printf("{\"t\":%lu,\"az\":%.3f,\"el\":%.3f}", (unsigned long)los.unixtime(), az, el);
        los_az = az;
        if (el > max_el) { max_el = el; mel_az = az; mel_time = los; }
      }
    } else {
      double el = 0, rr = 0;
      web_sat_calc_point(idx, los, &los_az, &el, &rr);
    }
    web_sat_format_datetime(mel_time, mel_text, sizeof(mel_text));
    res->printf("],\"aos_az\":%.3f,\"mel\":\"%s\",\"mel_az\":%.3f,\"mel_el\":%.3f,\"los_az\":%.3f}",
                aos_az, mel_text, mel_az, max_el, los_az);
    request->send(res);
  });

  web_server.on("/api/sat/nowpos", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("index")) { request->send(400, "text/plain", "Missing index"); return; }
    const int idx = request->getParam("index")->value().toInt();
    if (idx < 0 || idx >= N_SATELLITES || sat_info[idx].name[0] == '\0' || sat_info[idx].YEAR == 0) {
      request->send(400, "text/plain", "Invalid satellite"); return;
    }
    double az = 0, el = 0, rr = 0;
    if (!web_sat_calc_point(idx, my_rtc, &az, &el, &rr)) {
      request->send(500, "text/plain", "Position calculation failed"); return;
    }
    char now_text[40];
    web_sat_format_datetime(my_rtc, now_text, sizeof(now_text));
    const bool active = compare_datetime(my_rtc, sat_info[idx].nextaos) >= 0 &&
                        compare_datetime(sat_info[idx].nextlos, my_rtc) >= 0;
    const bool selected = idx == plogw->sat_idx_selected && plogw->sat;
    char payload[320];
    snprintf(payload, sizeof(payload),
             "{\"index\":%d,\"jst\":\"%s\",\"az\":%.3f,\"el\":%.3f,\"rr\":%.5f,"
             "\"active\":%s,\"selected\":%s,\"up_hz\":%d,\"down_hz\":%d}",
             idx, now_text, az, el, rr,
             active ? "true" : "false", selected ? "true" : "false",
             selected ? plogw->up_f : 0, selected ? plogw->dn_f : 0);
    request->send(200, "application/json", payload);
  });

  web_server.on("/api/sat/aos", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncResponseStream *res = request->beginResponseStream("application/json");
    int total = 0;
    int completed = 0;
    for (int i=0;i<N_SATELLITES;i++) {
      if (!(sat_info[i].name[0] && sat_info[i].YEAR)) continue;
      total++;
      if (!plogw->f_nextaos || i < plogw->nextaos_satidx) completed++;
    }
    const int current_idx = plogw->nextaos_satidx;
    const char *current_name = (current_idx >= 0 && current_idx < N_SATELLITES && sat_info[current_idx].name[0]) ? sat_info[current_idx].name : "";
    const char *state_name = plogw->f_nextaos == 1 ? "prepare" :
                             plogw->f_nextaos == 2 ? "AOS search" :
                             plogw->f_nextaos == 3 ? "LOS search" :
                             plogw->f_nextaos == 4 ? "finalize" :
                             plogw->f_nextaos == 5 ? "next satellite" : "idle";
    res->printf("{\"calculating\":%s,\"completed\":%d,\"total\":%d,\"current_index\":%d,\"current_name\":\"%s\",\"state\":%d,\"state_name\":\"%s\",\"passes\":[",
                plogw->f_nextaos ? "true" : "false", completed, total, current_idx,
                current_name, plogw->f_nextaos, state_name);
    int aos_idx[N_SATELLITES];
    int aos_count = 0;
    for (int i = 0; i < N_SATELLITES; ++i) {
      if (sat_info[i].name[0] == '\0' || sat_info[i].YEAR == 0) continue;
      if (plogw->f_nextaos && i >= plogw->nextaos_satidx) continue;
      if (compare_datetime(sat_info[i].nextlos, my_rtc) <= 0) continue;
      if (compare_datetime(sat_info[i].nextlos, sat_info[i].nextaos) <= 0) continue;
      aos_idx[aos_count++] = i;
    }

    for (int a = 1; a < aos_count; ++a) {
      const int key = aos_idx[a];
      int b = a - 1;
      while (b >= 0 &&
             compare_datetime(sat_info[aos_idx[b]].nextaos,
                              sat_info[key].nextaos) > 0) {
        aos_idx[b + 1] = aos_idx[b];
        --b;
      }
      aos_idx[b + 1] = key;
    }

    bool first = true;
    const int max_show = 15;
    const int shown_count = aos_count < max_show ? aos_count : max_show;
    for (int n = 0; n < shown_count; ++n) {
      const int i = aos_idx[n];
      if (!first) res->print(',');
      first = false;
      char aos[24], los[24];
      snprintf(aos,sizeof(aos),"%04d-%02d-%02d %02d:%02d",sat_info[i].nextaos.year(),sat_info[i].nextaos.month(),sat_info[i].nextaos.day(),sat_info[i].nextaos.hour(),sat_info[i].nextaos.minute());
      snprintf(los,sizeof(los),"%04d-%02d-%02d %02d:%02d",sat_info[i].nextlos.year(),sat_info[i].nextlos.month(),sat_info[i].nextlos.day(),sat_info[i].nextlos.hour(),sat_info[i].nextlos.minute());
      res->printf("{\"index\":%d,\"name\":\"%s\",\"aos\":\"%s\",\"los\":\"%s\",\"max_el\":%.2f}",i,sat_info[i].name,aos,los,sat_info[i].maxel);
    }
    res->print("]}"); request->send(res);
  });


  web_server.on("/api/sat/tle/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    int valid_satellites = 0;
    for (int i = 0; i < N_SATELLITES; ++i) {
      if (sat_info[i].name[0] && sat_info[i].YEAR) valid_satellites++;
    }
    char payload[448];
    snprintf(payload,sizeof(payload),"{\"url\":\"%s\",\"requested\":%s,\"in_progress\":%s,\"last_result\":%d,\"tle_time\":%lu,\"valid_satellites\":%d}",
             sat_tle_url, sat_tle_update_requested?"true":"false", sat_tle_update_in_progress?"true":"false",
             sat_tle_last_result, plogw->tle_unixtime, valid_satellites);
    request->send(200,"application/json",payload);
  });
  web_server.on("/api/sat/tle/config", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("url")) { request->send(400,"text/plain","Missing URL"); return; }
    String url=request->getParam("url")->value(); url.trim();
    if (!(url.startsWith("http://") || url.startsWith("https://")) || url.length() >= sizeof(sat_tle_url)) { request->send(400,"text/plain","Invalid URL"); return; }
    strlcpy(sat_tle_url,url.c_str(),sizeof(sat_tle_url)); save_settings(""); request->send(200,"text/plain","TLE URL saved");
  });
  web_server.on("/api/sat/tle/update", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (sat_tle_update_in_progress || sat_tle_update_requested) { request->send(409,"text/plain","Already running"); return; }
    request_sat_tle_update(); request->send(202,"text/plain","Queued");
  });

  web_server.on("/api/sat/aos/recalculate", HTTP_POST, [](AsyncWebServerRequest *request) {
    start_calc_nextaos(); request->send(202, "text/plain", "Started");
  });

  // /potahelp
  //  const char pota_page[] PROGMEM = R"rawliteral(
  const char *pota_page = R"rawliteral(
<!DOCTYPE html>
<html lang="ja">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>POTA運用・ログ作成ヘルパー</title>
<style>
body{font-family:sans-serif;line-height:1.6;margin:18px;max-width:920px;color:#222}h1{font-size:1.55rem}h2{font-size:1.2rem;margin-top:1.8rem;border-left:5px solid #555;padding-left:.6rem}.step{border:1px solid #bbb;border-radius:8px;padding:12px 14px;margin:12px 0}.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}input{font-size:1rem;padding:7px;min-width:14em}button,.button{font-size:1rem;padding:7px 12px;cursor:pointer}.primary{font-weight:bold}.note{background:#f3f3f3;padding:9px 12px;border-radius:6px}.warn{background:#fff4d6;padding:9px 12px;border-radius:6px}.status{font-weight:bold;min-height:1.5em}.small{font-size:.92em;color:#444}code{background:#eee;padding:1px 4px}ul{padding-left:1.4em}a{color:#0645ad}
</style></head>
<body onload="updateDownloadLink()">
<p><a href="/potahelp?lang=en">English</a> | <a href="/">ホーム</a></p>
<h1>POTA運用・ログ作成ヘルパー</h1>
<p>このページでは、現在運用しているPOTA公園をDVPloggerへ設定し、その公園で記録したQSOだけをADIFで取り出してPOTAへアップロードできます。</p>
<div class="warn"><strong>重要：</strong>公園番号を入力しただけではログへ反映されません。必ず「この公園をDVPloggerへ設定」を押してください。設定後のQSOにはRemarksへ <code>POTA_MY:公園番号</code> が自動記録されます。</div>

<h2>1. 運用する公園を設定</h2>
<div class="step">
<label for="park"><strong>POTA公園番号</strong></label>
<div class="row"><input type="text" id="park" %PARK_ID% placeholder="例: JP-1001" oninput="updateDownloadLink()">
<button class="primary" onclick="setCurrentPark()">この公園をDVPloggerへ設定</button>
<button onclick="openParkPage()">公園情報をPOTAで確認</button></div>
<p id="setStatus" class="status"></p>
<p class="small">設定するとDVPloggerのJCC/JCG欄へ <code>POTA/JP-xxxx</code> が入ります。以後に登録したQSOが、この公園からのアクティベーションQSOとして識別されます。</p>
</div>

<h2>公園番号が分からない場合</h2>
<div class="step">
<p>現在地のグリッドロケーターから、近い公園を検索できます。検索結果をクリックすると、その公園を本体へ設定し、公園情報ページも開きます。</p>
<div class="row"><input id="grid" value="%GRID_LOCATOR%" placeholder="Grid例: PM95ru"><button onclick="findNearest()">近いPOTA公園を検索</button></div>
<p id="searchStatus" class="status"></p><ul id="results"></ul>
</div>

<h2>2. DVPloggerで通常どおり交信を記録</h2>
<div class="step"><p>CALLSIGN、RST、交換内容などを通常どおり入力してQSOを登録します。公園設定後に登録した各QSOへ、現在の公園番号が自動的に付加されます。</p>
<p class="note">途中で別の公園へ移動した場合は、移動後に新しい公園番号を設定してからQSOを登録してください。公園ごとにログを分けて出力できます。</p></div>

<h2>3. この公園のADIFログを作成</h2>
<div class="step"><p>下のボタンは、Remarksに現在の公園番号が記録されたQSOだけを抽出します。</p>
<a id="dl" class="button primary" href="/adif" download="pota_log.adi">この公園のADIFをダウンロード</a>
<p id="status" class="status"></p>
<p class="small">ファイル名は <code>pota_log_JP-xxxx.adi</code> です。ダウンロード前に公園番号が正しいことを確認してください。</p></div>

<h2>4. POTAへアップロード</h2>
<div class="step"><ol><li>先に上のボタンでADIFを保存します。</li><li>「POTAログ管理を開く」を押してPOTAへログインします。</li><li>POTAのログアップロード画面で、保存したADIFファイルを選択またはドラッグ＆ドロップします。</li><li>公園番号、日時、コールサインを確認して登録します。</li></ol>
<button onclick="openPOTA()">POTAログ管理を開く</button></div>

<h2>ボタンの意味</h2>
<ul><li><strong>この公園をDVPloggerへ設定：</strong>これから記録するQSOへ公園番号を付けます。</li><li><strong>公園情報をPOTAで確認：</strong>POTA公式の公園ページを別タブで開きます。本体設定は変更しません。</li><li><strong>近いPOTA公園を検索：</strong>SDカード上の公園一覧から距離順に候補を表示します。</li><li><strong>この公園のADIFをダウンロード：</strong>選択した公園からのQSOだけをADIFへ出力します。</li><li><strong>POTAログ管理を開く：</strong>POTA公式のログ画面を開きます。自動アップロードは行いません。</li></ul>
<p><a href="/">ホームへ戻る</a>　<a href="/sotahelp?lang=ja">SOTAヘルパーへ</a></p>
<script>
function normPark(){return document.getElementById('park').value.trim().toUpperCase();}
function findNearest(){
 const grid=document.getElementById('grid').value.trim(); const st=document.getElementById('searchStatus');
 if(!grid){st.textContent='グリッドロケーターを入力してください。';return;} st.textContent='検索中…';
 fetch(`/nearest?grid=${encodeURIComponent(grid)}`).then(r=>{if(!r.ok)throw new Error('検索に失敗しました');return r.json();}).then(showResults).catch(e=>st.textContent=e.message);
}
function showResults(list){
 const ul=document.getElementById('results');ul.innerHTML='';document.getElementById('searchStatus').textContent=list.length?`${list.length}件の候補を表示しました。公園名をクリックすると本体へ設定します。`:'候補が見つかりませんでした。';
 list.forEach(p=>{const li=document.createElement('li'),a=document.createElement('a');a.href='#';a.textContent=`${p.code}: ${p.name}（${p.distance_km} km、方位 ${p.bearing_deg}°）`;a.onclick=(ev)=>{ev.preventDefault();selectPark(p.code,p.name,document.getElementById('grid').value);};li.appendChild(a);ul.appendChild(li);});
}
function notifyPark(code,name,grid){return fetch(`/select?code=${encodeURIComponent(code)}&name=${encodeURIComponent(name||'')}&grid=${encodeURIComponent(grid||'')}`).then(r=>{if(!r.ok)throw new Error('DVPloggerへの設定に失敗しました');return r.text();});}
function setCurrentPark(){
 const code=normPark(),st=document.getElementById('setStatus');if(!code){st.textContent='公園番号を入力してください。';return;}
 notifyPark(code,'',document.getElementById('grid').value).then(()=>{document.getElementById('park').value=code;updateDownloadLink();st.textContent=`${code} を現在の運用公園として設定しました。これ以後のQSOへ記録されます。`;}).catch(e=>st.textContent=e.message);
}
function selectPark(code,name,grid){notifyPark(code,name,grid).then(()=>{document.getElementById('park').value=code;updateDownloadLink();document.getElementById('setStatus').textContent=`${code} ${name} を現在の運用公園として設定しました。`;window.open(`https://pota.app/#/park/${encodeURIComponent(code)}`,'_blank');}).catch(e=>document.getElementById('setStatus').textContent=e.message);}
function openParkPage(){const code=normPark();if(!code){document.getElementById('setStatus').textContent='公園番号を入力してください。';return;}window.open(`https://pota.app/#/park/${encodeURIComponent(code)}`,'_blank');}
function updateDownloadLink(){const park=normPark(),link=document.getElementById('dl'),st=document.getElementById('status');if(!park){link.href='/adif';link.download='pota_log.adi';st.textContent='公園番号を入力し、本体へ設定してください。';}else{link.href=`/adif?park=${encodeURIComponent(park)}`;link.download=`pota_log_${park}.adi`;st.textContent=`${park} のQSOだけを抽出する準備ができています。`;}}
function openPOTA(){window.open('https://pota.app/#/user/logs','_blank');}
</script></body></html>
)rawliteral";  

  
  const char *pota_page_en = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>POTA Operation and Log Helper</title><style>body{font-family:sans-serif;line-height:1.6;margin:18px;max-width:920px;color:#222}h1{font-size:1.55rem}h2{font-size:1.2rem;margin-top:1.8rem;border-left:5px solid #555;padding-left:.6rem}.step{border:1px solid #bbb;border-radius:8px;padding:12px 14px;margin:12px 0}.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}input{font-size:1rem;padding:7px;min-width:14em}button,.button{font-size:1rem;padding:7px 12px;cursor:pointer}.primary{font-weight:bold}.note{background:#f3f3f3;padding:9px 12px;border-radius:6px}.warn{background:#fff4d6;padding:9px 12px;border-radius:6px}.status{font-weight:bold;min-height:1.5em}.small{font-size:.92em;color:#444}code{background:#eee;padding:1px 4px}ul{padding-left:1.4em}a{color:#0645ad}</style></head><body onload="updateDownloadLink()"><p><a href="/potahelp?lang=ja">日本語</a> | <a href="/">Home</a></p><h1>POTA Operation and Log Helper</h1><p>Set the POTA park currently being activated, export only QSOs made from that park as ADIF, and upload the file to POTA.</p><div class="warn"><strong>Important:</strong> Typing a park reference alone does not change the logger. Press “Set this park in DVPlogger”. Subsequent QSOs receive <code>POTA_MY:park-reference</code> in Remarks.</div>
<h2>1. Set the park being activated</h2><div class="step"><label for="park"><strong>POTA park reference</strong></label><div class="row"><input type="text" id="park" %PARK_ID% placeholder="Example: JP-1001" oninput="updateDownloadLink()"><button class="primary" onclick="setCurrentPark()">Set this park in DVPlogger</button><button onclick="openParkPage()">Open park information</button></div><p id="setStatus" class="status"></p><p class="small">DVPlogger stores <code>POTA/JP-xxxx</code> in the JCC/JCG field. QSOs logged afterward are identified as activation QSOs from this park.</p></div>
<h2>Find a nearby park</h2><div class="step"><p>Search for nearby parks using the current grid locator. Clicking a result sets the park in DVPlogger and opens its POTA page.</p><div class="row"><input id="grid" value="%GRID_LOCATOR%" placeholder="Grid example: PM95ru"><button onclick="findNearest()">Find nearby POTA parks</button></div><p id="searchStatus" class="status"></p><ul id="results"></ul></div>
<h2>2. Log QSOs normally</h2><div class="step"><p>Enter callsign, RST and exchange normally. Each QSO logged after setting the park is tagged with the current park reference.</p><p class="note">After moving to another park, set the new park before logging more QSOs. Logs can be exported separately for each park.</p></div>
<h2>3. Export this park's ADIF log</h2><div class="step"><p>The button below extracts only QSOs whose Remarks contain the current park reference.</p><a id="dl" class="button primary" href="/adif" download="pota_log.adi">Download ADIF for this park</a><p id="status" class="status"></p><p class="small">The filename is <code>pota_log_JP-xxxx.adi</code>. Verify the park reference before downloading.</p></div>
<h2>4. Upload to POTA</h2><div class="step"><ol><li>Save the ADIF file above.</li><li>Open POTA Log Manager and sign in.</li><li>Select or drag the saved ADIF file into the upload page.</li><li>Confirm the park, date/time and callsign before submitting.</li></ol><button onclick="openPOTA()">Open POTA Log Manager</button></div>
<h2>Button reference</h2><ul><li><strong>Set this park in DVPlogger:</strong> tags subsequently logged QSOs with the park reference.</li><li><strong>Open park information:</strong> opens the official POTA park page without changing DVPlogger.</li><li><strong>Find nearby POTA parks:</strong> lists candidates from the park file on the SD card.</li><li><strong>Download ADIF for this park:</strong> exports only QSOs made from the selected park.</li><li><strong>Open POTA Log Manager:</strong> opens the official log page; upload is not automatic.</li></ul><p><a href="/sotahelp?lang=en">SOTA helper</a></p>
<script>function normPark(){return document.getElementById('park').value.trim().toUpperCase();}function findNearest(){const grid=document.getElementById('grid').value.trim(),st=document.getElementById('searchStatus');if(!grid){st.textContent='Enter a grid locator.';return;}st.textContent='Searching...';fetch(`/nearest?grid=${encodeURIComponent(grid)}`).then(r=>{if(!r.ok)throw new Error('Search failed');return r.json();}).then(showResults).catch(e=>st.textContent=e.message);}function showResults(list){const ul=document.getElementById('results');ul.innerHTML='';document.getElementById('searchStatus').textContent=list.length?`${list.length} candidate(s). Click a park name to set it.`:'No candidates found.';list.forEach(p=>{const li=document.createElement('li'),a=document.createElement('a');a.href='#';a.textContent=`${p.code}: ${p.name} (${p.distance_km} km, bearing ${p.bearing_deg}°)`;a.onclick=(ev)=>{ev.preventDefault();selectPark(p.code,p.name,document.getElementById('grid').value);};li.appendChild(a);ul.appendChild(li);});}function notifyPark(code,name,grid){return fetch(`/select?code=${encodeURIComponent(code)}&name=${encodeURIComponent(name||'')}&grid=${encodeURIComponent(grid||'')}`).then(r=>{if(!r.ok)throw new Error('Failed to set DVPlogger');return r.text();});}function setCurrentPark(){const code=normPark(),st=document.getElementById('setStatus');if(!code){st.textContent='Enter a park reference.';return;}notifyPark(code,'',document.getElementById('grid').value).then(()=>{document.getElementById('park').value=code;updateDownloadLink();st.textContent=`${code} is now the active park. It will be recorded in subsequent QSOs.`;}).catch(e=>st.textContent=e.message);}function selectPark(code,name,grid){notifyPark(code,name,grid).then(()=>{document.getElementById('park').value=code;updateDownloadLink();document.getElementById('setStatus').textContent=`${code} ${name} is now the active park.`;window.open(`https://pota.app/#/park/${encodeURIComponent(code)}`,'_blank');}).catch(e=>document.getElementById('setStatus').textContent=e.message);}function openParkPage(){const code=normPark();if(!code){document.getElementById('setStatus').textContent='Enter a park reference.';return;}window.open(`https://pota.app/#/park/${encodeURIComponent(code)}`,'_blank');}function updateDownloadLink(){const park=normPark(),link=document.getElementById('dl'),st=document.getElementById('status');if(!park){link.href='/adif';link.download='pota_log.adi';st.textContent='Enter a park reference and set it in DVPlogger.';}else{link.href=`/adif?park=${encodeURIComponent(park)}`;link.download=`pota_log_${park}.adi`;st.textContent=`Ready to extract QSOs for ${park}.`;}}function openPOTA(){window.open('https://pota.app/#/user/logs','_blank');}</script></body></html>
)rawliteral";

  web_server.on("/potahelp", HTTP_GET, [pota_page,pota_page_en](AsyncWebServerRequest* request){
    const bool japanese = request->hasParam("lang") && request->getParam("lang")->value().equalsIgnoreCase("ja");
    String html(japanese ? pota_page : pota_page_en);
    String gl = String(plogw->grid_locator_set);
    html.replace("%GRID_LOCATOR%", gl);
    // replace park
    char *p1;
    if ((p1=strstr(plogw->jcc+2,"POTA/"))!=NULL) {
	char tmpbuf1[100];
	strcpy(tmpbuf1,p1+5);
	p1=strtok(tmpbuf1," ");
	if (p1!=NULL) {
	  String park = String("value=\"")+String(p1)+String("\"");
	  html.replace("%PARK_ID%",park);
	  park="";
	}
    } else {
      html.replace("%PARK_ID%",String(""));
    }

    //    request->send_P(200, "text/html", pota_page, processor);
    request->send(200, "text/html", html);
  });



  /* ───── /select?code=JA-0001&name=Yoyogi&grid=PM95ru ───── */
  web_server.on("/select", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("code") && req->hasParam("name") && req->hasParam("grid")) {
      selectedParkCode = req->getParam("code")->value();
      selectedParkName = req->getParam("name")->value();
      selectedGrid     = req->getParam("grid")->value();
      webLog.printf("SELECTED  %s  %s  %s\n",
		    selectedParkCode.c_str(),
		    selectedParkName.c_str(),
		    selectedGrid.c_str());

      // replace park(POTA/) in plogw->jcc+2
      replace_string(plogw->jcc+2,"POTA/",selectedParkCode.c_str()," ");
      webLog.print("jcc modified:");webLog.println(plogw->jcc+2);
      req->send(200, "text/plain", "OK");
    } else {
      req->send(400, "text/plain", "Missing params");
  }
  });


  const char *sota_page = R"rawliteral(
<!DOCTYPE html>
<html lang="ja">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SOTA運用・ログ作成ヘルパー</title>
<style>
body{font-family:sans-serif;line-height:1.6;margin:18px;max-width:920px;color:#222}h1{font-size:1.55rem}h2{font-size:1.2rem;margin-top:1.8rem;border-left:5px solid #555;padding-left:.6rem}.step{border:1px solid #bbb;border-radius:8px;padding:12px 14px;margin:12px 0}.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}input{font-size:1rem;padding:7px;min-width:14em}button,.button{font-size:1rem;padding:7px 12px;cursor:pointer}.primary{font-weight:bold}.note{background:#f3f3f3;padding:9px 12px;border-radius:6px}.warn{background:#fff4d6;padding:9px 12px;border-radius:6px}.status{font-weight:bold;min-height:1.5em}.small{font-size:.92em;color:#444}code{background:#eee;padding:1px 4px}ul{padding-left:1.4em}a{color:#0645ad}
</style></head>
<body onload="updateDownloadLinkSOTA()">
<p><a href="/sotahelp?lang=en">English</a> | <a href="/">ホーム</a></p>
<h1>SOTA運用・ログ作成ヘルパー</h1>
<p>このページでは、現在運用しているSOTA山頂をDVPloggerへ設定し、その山頂で記録したQSOだけをADIFで取り出してSOTA Databaseへアップロードできます。</p>
<div class="warn"><strong>重要：</strong>山頂IDを入力しただけではログへ反映されません。必ず「この山頂をDVPloggerへ設定」を押してください。設定後のQSOにはRemarksへ <code>SOTA_MY:山頂ID</code> が自動記録されます。</div>

<h2>1. 運用する山頂を設定</h2>
<div class="step"><label for="summit"><strong>SOTA山頂ID</strong></label>
<div class="row"><input type="text" id="summit" %SUMMIT_ID% placeholder="例: JA/KN-006" oninput="updateDownloadLinkSOTA()">
<button class="primary" onclick="setCurrentSummit()">この山頂をDVPloggerへ設定</button>
<button onclick="openSummitPage()">山頂情報を確認</button></div>
<p id="setStatus" class="status"></p><p class="small">設定するとDVPloggerのJCC/JCG欄へ <code>SOTA/JA/xx-xxx</code> が入ります。以後に登録したQSOが、この山頂からのアクティベーションQSOとして識別されます。</p></div>

<h2>山頂IDが分からない場合</h2>
<div class="step"><p>現在地のグリッドロケーターから近い山頂を検索できます。検索結果をクリックすると、その山頂を本体へ設定し、山頂情報ページも開きます。</p>
<div class="row"><input id="grid" value="%GRID_LOCATOR%" placeholder="Grid例: PM95ru"><button onclick="findSota()">近いSOTA山頂を検索</button></div>
<p id="searchStatus" class="status"></p><ul id="sotaResults"></ul></div>

<h2>2. DVPloggerで通常どおり交信を記録</h2>
<div class="step"><p>CALLSIGN、RST、交換内容などを通常どおり入力してQSOを登録します。山頂設定後に登録した各QSOへ、現在の山頂IDが自動的に付加されます。</p>
<p class="note">別の山頂へ移動した場合は、新しい山頂IDを設定してからQSOを登録してください。山頂ごとにログを分けて出力できます。</p></div>

<h2>3. この山頂のADIFログを作成</h2>
<div class="step"><p>下のボタンは、Remarksに現在の山頂IDが記録されたQSOだけを抽出します。</p>
<a id="dl" class="button primary" href="/adif" download="sota_log.adi">この山頂のADIFをダウンロード</a><p id="status" class="status"></p>
<p class="small">ファイル名は <code>sota_log_JA_xx-xxx.adi</code> です。ブラウザーによっては山頂ID中の「/」が「_」などへ置換されます。</p></div>

<h2>4. SOTA Databaseへアップロード</h2>
<div class="step"><ol><li>先に上のボタンでADIFを保存します。</li><li>「SOTAログアップロードを開く」を押してログインします。</li><li>Activator logのアップロードを選び、保存したADIFを指定します。</li><li>山頂ID、日時、コールサインを確認して登録します。</li></ol><button onclick="openSOTA()">SOTAログアップロードを開く</button></div>

<h2>ボタンの意味</h2><ul><li><strong>この山頂をDVPloggerへ設定：</strong>これから記録するQSOへ山頂IDを付けます。</li><li><strong>山頂情報を確認：</strong>SOTLASの山頂ページを別タブで開きます。本体設定は変更しません。</li><li><strong>近いSOTA山頂を検索：</strong>SDカード上の山頂一覧から距離順に候補を表示します。</li><li><strong>この山頂のADIFをダウンロード：</strong>選択した山頂からのQSOだけをADIFへ出力します。</li><li><strong>SOTAログアップロードを開く：</strong>SOTA Databaseのアップロード画面を開きます。自動アップロードは行いません。</li></ul>
<p><a href="/">ホームへ戻る</a>　<a href="/potahelp?lang=ja">POTAヘルパーへ</a></p>
<script>
function normSummit(){return document.getElementById('summit').value.trim().toUpperCase();}
function findSota(){const g=document.getElementById('grid').value.trim(),st=document.getElementById('searchStatus');if(!g){st.textContent='グリッドロケーターを入力してください。';return;}st.textContent='検索中…';fetch(`/nearest_summit?grid=${encodeURIComponent(g)}`).then(r=>{if(!r.ok)throw new Error('検索に失敗しました');return r.json();}).then(showSota).catch(e=>st.textContent=e.message);}
function showSota(list){const ul=document.getElementById('sotaResults');ul.innerHTML='';document.getElementById('searchStatus').textContent=list.length?`${list.length}件の候補を表示しました。山頂名をクリックすると本体へ設定します。`:'候補が見つかりませんでした。';list.forEach(s=>{const li=document.createElement('li'),a=document.createElement('a');a.href='#';a.textContent=`${s.code}: ${s.name}（${s.distance_km} km、標高 ${s.alt} m、方位 ${s.bearing_deg}°）`;a.onclick=(ev)=>{ev.preventDefault();selectSota(s.code,s.name,document.getElementById('grid').value);};li.appendChild(a);ul.appendChild(li);});}
function notifySummit(code,name,grid){return fetch(`/select_summit?code=${encodeURIComponent(code)}&name=${encodeURIComponent(name||'')}&grid=${encodeURIComponent(grid||'')}`).then(r=>{if(!r.ok)throw new Error('DVPloggerへの設定に失敗しました');return r.text();});}
function setCurrentSummit(){const code=normSummit(),st=document.getElementById('setStatus');if(!code){st.textContent='山頂IDを入力してください。';return;}notifySummit(code,'',document.getElementById('grid').value).then(()=>{document.getElementById('summit').value=code;updateDownloadLinkSOTA();st.textContent=`${code} を現在の運用山頂として設定しました。これ以後のQSOへ記録されます。`;}).catch(e=>st.textContent=e.message);}
function selectSota(code,name,grid){notifySummit(code,name,grid).then(()=>{document.getElementById('summit').value=code;updateDownloadLinkSOTA();document.getElementById('setStatus').textContent=`${code} ${name} を現在の運用山頂として設定しました。`;window.open(`https://sotl.as/summits/${encodeURIComponent(code)}`,'_blank');}).catch(e=>document.getElementById('setStatus').textContent=e.message);}
function openSummitPage(){const code=normSummit();if(!code){document.getElementById('setStatus').textContent='山頂IDを入力してください。';return;}window.open(`https://sotl.as/summits/${encodeURIComponent(code)}`,'_blank');}
function updateDownloadLinkSOTA(){const summit=normSummit(),link=document.getElementById('dl'),st=document.getElementById('status');if(!summit){link.href='/adif';link.download='sota_log.adi';st.textContent='山頂IDを入力し、本体へ設定してください。';}else{link.href=`/adif?summit=${encodeURIComponent(summit)}`;link.download=`sota_log_${summit.replaceAll('/','_')}.adi`;st.textContent=`${summit} のQSOだけを抽出する準備ができています。`;}}
function openSOTA(){window.open('https://www.sotadata.org.uk/ja/upload','_blank');}
</script></body></html>
)rawliteral";  
  
  const char *sota_page_en = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>SOTA Operation and Log Helper</title><style>body{font-family:sans-serif;line-height:1.6;margin:18px;max-width:920px;color:#222}h1{font-size:1.55rem}h2{font-size:1.2rem;margin-top:1.8rem;border-left:5px solid #555;padding-left:.6rem}.step{border:1px solid #bbb;border-radius:8px;padding:12px 14px;margin:12px 0}.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}input{font-size:1rem;padding:7px;min-width:14em}button,.button{font-size:1rem;padding:7px 12px;cursor:pointer}.primary{font-weight:bold}.note{background:#f3f3f3;padding:9px 12px;border-radius:6px}.warn{background:#fff4d6;padding:9px 12px;border-radius:6px}.status{font-weight:bold;min-height:1.5em}.small{font-size:.92em;color:#444}code{background:#eee;padding:1px 4px}ul{padding-left:1.4em}a{color:#0645ad}</style></head><body onload="updateDownloadLinkSOTA()"><p><a href="/sotahelp?lang=ja">日本語</a> | <a href="/">Home</a></p><h1>SOTA Operation and Log Helper</h1><p>Set the SOTA summit currently being activated, export only QSOs made from that summit as ADIF, and upload the file to SOTA Database.</p><div class="warn"><strong>Important:</strong> Typing a summit reference alone does not change the logger. Press “Set this summit in DVPlogger”. Subsequent QSOs receive <code>SOTA_MY:summit-reference</code> in Remarks.</div>
<h2>1. Set the summit being activated</h2><div class="step"><label for="summit"><strong>SOTA summit reference</strong></label><div class="row"><input type="text" id="summit" %SUMMIT_ID% placeholder="Example: JA/KN-006" oninput="updateDownloadLinkSOTA()"><button class="primary" onclick="setCurrentSummit()">Set this summit in DVPlogger</button><button onclick="openSummitPage()">Open summit information</button></div><p id="setStatus" class="status"></p><p class="small">DVPlogger stores <code>SOTA/JA/xx-xxx</code> in the JCC/JCG field. QSOs logged afterward are identified as activation QSOs from this summit.</p></div>
<h2>Find a nearby summit</h2><div class="step"><p>Search for nearby summits using the current grid locator. Clicking a result sets the summit in DVPlogger and opens its information page.</p><div class="row"><input id="grid" value="%GRID_LOCATOR%" placeholder="Grid example: PM95ru"><button onclick="findSota()">Find nearby SOTA summits</button></div><p id="searchStatus" class="status"></p><ul id="sotaResults"></ul></div>
<h2>2. Log QSOs normally</h2><div class="step"><p>Enter callsign, RST and exchange normally. Each QSO logged after setting the summit is tagged with the current summit reference.</p><p class="note">After moving to another summit, set the new reference before logging more QSOs. Logs can be exported separately for each summit.</p></div>
<h2>3. Export this summit's ADIF log</h2><div class="step"><p>The button below extracts only QSOs whose Remarks contain the current summit reference.</p><a id="dl" class="button primary" href="/adif" download="sota_log.adi">Download ADIF for this summit</a><p id="status" class="status"></p><p class="small">The filename is <code>sota_log_JA_xx-xxx.adi</code>. A browser may replace “/” in the summit reference with “_”.</p></div>
<h2>4. Upload to SOTA Database</h2><div class="step"><ol><li>Save the ADIF file above.</li><li>Open SOTA log upload and sign in.</li><li>Select Activator log upload and choose the saved ADIF file.</li><li>Confirm the summit, date/time and callsign before submitting.</li></ol><button onclick="openSOTA()">Open SOTA log upload</button></div>
<h2>Button reference</h2><ul><li><strong>Set this summit in DVPlogger:</strong> tags subsequently logged QSOs with the summit reference.</li><li><strong>Open summit information:</strong> opens the SOTLAS summit page without changing DVPlogger.</li><li><strong>Find nearby SOTA summits:</strong> lists candidates from the summit file on the SD card.</li><li><strong>Download ADIF for this summit:</strong> exports only QSOs made from the selected summit.</li><li><strong>Open SOTA log upload:</strong> opens the SOTA Database upload page; upload is not automatic.</li></ul><p><a href="/potahelp?lang=en">POTA helper</a></p>
<script>function normSummit(){return document.getElementById('summit').value.trim().toUpperCase();}function findSota(){const g=document.getElementById('grid').value.trim(),st=document.getElementById('searchStatus');if(!g){st.textContent='Enter a grid locator.';return;}st.textContent='Searching...';fetch(`/nearest_summit?grid=${encodeURIComponent(g)}`).then(r=>{if(!r.ok)throw new Error('Search failed');return r.json();}).then(showSota).catch(e=>st.textContent=e.message);}function showSota(list){const ul=document.getElementById('sotaResults');ul.innerHTML='';document.getElementById('searchStatus').textContent=list.length?`${list.length} candidate(s). Click a summit name to set it.`:'No candidates found.';list.forEach(x=>{const li=document.createElement('li'),a=document.createElement('a');a.href='#';a.textContent=`${x.code}: ${x.name} (${x.distance_km} km, altitude ${x.alt} m, bearing ${x.bearing_deg}°)`;a.onclick=(ev)=>{ev.preventDefault();selectSota(x.code,x.name,document.getElementById('grid').value);};li.appendChild(a);ul.appendChild(li);});}function notifySummit(code,name,grid){return fetch(`/select_summit?code=${encodeURIComponent(code)}&name=${encodeURIComponent(name||'')}&grid=${encodeURIComponent(grid||'')}`).then(r=>{if(!r.ok)throw new Error('Failed to set DVPlogger');return r.text();});}function setCurrentSummit(){const code=normSummit(),st=document.getElementById('setStatus');if(!code){st.textContent='Enter a summit reference.';return;}notifySummit(code,'',document.getElementById('grid').value).then(()=>{document.getElementById('summit').value=code;updateDownloadLinkSOTA();st.textContent=`${code} is now the active summit. It will be recorded in subsequent QSOs.`;}).catch(e=>st.textContent=e.message);}function selectSota(code,name,grid){notifySummit(code,name,grid).then(()=>{document.getElementById('summit').value=code;updateDownloadLinkSOTA();document.getElementById('setStatus').textContent=`${code} ${name} is now the active summit.`;window.open(`https://sotl.as/summits/${encodeURIComponent(code)}`,'_blank');}).catch(e=>document.getElementById('setStatus').textContent=e.message);}function openSummitPage(){const code=normSummit();if(!code){document.getElementById('setStatus').textContent='Enter a summit reference.';return;}window.open(`https://sotl.as/summits/${encodeURIComponent(code)}`,'_blank');}function updateDownloadLinkSOTA(){const summit=normSummit(),link=document.getElementById('dl'),st=document.getElementById('status');if(!summit){link.href='/adif';link.download='sota_log.adi';st.textContent='Enter a summit reference and set it in DVPlogger.';}else{link.href=`/adif?summit=${encodeURIComponent(summit)}`;link.download=`sota_log_${summit.replaceAll('/','_')}.adi`;st.textContent=`Ready to extract QSOs for ${summit}.`;}}function openSOTA(){window.open('https://www.sotadata.org.uk/en/upload','_blank');}</script></body></html>
)rawliteral";

  web_server.on("/sotahelp", HTTP_GET, [sota_page,sota_page_en](AsyncWebServerRequest* request){
    const bool japanese = request->hasParam("lang") && request->getParam("lang")->value().equalsIgnoreCase("ja");
    String html(japanese ? sota_page : sota_page_en);
    String gl = String(plogw->grid_locator_set);
    html.replace("%GRID_LOCATOR%", gl);
    gl="";
    // replace summit
    char *p1;
    if ((p1=strstr(plogw->jcc+2,"SOTA/"))!=NULL) {
      char tmpbuf1[100];
      strcpy(tmpbuf1,p1+5);
      p1=strtok(tmpbuf1," ");
      if (p1!=NULL) {
	String summit = String("value=\"")+String(p1)+String("\"");
	html.replace("%SUMMIT_ID%",summit);
	summit="";
      }
    } else {
      html.replace("%SUMMIT_ID%",String(""));
    }

    //    request->send_P(200, "text/html", pota_page, processor);
    request->send(200, "text/html", html);
    html="";
  });



  web_server.on("/select_summit", HTTP_GET, [](AsyncWebServerRequest *r){
    if(r->hasParam("code")&&r->hasParam("name")&&r->hasParam("grid")){
      selSotaCode=r->getParam("code")->value();
      selSotaName=r->getParam("name")->value();
      selGrid    =r->getParam("grid")->value();

      replace_string(plogw->jcc+2,"SOTA/",selSotaCode.c_str()," ");
      webLog.print("jcc modified:");webLog.println(plogw->jcc+2);
      
      webLog.print("select sota code:"); webLog.print(selSotaCode);
      webLog.print(" grid:"); webLog.println(selGrid);
      r->send(200,"text/plain","OK");
    } else r->send(400,"text/plain","missing");
  });
  
  // Send a GET request to <IP>/get?message=<message>  
  // run handleUpload function when any file is uploaded
  web_server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *request) {
    // The upload callback only writes and closes the file.  Send one response
    // here after the complete multipart body has been received.
    request->redirect("/");
  }, handleUpload);

  
  // /log
  web_server.on("/log", HTTP_GET, [](AsyncWebServerRequest* request) {
    String numstr = request->hasParam("num") ? request->getParam("num")->value() : "";
    String type= request->hasParam("type") ? request->getParam("type")->value() : "0";
    handleQsoLogDump(request, numstr,type.toInt());  // option 0: plain, 1: html  type 0:dump 1:txt 2:adif 3:csv
  });

  // /jarlog
  web_server.on("/jarllog", HTTP_GET, [](AsyncWebServerRequest* request) {
    String numstr = request->hasParam("num") ? request->getParam("num")->value() : "";
    handleQsoLogDump(request, numstr,4);  //   type 0:dump 1:txt 2:adif 3:csv 4:jarllog
  });

  // /readqso
  web_server.on("/readqso", HTTP_GET, [](AsyncWebServerRequest* request) {
    String numstr = request->hasParam("num") ? request->getParam("num")->value() : "";
    handleQsoLogDump(request, numstr,1);  // type 0:dump 1:txt 2:adif 3:csv 4:jarlog
  });

  // /dumpqso
  web_server.on("/dumpqso", HTTP_GET, [](AsyncWebServerRequest* request) {
    String numstr = request->hasParam("num") ? request->getParam("num")->value() : "";
    handleQsoLogDump(request, numstr,0);  // type 0:dump 1:txt 2:adif 3:csv 4:jarlog
  });
  
  // /adif
  web_server.on("/adif", HTTP_GET, [](AsyncWebServerRequest* request) {
    String numstr = request->hasParam("num") ? request->getParam("num")->value() : "";

    handleQsoLogDump(request, numstr,2);  // type 0:dump 1:txt 2:adif 3:csv 4:jarlog
  });
  
  // /csv hamlogcsv
  web_server.on("/csv", HTTP_GET, [](AsyncWebServerRequest* request) {
    String numstr = request->hasParam("num") ? request->getParam("num")->value() : "";
    handleQsoLogDump(request, numstr,3);  // type 0:dump 1:txt 2:adif 3:csv 4:jarlog
  });

// static変数としてShiftキーの状態を保持
// DVPlogger status page.  Use ?lang=en for English; Japanese is default.
web_server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
  struct StatusChunkState {
    bool english = false;
    uint8_t stage = 0;
    size_t offset = 0;
    String pending;
  };

  auto state = std::make_shared<StatusChunkState>();
  state->english = request->hasParam("lang") &&
                   request->getParam("lang")->value().equalsIgnoreCase("en");
  // Keep only one status-page section in RAM at a time.  The old handler
  // reserved 6000+ bytes and built the complete page before sending it.
  state->pending.reserve(1536);

  AsyncWebServerResponse *response = request->beginChunkedResponse(
    "text/html; charset=utf-8",
    [state](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
      (void)index;

      auto esc = [](const String &src) {
        String dst;
        dst.reserve(src.length() + 12);
        for (size_t i = 0; i < src.length(); ++i) {
          switch (src[i]) {
          case '&': dst += F("&amp;"); break;
          case '<': dst += F("&lt;"); break;
          case '>': dst += F("&gt;"); break;
          case '"': dst += F("&quot;"); break;
          default: dst += src[i]; break;
          }
        }
        return dst;
      };

      auto input_name = [state](int ptr) -> String {
        const bool english = state->english;
        if (ptr >= 10 && ptr < 10 + N_CWMSG)
          return String(english ? "CW message F" : "CWメッセージ F") + String(ptr - 9);
        if (ptr >= 30 && ptr < 30 + N_CWMSG)
          return String(english ? "RTTY message F" : "RTTYメッセージ F") + String(ptr - 29);
        switch (ptr) {
        case 0: return english ? String("Callsign") : String("相手局コールサイン");
        case 1: return english ? String("Received exchange") : String("受信ナンバー");
        case 2: return english ? String("Sent RST") : String("送信RST");
        case 3: return english ? String("Received RST") : String("受信RST");
        case 4: return english ? String("My callsign") : String("自局コールサイン");
        case 5: return english ? String("Sent exchange") : String("送出ナンバー");
        case 6: return String("Remarks");
        case 7: return english ? String("Satellite") : String("衛星名");
        case 8: return english ? String("Grid locator") : String("グリッドロケータ");
        case 9: return String("JCC/JCG");
        case 20: return english ? String("Rig name") : String("リグ名");
        case 21: return english ? String("Cluster name") : String("Cluster名");
        case 22: return english ? String("Email address") : String("メールアドレス");
        case 23: return english ? String("Cluster command") : String("Clusterコマンド");
        case 24: return english ? String("Power code") : String("電力コード");
        case 25: return String("Wi-Fi SSID");
        case 26: return english ? String("Wi-Fi password") : String("Wi-Fiパスワード");
        case 27: return english ? String("Rig specification") : String("リグ仕様");
        case 28: return String("Z-server");
        case 29: return english ? String("Operator name") : String("オペレータ名");
        case 40: return english ? String("Contest") : String("コンテスト");
        case 41: return english ? String("Cluster2 name") : String("Cluster2名");
        case 42: return english ? String("Cluster2 command") : String("Cluster2コマンド");
        default: return String("#") + String(ptr);
        }
      };

      auto input_value = [](struct radio *radio) -> String {
        const int ptr = radio->ptr_curr;
        if (ptr >= 10 && ptr < 10 + N_CWMSG) return String(plogw->cw_msg[ptr - 10] + 2);
        if (ptr >= 30 && ptr < 30 + N_CWMSG) return String(plogw->rtty_msg[ptr - 30] + 2);
        switch (ptr) {
        case 0: return String(radio->callsign + 2);
        case 1: return String(radio->recv_exch + 2);
        case 2: return String(radio->sent_rst + 2);
        case 3: return String(radio->recv_rst + 2);
        case 4: return String(plogw->my_callsign + 2);
        case 5: return String(plogw->sent_exch + 2);
        case 6: return String(radio->remarks + 2);
        case 7: return String(plogw->sat_name + 2);
        case 8: return String(plogw->grid_locator + 2);
        case 9: return String(plogw->jcc + 2);
        case 20: return String(radio->rig_name + 2);
        case 21: return String(plogw->cluster_name + 2);
        case 22: return String(plogw->email_addr + 2);
        case 23: return String(plogw->cluster_cmd + 2);
        case 24: return String(plogw->power_code + 2);
        case 25: return String(plogw->wifi_ssid + 2);
        case 26: return String("********");
        case 27: return String(radio->rig_spec_str + 2);
        case 28: return String(plogw->zserver_name + 2);
        case 29: return String(plogw->my_name + 2);
        case 40: return String(plogw->contest_name + 2);
        case 41: return String(plogw->cluster2_name + 2);
        case 42: return String(plogw->cluster2_cmd + 2);
        default: return String("-");
        }
      };

      auto add_row = [&](const __FlashStringHelper *ja,
                         const __FlashStringHelper *en,
                         const String &value) {
        state->pending += F("<tr><th>");
        state->pending += state->english ? en : ja;
        state->pending += F("</th><td>");
        state->pending += esc(value);
        state->pending += F("</td></tr>");
      };

      while (state->offset >= state->pending.length()) {
        state->pending = "";
        state->offset = 0;
        const bool english = state->english;

        switch (state->stage++) {
        case 0:
          state->pending += F("<!doctype html><html lang=\"");
          state->pending += english ? F("en") : F("ja");
          state->pending += F("\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
          state->pending += F("<title>DVPlogger Status</title><style>body{font-family:sans-serif;margin:20px;max-width:1050px}table{border-collapse:collapse;width:100%;margin-bottom:18px}th,td{border:1px solid #bbb;padding:7px;text-align:left}th{background:#eee}.summary th{width:32%}.radio th,.radio td{white-space:nowrap}.radio td:last-child{white-space:normal}.nav a{margin-right:14px}h2{margin-bottom:6px}</style></head><body>");
          state->pending += F("<div class=\"nav\"><a href=\"/\">");
          state->pending += english ? F("Home") : F("ホーム");
          state->pending += F("</a><a href=\"/settings\">");
          state->pending += english ? F("Settings") : F("設定");
          state->pending += F("</a><a href=\"/status?lang=");
          state->pending += english ? F("ja\">日本語") : F("en\">English");
          state->pending += F("</a></div><h1>DVPlogger Status</h1><h2>");
          state->pending += english ? F("Logger") : F("ロガー");
          state->pending += F("</h2><table class=\"summary\">");
          break;

        case 1: {
          const bool connected = (WiFi.status() == WL_CONNECTED);
          add_row(F("IPアドレス"), F("IP address"), connected ? WiFi.localIP().toString() : String("-"));
          add_row(F("自局コールサイン"), F("My callsign"), String(plogw->my_callsign + 2));
          add_row(F("現在のコンテスト"), F("Current contest"), String(plogw->contest_name + 2));
          add_row(F("コンテスト運用"), F("Contest logging"),
                  plogw->f_off_contest ? (english ? String("OFF (OFFCONTEST)") : String("OFF（OFFCONTEST）"))
                                      : (english ? String("ON (ONCONTEST)") : String("ON（ONCONTEST）")));
          add_row(F("フォーカスRadio"), F("Focused radio"), String(so2r.focused_radio() + 1));
          add_row(F("RX Radio"), F("RX radio"), String(so2r.rx() + 1));
          add_row(F("TX Radio"), F("TX radio"), String(so2r.tx() + 1));
          struct radio *selected = so2r.radio_selected();
          add_row(F("LCD入力欄"), F("LCD input field"), input_name(selected->ptr_curr));
          add_row(F("入力中の内容"), F("Current input"), input_value(selected));
          state->pending += F("</table><h2>Radio</h2><table class=\"radio\"><tr><th>#</th><th>");
          state->pending += english ? F("State") : F("状態");
          state->pending += F("</th><th>Rig</th><th>");
          state->pending += english ? F("Frequency") : F("周波数");
          state->pending += F("</th><th>Mode</th><th>S</th><th>CQ/S&amp;P</th><th>");
          state->pending += english ? F("LCD input field") : F("LCD入力欄");
          state->pending += F("</th></tr>");
          break;
        }

        case 2:
          for (int i = 0; i < N_RADIO; ++i) {
            struct radio *radio = &radio_list[i];
            String radio_state = radio->enabled
              ? (english ? String("Enabled") : String("有効"))
              : (english ? String("Disabled") : String("無効"));
            if (so2r.focused_radio() == i) radio_state += F(" / Focus");
            if (so2r.rx() == i) radio_state += F(" / RX");
            if (so2r.tx() == i) radio_state += F(" / TX");
            char fbuf[24];
            snprintf(fbuf, sizeof(fbuf), "%u.%05u MHz", radio->freq / 100000U,
                     radio->freq % 100000U);
            const char *rig_name = (radio->rig_spec && radio->rig_spec->name)
                                   ? radio->rig_spec->name : "-";
            state->pending += F("<tr><td>"); state->pending += String(i + 1);
            state->pending += F("</td><td>"); state->pending += esc(radio_state);
            state->pending += F("</td><td>"); state->pending += esc(String(rig_name));
            state->pending += F("</td><td>"); state->pending += fbuf;
            state->pending += F("</td><td>"); state->pending += esc(String(radio->opmode));
            state->pending += F("</td><td>");
            state->pending += radio->enabled ? String(radio->smeter / SMETER_UNIT_DBM) : String("-");
            state->pending += F("</td><td>");
            state->pending += radio->cq[radio->modetype] ? F("CQ") : F("S&P");
            state->pending += F("</td><td>"); state->pending += esc(input_name(radio->ptr_curr));
            state->pending += F("</td></tr>");
          }
          state->pending += F("</table><h2>Wi-Fi</h2><table class=\"summary\">");
          break;

        case 3: {
          const bool connected = (WiFi.status() == WL_CONNECTED);
          add_row(F("Wi-Fi状態"), F("Wi-Fi status"),
                  connected ? (english ? String("Connected") : String("接続中"))
                            : (english ? String("Disconnected") : String("未接続")));
          add_row(F("SSID"), F("SSID"), connected ? WiFi.SSID() : String("-"));
          add_row(F("受信強度"), F("Wi-Fi RSSI"), connected ? String(WiFi.RSSI()) + " dBm" : String("-"));
          add_row(F("サブネットマスク"), F("Subnet mask"), connected ? WiFi.subnetMask().toString() : String("-"));
          add_row(F("ゲートウェイ"), F("Gateway"), connected ? WiFi.gatewayIP().toString() : String("-"));
          add_row(F("MACアドレス"), F("MAC address"), WiFi.macAddress());
          state->pending += F("</table><h2>");
          state->pending += english ? F("System") : F("システム");
          state->pending += F("</h2><table class=\"summary\">");
          break;
        }

        case 4: {
          const uint32_t seconds = millis() / 1000UL;
          const uint32_t days = seconds / 86400UL;
          const uint32_t hours = (seconds / 3600UL) % 24UL;
          const uint32_t minutes = (seconds / 60UL) % 60UL;
          const uint32_t secs = seconds % 60UL;
          add_row(F("稼働時間"), F("Uptime"),
                  String(days) + "d " + String(hours) + "h " + String(minutes) + "m " + String(secs) + "s");
          add_row(F("空きヒープ"), F("Free heap"), String(ESP.getFreeHeap()) + " bytes");
          add_row(F("最小空きヒープ"), F("Minimum free heap"), String(ESP.getMinFreeHeap()) + " bytes");
          const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
          const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
          const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
          const size_t psram_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
          add_row(F("PSRAM総容量"), F("PSRAM total"), String(psram_total) + " bytes");
          add_row(F("メモリ動作モード"), F("Memory mode"),
                  String(psram_total == 0 ? "LOW (no PSRAM)" : "NORMAL (PSRAM)"));
          add_row(F("PSRAM空き"), F("Free PSRAM"), String(psram_free) + " bytes");
          add_row(F("PSRAM最大連続領域"), F("Largest PSRAM block"), String(psram_largest) + " bytes");
          add_row(F("PSRAM最小空き"), F("Minimum free PSRAM"), String(psram_min_free) + " bytes");
          state->pending += F("</table><p>");
          state->pending += english ? F("Reload this page to refresh the values.")
                                    : F("表示を更新するにはページを再読み込みしてください。");
          state->pending += F("</p></body></html>");
          break;
        }

        default:
          return 0;
        }
      }

      const size_t remain = state->pending.length() - state->offset;
      const size_t ncopy = std::min(remain, maxLen);
      if (ncopy) {
        memcpy(buffer, state->pending.c_str() + state->offset, ncopy);
        state->offset += ncopy;
      }
      return ncopy;
    });
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
});

web_server.on("/antenna_status", HTTP_GET, [](AsyncWebServerRequest *request) {
  request->send(200, "application/json", antenna_status_json());
});

web_server.on("/antenna", HTTP_GET, [](AsyncWebServerRequest *request) {
  request->send_P(200, "text/html", antenna_page_html);
});

web_server.on("/antenna_config", HTTP_GET, [](AsyncWebServerRequest *request) {
  String out;
  out.reserve(700);
  out += F("{\"enable\":"); out += antenna_control_enable ? F("true") : F("false");
  out += F(",\"host\":\""); out += antenna_host;
  out += F("\",\"port\":"); out += String(antenna_port);
  out += F(",\"pref\":[");
  for (int i=0;i<ANTENNA_PREF_ROWS;i++){if(i)out+=',';out+='\"';out+=antenna_pref[i];out+='\"';}
  out += F("],\"names\":[");
  for (int i=0;i<ANTENNA_MAX_ID;i++){if(i)out+=',';out+='\"';out+=antenna_name[i];out+='\"';}
  out += F("]}");
  request->send(200, "application/json", out);
});

web_server.on("/antenna_config", HTTP_POST, [](AsyncWebServerRequest *request) {
  if (request->hasParam("enable")) antenna_control_enable = request->getParam("enable")->value().toInt() ? 1 : 0;
  if (request->hasParam("host")) strlcpy(antenna_host, request->getParam("host")->value().c_str(), sizeof(antenna_host));
  if (request->hasParam("port")) antenna_port = request->getParam("port")->value().toInt();
  for (int i=0;i<ANTENNA_PREF_ROWS;i++) {
    String key = String("pref") + String(i+1);
    if (request->hasParam(key)) strlcpy(antenna_pref[i], request->getParam(key)->value().c_str(), sizeof(antenna_pref[i]));
  }
  for (int i=0;i<ANTENNA_MAX_ID;i++) {
    String key = String("name") + String(i+1);
    if (request->hasParam(key)) strlcpy(antenna_name[i], request->getParam(key)->value().c_str(), sizeof(antenna_name[i]));
  }
  antenna_settings_changed();
  save_settings("");
  request->send(200, "text/plain", "Saved");
});

web_server.on("/network_services_status", HTTP_GET, [](AsyncWebServerRequest *request) {
  char payload[512];
  snprintf(payload, sizeof(payload),
           "{\"zserver\":{\"auto\":%s,\"connected\":%s,\"state\":\"%s\"},"
           "\"cluster1\":{\"auto\":%s,\"connected\":%s,\"state\":\"%s\"},"
           "\"cluster2\":{\"auto\":%s,\"connected\":%s,\"state\":\"%s\"},"
           "\"ntp\":{\"started\":%s,\"synced\":%s}}",
           get_zserver_auto() ? "true" : "false",
           zserver_is_connected() ? "true" : "false", zserver_connection_state(),
           get_cluster_auto(1) ? "true" : "false",
           cluster_is_connected(1) ? "true" : "false", cluster_connection_state(1),
           get_cluster_auto(2) ? "true" : "false",
           cluster_is_connected(2) ? "true" : "false", cluster_connection_state(2),
           network_ntp_started() ? "true" : "false",
           network_ntp_synced() ? "true" : "false");
  request->send(200, "application/json", payload);
});

web_server.on("/network_service_mode", HTTP_GET, [](AsyncWebServerRequest *request) {
  if (!request->hasParam("service") || !request->hasParam("auto")) {
    request->send(400, "text/plain", "Missing service/auto");
    return;
  }
  const String service = request->getParam("service")->value();
  const int enabled = request->getParam("auto")->value().toInt() ? 1 : 0;
  if (service == "zserver") set_zserver_auto(enabled);
  else if (service == "cluster1") set_cluster_auto(1, enabled);
  else if (service == "cluster2") set_cluster_auto(2, enabled);
  else {
    request->send(400, "text/plain", "Unknown service");
    return;
  }
  save_settings("");
  request->send(200, "text/plain", enabled ? "AUTO" : "OFF");
});

// /op ページ配信
web_server.on("/op", HTTP_GET, [](AsyncWebServerRequest *request) {
  request->send_P(200, "text/html", oppage_html);
});

web_server.on("/rig_key", HTTP_GET, [](AsyncWebServerRequest *req) {
  char response_string[100];
  strcpy(response_string,"");
  if (req->hasParam("keycode")) {
    WebUiCommand cmd{};
    cmd.type=WEB_UI_KEY;
    cmd.value=req->getParam("keycode")->value().toInt();
    cmd.index=req->hasParam("index") ? req->getParam("index")->value().toInt() : -1;
    cmd.radio_index=so2r.focused_radio();
    if (req->hasParam("input0")) normalize_op_cstr(0,cmd.input0,sizeof(cmd.input0),req->getParam("input0")->value());
    if (req->hasParam("input1")) normalize_op_cstr(1,cmd.input1,sizeof(cmd.input1),req->getParam("input1")->value());
    if ((cmd.index==0 || cmd.index==1) && req->hasParam("input0") && req->hasParam("input1")) {
      cmd.type=WEB_UI_ENTER;
      cmd.value=req->hasParam("no_tx") ? req->getParam("no_tx")->value().toInt() : 0;
    }
    if (!enqueue_web_ui(cmd)) { req->send(503,"text/plain","Web UI queue full"); return; }
    req->send(202,"text/plain","Queued");
  } else if (req->hasParam("command")) {

    String command;
    // rig name change
    command = req->getParam("command")->value();
    webLog.print("command:");webLog.println(command);
    if (command == "cqsp") {
      WebUiCommand cmd{};
      cmd.type = WEB_UI_CQSP_TOGGLE;
      cmd.radio_index = so2r.focused_radio();
      if (req->hasParam("radio")) {
        const int requested_radio = req->getParam("radio")->value().toInt();
        if (requested_radio < 0 || requested_radio >= N_RADIO) {
          req->send(400,"text/plain","Invalid radio");
          return;
        }
        cmd.radio_index = requested_radio;
      }
      if (!enqueue_web_ui(cmd)) { req->send(503,"text/plain","Web UI queue full"); return; }
      req->send(202,"text/plain","CQ/S&P toggle queued");
      return;
    }
    if (command == "cw_send") {
      if (!req->hasParam("value")) { req->send(400,"text/plain","Missing CW / Voice text"); return; }
      WebUiCommand cmd{};
      cmd.type = WEB_UI_CW_SEND;
      cmd.radio_index = so2r.focused_radio();
      if (req->hasParam("radio")) {
        const int requested_radio = req->getParam("radio")->value().toInt();
        if (requested_radio < 0 || requested_radio >= N_RADIO) {
          req->send(400,"text/plain","Invalid radio");
          return;
        }
        cmd.radio_index = requested_radio;
      }
      String value = req->getParam("value")->value();
      value.replace("\r", " ");
      value.replace("\n", " ");
      strlcpy(cmd.input0, value.c_str(), sizeof(cmd.input0));
      if (!enqueue_web_ui(cmd)) { req->send(503,"text/plain","Web UI queue full"); return; }
      req->send(202,"text/plain","CW / Voice queued");
      return;
    }
    if (command == "wpm") {
      if (!req->hasParam("value")) { req->send(400,"text/plain","Missing WPM"); return; }
      WebUiCommand cmd{};
      cmd.type = WEB_UI_WPM_SET;
      cmd.value = req->getParam("value")->value().toInt();
      if (!enqueue_web_ui(cmd)) { req->send(503,"text/plain","Web UI queue full"); return; }
      req->send(202,"text/plain","WPM queued");
      return;
    }
    if (command == "tone_cw") {
      if (!req->hasParam("radio")) { req->send(400,"text/plain","Missing radio"); return; }
      int radio_index = req->getParam("radio")->value().toInt();
      if (radio_index < 0 || radio_index >= N_RADIO) {
        req->send(400,"text/plain","Invalid radio");
        return;
      }
      WebUiCommand cmd{};
      cmd.type = WEB_UI_TONE_CW;
      cmd.radio_index = radio_index;
      if (!enqueue_web_ui(cmd)) { req->send(503,"text/plain","Web UI queue full"); return; }
      req->send(202,"text/plain","Tone CW queued");
      return;
    }
    if (command == "set") {
      if (req->hasParam("index") && req->hasParam("value")) {
	// get index and value
	int idx = req->getParam("index")->value().toInt();
	String value = normalize_op_value(idx, req->getParam("value")->value());
        if ((idx >= 0 && idx <= 5)) {
          WebUiCommand cmd{};
          cmd.type = WEB_UI_SET;
          cmd.index = idx;
          strlcpy(cmd.input0, value.c_str(), sizeof(cmd.input0));
          if (!enqueue_web_ui(cmd)) { req->send(503,"text/plain","Web UI queue full"); return; }
          req->send(202,"text/plain","Queued");
          return;
        }
	switch (idx) {
	case 10: // radio 0 name
	case 11: // radio 1 name
	case 12: // radio 2 name
	  int radio_idx;	  
	  radio_idx=idx-10;
	  if (radio_idx>=0 || radio_idx <= 2) {
	    struct radio *radio ;
	    radio = &radio_list[radio_idx];
	    strncpy(radio->rig_name+2,value.c_str(),LEN_RIG_NAME-1);
	    set_rig_from_name(radio);
	    webLog.print("rig change radio=");webLog.print(radio_idx);
	    webLog.print(" name=");webLog.println(radio->rig_name+2);
	    strcpy(response_string,"OK");
	  }
	  break;
	case 13: // contest name
	  strncpy(plogw->contest_name+2,value.c_str(),LEN_CONTEST_NAME);
          plogw->contest_name[2 + LEN_CONTEST_NAME] = '\0';
          if (is_user_md_contest_name(plogw->contest_name + 2)) {
            start_user_md_contest(plogw->contest_name + 2);
          } else {
	    search_contest_id_from_name();
          }
	  strcpy(response_string,"OK");
	  break;
	}
	value="";
      }
    }
    command="";
    req->send(200, "text/plain",response_string);
  } else {
    req->send(400, "text/plain", "Missing parameter");
  }

});

web_server.on("/control", HTTP_GET, [](AsyncWebServerRequest *request) {
  if (!request->hasParam("type") || !request->hasParam("value")) {
    request->send(400,"text/plain","Missing parameter"); return;
  }
  WebUiCommand cmd{};
  cmd.type=WEB_UI_CONTROL;
  strlcpy(cmd.name,request->getParam("type")->value().c_str(),sizeof(cmd.name));
  String value=request->getParam("value")->value();
  cmd.value=value.toInt();
  strlcpy(cmd.input0,value.c_str(),sizeof(cmd.input0));
  if (!enqueue_web_ui(cmd)) { request->send(503,"text/plain","Web UI queue full"); return; }
  request->send(202,"text/plain","Queued");
});

       
web_server.on("/radio_mode", HTTP_GET, [](AsyncWebServerRequest *req) {
  if (!req->hasParam("mode")) { req->send(400, "text/plain", "Missing mode"); return; }
  const int mode = req->getParam("mode")->value().toInt();
  if (mode < SO2R::RADIO_MODE_SO1R || mode > SO2R::RADIO_MODE_SO2R) {
    req->send(400, "text/plain", "Invalid radio mode"); return;
  }
  if (so2r.sequence_stat() != SO2R::Default) {
    req->send(409, "text/plain", "TX/sequence active"); return;
  }
  for (int i = 0; i < N_RADIO; ++i) {
    if (radio_list[i].ptt_stat != 0) { req->send(409, "text/plain", "PTT active"); return; }
  }
  WebUiCommand cmd{};
  cmd.type = WEB_UI_RADIO_MODE;
  cmd.value = mode;
  if (!enqueue_web_ui(cmd)) { req->send(503, "text/plain", "Web UI queue full"); return; }
  req->send(202, "text/plain", "Queued");
});

web_server.on("/so2r_pair_status", HTTP_GET, [](AsyncWebServerRequest *req) {
  so2r.validate_so2r_pairs();
  char payload[192];
  snprintf(payload, sizeof(payload),
           "{\"enabled\":[%d,%d,%d],\"a\":%d,\"b\":%d}",
           radio_list[0].enabled ? 1 : 0,
           radio_list[1].enabled ? 1 : 0,
           radio_list[2].enabled ? 1 : 0,
           so2r.so2r_pair_a(), so2r.so2r_pair_b());
  req->send(200, "application/json", payload);
});

web_server.on("/so2r_pair", HTTP_GET, [](AsyncWebServerRequest *req) {
  if (!req->hasParam("tx") || !req->hasParam("rx")) {
    req->send(400, "text/plain", "Missing tx or rx");
    return;
  }
  const int tx = req->getParam("tx")->value().toInt();
  const int rx = req->getParam("rx")->value().toInt();
  if (!so2r.set_so2r_pair(tx, rx)) {
    req->send(400, "text/plain", "Invalid or disabled SO2R partner");
    return;
  }
  save_settings("");
  snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
           "SO2R PAIR SET\nR%d <-> R%d\nSaved", tx, rx);
  request_display_update_on_demand();
  request_bandmap_update_on_demand();
  upd_display_info_flash(dp->lcdbuf);
  req->send(200, "text/plain", "Saved");
});

// /op main status: one compact response instead of many /radio_status requests.
web_server.on("/op_status", HTTP_GET, [](AsyncWebServerRequest *req) {
  struct radio *radio = so2r.radio_selected();
  char radio_text[120];
  char web_clock[32];
  format_display_clock(web_clock, sizeof(web_clock), true);
  unsigned int frac = (radio->freq % (1000/FREQ_UNIT))/(10/FREQ_UNIT);
  unsigned int khz = (radio->freq / (1000/FREQ_UNIT)) % 1000;
  snprintf(radio_text, sizeof(radio_text),
           "%-20s Radio:%d %3s Freq:%4d.%03d.%02d Hz Mode:%4s",
           web_clock, radio->rig_idx,
           radio->cq[radio->modetype] == LOG_CQ ? "CQ" : "S&P",
           radio->freq / (1000000/FREQ_UNIT), khz, frac, radio->opmode);
  char payload[512];
  snprintf(payload, sizeof(payload),
           "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%s\t%d\t1\t%d\t%d\t%d",
           radio->callsign+2, radio->recv_exch+2,
           radio->recv_rst+2, radio->sent_rst+2,
           plogw->sent_exch+2, plogw->my_callsign+2,
           radio_list[0].rig_name+2, radio_list[1].rig_name+2,
           radio_list[2].rig_name+2, plogw->contest_name+2,
           radio_text, plogw->cwbuf_display,
           radio->rig_idx, radio->opmode, radio->bandid, so2r.radio_mode,
           cw_spd, radio->f_tone_keying ? 1 : 0);
  req->send(200, "text/plain", payload);
});

// サーバー側でHTMLの一部（radio状態）を返すエンドポイント
web_server.on("/radio_status", HTTP_GET, [](AsyncWebServerRequest *req) {
  struct radio *radio;  
  radio=so2r.radio_selected();
  char string_buf[100];  
  if (req->hasParam("index")) {
    // indexパラメータに応じて、返す内容を変更
    int index = req->getParam("index")->value().toInt();
    unsigned int tmp1, tmp2;

    switch (index) {
    case 0: // call
      req->send(200, "text/plain", radio->callsign+2);  // 状態をテキストとして返す      
      break;
    case 1: // exch
      req->send(200, "text/plain", radio->recv_exch+2); 
      break;
    case 2: // received rst
      req->send(200, "text/plain", radio->recv_rst+2);  
      break;
    case 3: // sent rst
      req->send(200, "text/plain",radio->sent_rst+2);        
      break;
    case 4: // sent exch
      req->send(200, "text/plain", plogw->sent_exch+2); // this needs to be revised to reflect expanded exchange character
      break;
    case 5: // my_callsign
      req->send(200, "text/plain", plogw->my_callsign+2);              
      break;
    case 6: // rig name (focused radio)
      req->send(200, "text/plain", radio->rig_name+2);
      break;
    case 7: // rig_idx (radio #)
      sprintf(string_buf,"%d",radio->rig_idx);
      req->send(200, "text/plain", string_buf);
      break;
    case 8: // mode
      req->send(200, "text/plain", radio->opmode);
      break;
    case 9: // bandid
      sprintf(string_buf,"%d",radio->bandid);
      req->send(200, "text/plain", string_buf);
      break;
    case 10: // rig_name (radio 0)
      radio=&radio_list[0];
      req->send(200, "text/plain", radio->rig_name+2);
      break;
    case 11: // rig_name (radio 1)
      radio=&radio_list[1];
      req->send(200, "text/plain", radio->rig_name+2);
      break;
    case 12: // rig_name (radio 2)
      radio=&radio_list[2];
      req->send(200, "text/plain", radio->rig_name+2);
      break;
    case 13: // contest_name 
      req->send(200, "text/plain", plogw->contest_name+2);
      break;
    case 98: // cw
      strlcpy(string_buf, plogw->cwbuf_display, sizeof(string_buf));
      req->send(200, "text/plain", string_buf);
      break;
    case 99: // radio
      {
      char web_clock[32];
      format_display_clock(web_clock, sizeof(web_clock), true);
      tmp2= (radio->freq % (1000/FREQ_UNIT))/(10/FREQ_UNIT); // below kHz
      tmp1 = radio->freq / (1000/FREQ_UNIT); 
      tmp1 = tmp1 % 1000 ;  // kHz
      
      sprintf(string_buf,"%-20s Radio:%d %3s Freq:%4d.%03d.%02d Hz Mode:%4s",
	      web_clock,
	      radio->rig_idx,
	      radio->cq[radio->modetype] == LOG_CQ ? "CQ" : "S&P",
	      radio->freq/(1000000/FREQ_UNIT),tmp1,tmp2,
	      radio->opmode);
      req->send(200, "text/plain", string_buf);
      //      req->send(200, "text/plain", "");
      }
      break;
    default:
       req->send(400, "text/plain", "Invalid index parameter");
       break;
    }
  } else {
    req->send(400, "text/plain", "Missing index parameter");
    return;
  }
});

  // 入力受信用API
  web_server.on("/input", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("call")) {
      String call = request->getParam("call")->value();
      webLog.printf("[AJAX] Received: %s\n", call.c_str());
      request->send(200, "text/plain", "Received: " + call);
    } else {
      request->send(400, "text/plain", "Missing call param");
    }
  });

  // Send a POST request to <IP>/post with a form field message set to <message>
  web_server.on("/post", HTTP_POST, [](AsyncWebServerRequest *request){
    String message;
    if (request->hasParam(PARAM_MESSAGE, true)) {
      message = request->getParam(PARAM_MESSAGE, true)->value();
    } else {
      message = "No message sent";
    }
    request->send(200, "text/plain", "Hello, POST: " + message);
  });


  if (f_low_memory_mode) {
    webLog.println("[WEB] LOWMEM: bandmap snapshots/API disabled");
    web_server.on("/bandmap", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send(200, "text/html; charset=utf-8",
        "<!doctype html><meta charset='utf-8'><title>DVPlogger low memory</title>"
        "<h2>Bandmap disabled</h2>"
        "<p>This unit has no PSRAM. The Web bandmap is disabled to preserve memory.</p>"
        "<p>LCD bandmap, logging, status and settings remain available.</p>"
        "<p><a href='/'>Home</a> | <a href='/status'>Status</a> | <a href='/settings'>Settings</a></p>");
    });
    web_server.on("/api/bandmap/version", HTTP_GET,
      [](AsyncWebServerRequest *request) {
        request->send(503, "application/json", "{\"ready\":false,\"low_memory\":true}");
      });
  } else {
    setup_web_bandmap_handlers();
  }

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "*");

  web_server.onNotFound(notFound);

  // Keep Nearest/Summit available in LOWMEM mode. Measure each handler
  // group separately so its permanent internal-RAM cost is visible.
  web_heap_point("before nearest");
  setupNearestHandler(web_server);
  web_heap_point("after nearest");
  setupNearestSummit(web_server);
  web_heap_point("after summit");
  setupContestPageHandler();
  web_heap_point("after contest handler");
  setupSettingsPageHandler();
  web_heap_point("after settings handler");
  web_heap_point("after web handlers");
  web_server.begin();
  web_heap_point("after web begin");
}

static bool webserver_suspended_for_flash = false;

void suspend_webserver_for_flash() {
  if (webserver_suspended_for_flash) return;
  webserver_suspended_for_flash = true;
  web_server.end();
  // Async callbacks run on another task. Give an in-flight callback a short
  // chance to leave SD before flashersd takes exclusive ownership.
  delay(200);
}

void resume_webserver_after_flash() {
  if (!webserver_suspended_for_flash) return;
  web_server.begin();
  webserver_suspended_for_flash = false;
}



