/*
 * Minimal USB Audio Class 1 capture support for the TI PCM2901 used by IC-705.
 */
#include "uac_pcm2901.h"

#include "decl.h"
#include "variables.h"
#include "esp_heap_caps.h"

#ifndef VERBOSE_USB
#define VERBOSE_USB 256
#endif

PCMAudioCapture::PCMAudioCapture(USB *usb)
    : pUsb(usb),
      bAddress(0),
      bReady(false),
      fCapture(false),
      captureBuf(NULL),
      captureSamples(0),
      isoPackets(0),
      isoErrors(0),
      noRcvDav(0),
      isoRawBytes(0),
      isoFifoChunks(0),
      isoHxfrDone(0),
      isoWaitTimeouts(0),
      fSofSync(true),
      sofWaits(0),
      sofTimeouts(0),
      jerrDiagPrinted(0),
      lastIsoError(0),
      malformedBytes(0),
      minSample(0),
      maxSample(0),
      startedMs(0),
      finishedMs(0) {
  memset(epInfo, 0, sizeof(epInfo));
  epInfo[0].epAddr = 0;
  epInfo[0].maxPktSize = 8;
  epInfo[0].bmNakPower = USB_NAK_MAX_POWER;

  epInfo[1].epAddr = kCaptureEndpointNumber;
  epInfo[1].maxPktSize = kCaptureMaxPacket;
  epInfo[1].bmNakPower = USB_NAK_NOWAIT;

  if (pUsb) pUsb->RegisterDeviceClass(this);
}

void PCMAudioCapture::resetStats() {
  captureSamples = 0;
  isoPackets = 0;
  isoErrors = 0;
  memset(isoResult, 0, sizeof(isoResult));
  noRcvDav = 0;
  isoRawBytes = 0;
  isoFifoChunks = 0;
  isoHxfrDone = 0;
  isoWaitTimeouts = 0;
  sofWaits = 0;
  sofTimeouts = 0;
  jerrDiagPrinted = 0;
  lastIsoError = 0;
  malformedBytes = 0;
  minSample = 32767;
  maxSample = -32768;
  startedMs = 0;
  finishedMs = 0;
}

uint8_t PCMAudioCapture::setInterface(uint8_t iface, uint8_t alt) {
  // Standard SET_INTERFACE request: bmRequestType = host-to-device,
  // standard, interface recipient (0x01).
  return pUsb->ctrlReq(
      bAddress, 0,
      USB_SETUP_HOST_TO_DEVICE | USB_SETUP_TYPE_STANDARD |
          USB_SETUP_RECIPIENT_INTERFACE,
      USB_REQUEST_SET_INTERFACE,
      alt, 0,
      iface,
      0, 0, NULL, NULL);
}

uint8_t PCMAudioCapture::setSampleRate48k() {
  // UAC1 endpoint SET_CUR / Sampling Frequency Control.
  // 48000 Hz = 0x00BB80, sent little-endian in three bytes.
  uint8_t freq[3] = {0x80, 0xbb, 0x00};
  return pUsb->ctrlReq(
      bAddress, 0,
      USB_SETUP_HOST_TO_DEVICE | USB_SETUP_TYPE_CLASS |
          USB_SETUP_RECIPIENT_ENDPOINT,
      0x01,              // SET_CUR
      0x00, 0x01,        // wValue = SAMPLING_FREQ_CONTROL << 8
      kCaptureEndpoint,  // wIndex = endpoint address
      sizeof(freq), sizeof(freq), freq, NULL);
}

uint8_t PCMAudioCapture::getInterface(uint8_t iface, uint8_t *alt) {
  if (!alt) return USB_ERROR_INVALID_ARGUMENT;
  *alt = 0xff;
  return pUsb->ctrlReq(
      bAddress, 0,
      USB_SETUP_DEVICE_TO_HOST | USB_SETUP_TYPE_STANDARD |
          USB_SETUP_RECIPIENT_INTERFACE,
      USB_REQUEST_GET_INTERFACE,
      0, 0, iface, 1, 1, alt, NULL);
}

uint8_t PCMAudioCapture::getSampleRate(uint32_t *hz) {
  if (!hz) return USB_ERROR_INVALID_ARGUMENT;
  uint8_t freq[3] = {0, 0, 0};
  uint8_t rcode = pUsb->ctrlReq(
      bAddress, 0,
      USB_SETUP_DEVICE_TO_HOST | USB_SETUP_TYPE_CLASS |
          USB_SETUP_RECIPIENT_ENDPOINT,
      0x81,              // GET_CUR
      0x00, 0x01,        // wValue = SAMPLING_FREQ_CONTROL << 8
      kCaptureEndpoint,  // wIndex = endpoint address
      sizeof(freq), sizeof(freq), freq, NULL);
  if (!rcode)
    *hz = (uint32_t)freq[0] | ((uint32_t)freq[1] << 8) |
          ((uint32_t)freq[2] << 16);
  else
    *hz = 0;
  return rcode;
}

void PCMAudioCapture::dumpConfiguration(Print *out) {
  if (!out) out = &Serial;
  if (!pUsb || !bAddress) {
    out->println("USB AUDIO DESC: device not addressed");
    return;
  }

  uint8_t head[9] = {0};
  uint8_t rcode = pUsb->getConfDescr(bAddress, 0, sizeof(head), 0, head);
  if (rcode) {
    out->printf("USB AUDIO DESC: config header failed rcode=0x%02X\n", rcode);
    return;
  }
  if (head[0] < 9 || head[1] != USB_DESCRIPTOR_CONFIGURATION) {
    out->printf("USB AUDIO DESC: invalid config header len=%u type=%u\n",
                head[0], head[1]);
    return;
  }

  uint16_t total = (uint16_t)head[2] | ((uint16_t)head[3] << 8);
  if (total < 9 || total > 1024) {
    out->printf("USB AUDIO DESC: suspicious wTotalLength=%u\n", total);
    return;
  }

  uint8_t *buf = static_cast<uint8_t *>(malloc(total));
  if (!buf) {
    out->printf("USB AUDIO DESC: malloc(%u) failed\n", total);
    return;
  }
  rcode = pUsb->getConfDescr(bAddress, 0, total, 0, buf);
  if (rcode) {
    out->printf("USB AUDIO DESC: full config failed rcode=0x%02X\n", rcode);
    free(buf);
    return;
  }

  out->printf("USB AUDIO DESC: config value=%u total=%u interfaces=%u\n",
              buf[5], total, buf[4]);
  uint8_t curIf = 0xff, curAlt = 0xff, curClass = 0, curSub = 0;
  for (uint16_t off = 0; off + 2 <= total;) {
    uint8_t len = buf[off];
    uint8_t type = buf[off + 1];
    if (len < 2 || off + len > total) {
      out->printf("USB AUDIO DESC: malformed descriptor off=%u len=%u\n", off, len);
      break;
    }

    if (type == USB_DESCRIPTOR_INTERFACE && len >= 9) {
      curIf = buf[off + 2];
      curAlt = buf[off + 3];
      curClass = buf[off + 5];
      curSub = buf[off + 6];
      out->printf("USB AUDIO DESC: IF=%u alt=%u eps=%u class=%02X sub=%02X proto=%02X\n",
                  curIf, curAlt, buf[off + 4], curClass, curSub, buf[off + 7]);
    } else if (type == USB_DESCRIPTOR_ENDPOINT && len >= 7) {
      uint16_t maxpkt = (uint16_t)buf[off + 4] | ((uint16_t)buf[off + 5] << 8);
      out->printf("USB AUDIO DESC:   EP=0x%02X attr=0x%02X maxpkt=%u interval=%u",
                  buf[off + 2], buf[off + 3], maxpkt, buf[off + 6]);
      if (curClass == 0x01 && curSub == 0x02) out->print(" AudioStreaming");
      out->println();
    } else if (type == 0x24 && curClass == 0x01 && curSub == 0x02 && len >= 3) {
      uint8_t subtype = buf[off + 2];
      out->printf("USB AUDIO DESC:   CS_INTERFACE subtype=0x%02X len=%u", subtype, len);
      // UAC1 FORMAT_TYPE descriptor.  For FORMAT_TYPE_I, bytes 4..7 are
      // channels, subframe size, bit resolution and sampling-frequency type.
      if (subtype == 0x02 && len >= 8) {
        uint8_t channels = buf[off + 4];
        uint8_t subframe = buf[off + 5];
        uint8_t bits = buf[off + 6];
        uint8_t nfreq = buf[off + 7];
        out->printf(" formatType=%u ch=%u subframe=%u bits=%u freqType=%u",
                    buf[off + 3], channels, subframe, bits, nfreq);
        if (nfreq && len >= (uint8_t)(8 + 3 * nfreq)) {
          out->print(" rates=");
          for (uint8_t i = 0; i < nfreq; ++i) {
            uint8_t *f = buf + off + 8 + 3 * i;
            uint32_t hz = (uint32_t)f[0] | ((uint32_t)f[1] << 8) |
                          ((uint32_t)f[2] << 16);
            if (i) out->print(',');
            out->print(hz);
          }
        }
      }
      out->println();
    } else if (type == 0x25 && curClass == 0x01 && curSub == 0x02) {
      out->printf("USB AUDIO DESC:   CS_ENDPOINT subtype=0x%02X len=%u\n",
                  len >= 3 ? buf[off + 2] : 0xff, len);
    }
    off += len;
  }
  free(buf);
}

void PCMAudioCapture::diagnose(Print *out) {
  if (!out) out = &Serial;
  if (!ready()) {
    out->println("USB AUDIO DIAG: PCM2901 is not ready");
    return;
  }

  dumpConfiguration(out);

  uint8_t alt = 0xff;
  uint8_t rcode = getInterface(kCaptureInterface, &alt);
  out->printf("USB AUDIO DIAG: GET_INTERFACE IF=%u rcode=0x%02X alt=%u\n",
              kCaptureInterface, rcode, alt);

  uint32_t hz = 0;
  rcode = getSampleRate(&hz);
  out->printf("USB AUDIO DIAG: GET_CUR EP=0x%02X rcode=0x%02X rate=%lu\n",
              kCaptureEndpoint, rcode, (unsigned long)hz);
}

uint8_t PCMAudioCapture::Init(uint8_t parent, uint8_t port, bool lowspeed) {
  if (bAddress) return USB_ERROR_CLASS_INSTANCE_ALREADY_IN_USE;
  if (!pUsb) return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;
  if (lowspeed) return USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED;

  AddressPool &addrPool = pUsb->GetAddressPool();
  UsbDevice *p = addrPool.GetUsbDevicePtr(0);
  if (!p) return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;
  if (!p->epinfo) return USB_ERROR_EPINFO_IS_NULL;

  EpInfo *oldEp = p->epinfo;
  p->epinfo = epInfo;
  p->lowspeed = false;

  // Configuring() has already proved that a complete descriptor can be read
  // at address 0.  Check VID/PID before allocating an address because older
  // USB Host Shield drivers are also tried blindly for otherwise-unsupported
  // devices.  Without this check this class could momentarily claim them.
  USB_DEVICE_DESCRIPTOR addr0Desc = {};
  uint8_t rcode = pUsb->getDevDescr(
      0, 0, sizeof(addr0Desc), reinterpret_cast<uint8_t *>(&addr0Desc));
  p->epinfo = oldEp;
  if (rcode) return rcode;
  if (addr0Desc.idVendor != kVid || addr0Desc.idProduct != kPid)
    return USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED;

  epInfo[0].maxPktSize = addr0Desc.bMaxPacketSize0;

  bAddress = addrPool.AllocAddress(parent, false, port);
  if (!bAddress) return USB_ERROR_OUT_OF_ADDRESS_SPACE_IN_POOL;

  rcode = pUsb->setAddr(0, 0, bAddress);
  if (rcode) goto fail;

  p = addrPool.GetUsbDevicePtr(bAddress);
  if (!p) {
    rcode = USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;
    goto fail;
  }
  p->lowspeed = false;

  USB_DEVICE_DESCRIPTOR desc;
  rcode = pUsb->getDevDescr(
      bAddress, 0, sizeof(desc), reinterpret_cast<uint8_t *>(&desc));
  if (rcode) goto fail;

  if (desc.idVendor != kVid || desc.idProduct != kPid) {
    rcode = USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED;
    goto fail;
  }

  rcode = pUsb->setEpInfoEntry(bAddress, 2, epInfo);
  if (rcode) goto fail;

  rcode = pUsb->setConf(bAddress, 0, kConfiguration);
  if (rcode) goto fail;

  delay(20);
  {
    // v6 diagnostic: verify the actual AudioStreaming descriptors and the
    // alternate-setting state instead of assuming IF2/alt1 silently worked.
    // Keep diagnostic locals scoped so earlier goto fail paths do not cross
    // their initialization.
    dumpConfiguration(&Serial);

    uint8_t altBefore = 0xff;
    uint8_t getIfBefore = getInterface(kCaptureInterface, &altBefore);
    Serial.printf("USB AUDIO INIT: GET_INTERFACE before IF=%u rcode=0x%02X alt=%u\n",
                  kCaptureInterface, getIfBefore, altBefore);

    rcode = setInterface(kCaptureInterface, kCaptureAltSetting);
    Serial.printf("USB AUDIO INIT: SET_INTERFACE IF=%u alt=%u rcode=0x%02X\n",
                  kCaptureInterface, kCaptureAltSetting, rcode);
    if (rcode) goto fail;

    uint8_t altAfter = 0xff;
    uint8_t getIfAfter = getInterface(kCaptureInterface, &altAfter);
    Serial.printf("USB AUDIO INIT: GET_INTERFACE after  IF=%u rcode=0x%02X alt=%u\n",
                  kCaptureInterface, getIfAfter, altAfter);

    uint8_t setRateResult = setSampleRate48k();
    Serial.printf("USB AUDIO INIT: SET_CUR EP=0x%02X rate=48000 rcode=0x%02X\n",
                  kCaptureEndpoint, setRateResult);
    // Some UAC1 implementations are fixed at 48 kHz and may reject SET_CUR;
    // do not fail enumeration solely for that reason.

    uint32_t rateReadback = 0;
    uint8_t getRateResult = getSampleRate(&rateReadback);
    Serial.printf("USB AUDIO INIT: GET_CUR EP=0x%02X rcode=0x%02X rate=%lu\n",
                  kCaptureEndpoint, getRateResult, (unsigned long)rateReadback);
  }

  bReady = true;
  fCapture = false;
  resetStats();
  Serial.printf(
      "USB AUDIO: PCM2901 ready addr=%u IF=%u alt=%u EP=0x%02X maxpkt=%u "
      "format=48000/16/stereo\n",
      bAddress, kCaptureInterface, kCaptureAltSetting, kCaptureEndpoint,
      kCaptureMaxPacket);
  return 0;

fail:
  if (bAddress) {
    addrPool.FreeAddress(bAddress);
    bAddress = 0;
  }
  bReady = false;
  fCapture = false;
  return rcode;
}

uint8_t PCMAudioCapture::Release() {
  fCapture = false;
  bReady = false;
  if (bAddress && pUsb) {
    pUsb->GetAddressPool().FreeAddress(bAddress);
  }
  bAddress = 0;
  return 0;
}

bool PCMAudioCapture::start(Print *out) {
  if (!out) out = &Serial;
  if (!ready()) {
    out->println("USB AUDIO: PCM2901 is not ready");
    return false;
  }

  if (!captureBuf) {
    const size_t bytes = kCaptureSamples * sizeof(int16_t);
    captureBuf = static_cast<int16_t *>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!captureBuf) {
      out->printf("USB AUDIO: PSRAM allocation failed (%u bytes), free=%u\n",
                  (unsigned int)bytes,
                  (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
      return false;
    }
  }

  resetStats();
  // DELAYISO asks MAX3421E to defer an ISO transaction to the next frame
  // when there is not enough bus time left in the current frame.  This is
  // particularly useful when the firmware reaches this code late in a frame.
  if (pUsb) pUsb->regWr(rMODE, pUsb->regRd(rMODE) | bmDELAYISO);

  drainStaleReceiveFifo();
  startedMs = millis();
  fCapture = true;
  out->printf(
      "USB AUDIO: capture started, 48kHz 16-bit mono(left), %u samples / "
      "%u bytes / %us\n",
      (unsigned int)kCaptureSamples,
      (unsigned int)(kCaptureSamples * sizeof(int16_t)),
      (unsigned int)kCaptureSeconds);
  return true;
}

void PCMAudioCapture::stop(Print *out) {
  if (!out) out = &Serial;
  if (fCapture) {
    fCapture = false;
    finishedMs = millis();
  }
  status(out);
}


void PCMAudioCapture::setSofSync(bool enable, Print *out) {
  fSofSync = enable;
  if (!out) out = &Serial;
  out->printf("USB AUDIO: SOF sync=%d (%s ISO scheduling)\n",
              fSofSync ? 1 : 0, fSofSync ? "fresh-SOF" : "immediate");
}

bool PCMAudioCapture::waitFreshSof() {
  if (!pUsb) return false;

  // FRAMEIRQ is sticky.  Clearing any old indication first is important:
  // otherwise merely testing the bit does not synchronize us to a new frame.
  pUsb->regWr(rHIRQ, bmFRAMEIRQ);
  const uint32_t started = micros();
  while ((uint32_t)(micros() - started) < 2500U) {
    if (pUsb->regRd(rHIRQ) & bmFRAMEIRQ) {
      pUsb->regWr(rHIRQ, bmFRAMEIRQ);
      sofWaits++;
      return true;
    }
    yield();
  }
  sofTimeouts++;
  return false;
}

void PCMAudioCapture::dumpJerrRegisters(uint8_t result) {
  if (!pUsb || jerrDiagPrinted >= 8) return;
  jerrDiagPrinted++;

  // Read-only snapshot immediately after the failed transfer.  Do not clear
  // HIRQ here: receiveIsoPacket()/dispatchPkt() own the IRQ acknowledgements.
  const uint8_t hrsl = pUsb->regRd(rHRSL);
  const uint8_t hirq = pUsb->regRd(rHIRQ);
  const uint8_t mode = pUsb->regRd(rMODE);
  const uint8_t peraddr = pUsb->regRd(rPERADDR);
  const uint8_t hctl = pUsb->regRd(rHCTL);
  const uint8_t hxfr = pUsb->regRd(rHXFR);
  const uint8_t rcvbc = pUsb->regRd(rRCVBC);
  const uint8_t usbirq = pUsb->regRd(rUSBIRQ);
  const uint8_t usbctl = pUsb->regRd(rUSBCTL);
  Serial.printf(
      "USB AUDIO JERR[%u] t=%lums us=%lu result=%02X HRSL=%02X HIRQ=%02X "
      "MODE=%02X PERADDR=%02X HCTL=%02X HXFR=%02X RCVBC=%u USBIRQ=%02X "
      "USBCTL=%02X sync=%d\n",
      (unsigned)jerrDiagPrinted, (unsigned long)millis(),
      (unsigned long)micros(), result, hrsl, hirq, mode, peraddr, hctl, hxfr,
      (unsigned)rcvbc, usbirq, usbctl, fSofSync ? 1 : 0);
}

void PCMAudioCapture::freeBuffer(Print *out) {
  if (!out) out = &Serial;
  fCapture = false;
  if (captureBuf) {
    heap_caps_free(captureBuf);
    captureBuf = NULL;
  }
  resetStats();
  out->printf("USB AUDIO: PSRAM buffer freed; PSRAM free=%u\n",
              (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void PCMAudioCapture::status(Print *out) const {
  if (!out) out = &Serial;
  const size_t bytes = captureSamples * sizeof(int16_t);
  uint32_t elapsed = 0;
  if (startedMs) {
    const uint32_t end = fCapture ? millis() : (finishedMs ? finishedMs : millis());
    elapsed = end - startedMs;
  }
  out->printf(
      "USB AUDIO: ready=%d capture=%d addr=%u samples=%u/%u bytes=%u "
      "packets=%u errors=%u malformed=%u min=%d max=%d elapsed=%ums "
      "PSRAM_free=%u\n",
      ready() ? 1 : 0, fCapture ? 1 : 0, bAddress,
      (unsigned int)captureSamples, (unsigned int)kCaptureSamples,
      (unsigned int)bytes, (unsigned int)isoPackets,
      (unsigned int)isoErrors, (unsigned int)malformedBytes,
      captureSamples ? minSample : 0, captureSamples ? maxSample : 0,
      (unsigned int)elapsed,
      (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  // MAX3421E HRSL low nibble meanings from max3421e.h.  Print every
  // non-zero bucket so a failed experiment shows the actual bus result
  // rather than only one aggregate error count.
  static const char *const hrName[16] = {
      "SUCCESS", "BUSY", "BADREQ", "UNDEF", "NAK", "STALL",
      "TOGERR", "WRONGPID", "BADBC", "PIDERR", "PKTERR", "CRCERR",
      "KERR", "JERR", "TIMEOUT", "BABBLE"};
  out->print("USB AUDIO ISO results:");
  bool any = false;
  for (uint8_t i = 0; i < 16; ++i) {
    if (!isoResult[i]) continue;
    out->printf(" %s=%u", hrName[i], (unsigned int)isoResult[i]);
    any = true;
  }
  if (noRcvDav) {
    out->printf(" NO_RCVDAV=%u", (unsigned int)noRcvDav);
    any = true;
  }
  if (!any) out->print(" none");
  out->printf(" last=0x%02X\n", lastIsoError);
  out->printf("USB AUDIO ISO schedule: sync=%d SOF=%u timeout=%u\n",
              fSofSync ? 1 : 0, (unsigned int)sofWaits,
              (unsigned int)sofTimeouts);
  out->printf(
      "USB AUDIO ISO stream: rawbytes=%u fifo_chunks=%u hxfrdn=%u "
      "wait_timeout=%u MODE=%02X\n",
      (unsigned int)isoRawBytes, (unsigned int)isoFifoChunks,
      (unsigned int)isoHxfrDone, (unsigned int)isoWaitTimeouts,
      pUsb ? pUsb->regRd(rMODE) : 0);
}

void PCMAudioCapture::drainStaleReceiveFifo() {
  if (!pUsb) return;

  // If an earlier experiment left a receive buffer owned by the CPU, empty
  // it before starting a new ISO transaction.  Clearing RCVDAVIRQ hands the
  // buffer back to the MAX3421E.
  uint8_t guard = 0;
  while ((pUsb->regRd(rHIRQ) & bmRCVDAVIRQ) && guard++ < 4) {
    uint8_t n = pUsb->regRd(rRCVBC);
    uint8_t scratch[64];
    while (n) {
      uint8_t chunk = (n > sizeof(scratch)) ? sizeof(scratch) : n;
      pUsb->bytesRd(rRCVFIFO, chunk, scratch);
      n -= chunk;
    }
    pUsb->regWr(rHIRQ, bmRCVDAVIRQ);
  }
  pUsb->regWr(rHIRQ, bmHXFRDNIRQ);
}

uint8_t PCMAudioCapture::receiveIsoPacket(uint8_t *buf, uint8_t *len) {
  if (!buf || !len || !ready()) return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;
  *len = 0;

  if (fSofSync) {
    if (!waitFreshSof()) return 0xf1;
    if (!fCapture) return 0xf2;
  }

  // Do not use USB::dispatchPkt() here.  dispatchPkt() waits only for
  // HXFRDNIRQ and therefore cannot service RCVDAVIRQ while one >64-byte
  // isochronous packet is arriving.  MAX3421E has two 64-byte receive
  // buffers; each one must be emptied and released while the transaction is
  // in progress or the device cannot stream a 192-ish-byte audio frame.
  pUsb->regWr(rPERADDR, bAddress);

  // Release any stale receive buffer and clear an old completion indication.
  // In the normal path these bits are already clear, so this is cheap.
  if (pUsb->regRd(rHIRQ) & bmRCVDAVIRQ) drainStaleReceiveFifo();
  pUsb->regWr(rHIRQ, bmHXFRDNIRQ);

  // Launch one ISO IN on endpoint 2.  DELAYISO is enabled by start(), so a
  // launch too late in the current frame is automatically moved to the next
  // SOF by the MAX3421E.
  pUsb->regWr(rHXFR, tokISOIN | kCaptureEndpointNumber);

  uint16_t total = 0;
  const uint32_t t0 = micros();
  const uint32_t timeoutUs = 3000U;

  for (;;) {
    const uint8_t hirq = pUsb->regRd(rHIRQ);

    // RCVDAV may occur more than once for a single large ISO packet.  Drain
    // it before looking at HXFRDN because both IRQs can be set together for
    // the final receive-buffer fragment.
    if (hirq & bmRCVDAVIRQ) {
      uint8_t n = pUsb->regRd(rRCVBC);
      isoFifoChunks++;
      isoRawBytes += n;

      if (n) {
        uint8_t copy = n;
        if ((uint16_t)(total + copy) > kCaptureMaxPacket)
          copy = (uint8_t)(kCaptureMaxPacket - total);

        if (copy) {
          pUsb->bytesRd(rRCVFIFO, copy, buf + total);
          total += copy;
        }

        // RCVBC should normally be <=64.  Drain anything beyond the local
        // packet buffer defensively so the FIFO is always released cleanly.
        uint8_t left = n - copy;
        uint8_t scratch[64];
        while (left) {
          uint8_t chunk = (left > sizeof(scratch)) ? sizeof(scratch) : left;
          pUsb->bytesRd(rRCVFIFO, chunk, scratch);
          left -= chunk;
        }
      }

      // Hand this receive buffer back to the SIE.  If the other half of the
      // double buffer is already full, RCVDAVIRQ will assert again and the
      // loop above drains it immediately.
      pUsb->regWr(rHIRQ, bmRCVDAVIRQ);
      continue;
    }

    if (hirq & bmHXFRDNIRQ) {
      isoHxfrDone++;
      pUsb->regWr(rHIRQ, bmHXFRDNIRQ);

      // A final RCVDAV can become visible just as HXFRDN is acknowledged.
      // Give it a short chance and drain it before returning.
      for (uint8_t tail = 0; tail < 3; ++tail) {
        if (!(pUsb->regRd(rHIRQ) & bmRCVDAVIRQ)) break;
        uint8_t n = pUsb->regRd(rRCVBC);
        isoFifoChunks++;
        isoRawBytes += n;
        uint8_t copy = n;
        if ((uint16_t)(total + copy) > kCaptureMaxPacket)
          copy = (uint8_t)(kCaptureMaxPacket - total);
        if (copy) {
          pUsb->bytesRd(rRCVFIFO, copy, buf + total);
          total += copy;
        }
        uint8_t left = n - copy;
        uint8_t scratch[64];
        while (left) {
          uint8_t chunk = (left > sizeof(scratch)) ? sizeof(scratch) : left;
          pUsb->bytesRd(rRCVFIFO, chunk, scratch);
          left -= chunk;
        }
        pUsb->regWr(rHIRQ, bmRCVDAVIRQ);
      }

      *len = (uint8_t)total;
      uint8_t result = pUsb->regRd(rHRSL) & 0x0f;
      if (result == hrSUCCESS && total == 0) return 0xf0;
      return result;
    }

    if ((uint32_t)(micros() - t0) >= timeoutUs) {
      isoWaitTimeouts++;
      *len = (uint8_t)total;
      // Keep the MAX3421E receive side usable even when the transfer never
      // raises HXFRDNIRQ.  The next Poll() can then start from a clean state.
      drainStaleReceiveFifo();
      return hrTIMEOUT;
    }
  }
}

void PCMAudioCapture::consumeStereo16(const uint8_t *buf, uint8_t len) {
  if (!captureBuf || !buf || !len) return;

  // Stereo, signed 16-bit little-endian: Llo,Lhi,Rlo,Rhi.
  const uint8_t usable = len & ~0x03U;
  malformedBytes += len - usable;
  for (uint8_t i = 0; i < usable && captureSamples < kCaptureSamples; i += 4) {
    int16_t left = static_cast<int16_t>(
        static_cast<uint16_t>(buf[i]) |
        (static_cast<uint16_t>(buf[i + 1]) << 8));
    captureBuf[captureSamples++] = left;
    if (left < minSample) minSample = left;
    if (left > maxSample) maxSample = left;
  }

  if (captureSamples >= kCaptureSamples) {
    fCapture = false;
    finishedMs = millis();
    Serial.printf(
        "USB AUDIO: capture complete samples=%u packets=%u errors=%u "
        "elapsed=%ums min=%d max=%d\n",
        (unsigned int)captureSamples, (unsigned int)isoPackets,
        (unsigned int)isoErrors,
        (unsigned int)(finishedMs - startedMs), minSample, maxSample);
  }
}

uint8_t PCMAudioCapture::Poll() {
  if (!fCapture || !ready()) return 0;

  uint8_t packet[kCaptureMaxPacket];
  uint8_t len = 0;
  uint8_t rcode = receiveIsoPacket(packet, &len);
  if (rcode) {
    isoErrors++;
    lastIsoError = rcode;
    if (rcode == 0xf0) {
      noRcvDav++;
    } else if (rcode == 0xf1 || rcode == 0xf2) {
      // SOF timeout/stop-race are reported separately from MAX3421E HRSL.
    } else {
      isoResult[rcode & 0x0f]++;
      if ((rcode & 0x0f) == hrJERR) dumpJerrRegisters(rcode);
    }
    return 0;  // Do not make USB::Task() tear down the device for a lost frame.
  }
  isoResult[hrSUCCESS]++;
  if (len) {
    isoPackets++;
    consumeStereo16(packet, len);
  }
  return 0;
}
