/* Copyright (C) 2011 Circuits At Home, LTD. All rights reserved.

This software may be distributed and modified under the terms of the GNU
General Public License version 2 (GPL2) as published by the Free Software
Foundation and appearing in the file GPL2.TXT included in the packaging of
this file. Please note that GPL2 Section 2[b] requires that all works based
on this software must also be made publicly available under the terms of
the GPL2 ("Copyleft").

Contact information
-------------------

Circuits At Home, LTD
Web      :  http://www.circuitsathome.com
e-mail   :  support@circuitsathome.com
 */
#include "cdcacm.h"

extern int verbose;
#ifndef VERBOSE_USB
#define VERBOSE_USB 256
#endif

const uint8_t ACM::epDataInIndex = 1;
const uint8_t ACM::epDataOutIndex = 2;
const uint8_t ACM::epInterruptInIndex = 3;
const uint8_t ACM::epDataIn1Index = 4;
const uint8_t ACM::epDataOut1Index = 5;
const uint8_t ACM::epInterruptIn1Index = 6;

ACM::ACM(USB *p, CDCAsyncOper *pasync) :
pUsb(p),
pAsync(pasync),
bAddress(0),
bControlIface(0),
bDataIface(0),
bNumEP(1),
idVendor(0),
idProduct(0),
qNextPollTime(0),
bPollEnable(false),
ready(false) {
        _enhanced_status = enhanced_features(); // Set up features
        for(uint8_t i = 0; i < ACM_MAX_ENDPOINTS; i++) {
                epInfo[i].epAddr = 0;
                epInfo[i].maxPktSize = (i) ? 0 : 8;
                epInfo[i].bmSndToggle = 0;
                epInfo[i].bmRcvToggle = 0;
                epInfo[i].bmNakPower = ((i == epDataInIndex)||(i == epDataIn1Index)) ? USB_NAK_NOWAIT : USB_NAK_MAX_POWER;

        }
        if(pUsb)
                pUsb->RegisterDeviceClass(this);
}

uint8_t ACM::Init(uint8_t parent, uint8_t port, bool lowspeed) {

        const uint8_t constBufSize = sizeof (USB_DEVICE_DESCRIPTOR);

        uint8_t buf[constBufSize];
        USB_DEVICE_DESCRIPTOR * udd = reinterpret_cast<USB_DEVICE_DESCRIPTOR*>(buf);

        uint8_t rcode;
        UsbDevice *p = NULL;
        EpInfo *oldep_ptr = NULL;
        uint8_t num_of_conf; // number of configurations
        bool ats_mini_bulk_only = false;
        const uint32_t enum_start_ms = millis();
        uint8_t enum_retry_addr0 = 0;
        uint8_t enum_retry_addrn = 0;
        uint8_t saved_addr0_nak_power = 0;
        uint8_t saved_addrn_nak_power = 0;
        // Enumeration-only EP0 NAK limit.  Normal EP0 uses MAX_POWER (15),
        // which can keep one control transfer blocked for ~5 s.  Power 4
        // returns control to the explicit outer retry loop much sooner.
        const uint8_t enum_ep0_nak_power = 4;

        AddressPool &addrPool = pUsb->GetAddressPool();

        USB_HOST_SERIAL.println("USB ACM enum start");

        if (verbose & VERBOSE_USB) USBTRACE("ACM Init\r\n");

        if(bAddress)
                return USB_ERROR_CLASS_INSTANCE_ALREADY_IN_USE;

        // Get pointer to pseudo device with address 0 assigned
        p = addrPool.GetUsbDevicePtr(0);

        if(!p)
                return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;

        if(!p->epinfo) {
                USBTRACE("epinfo is null return\r\n");
                return USB_ERROR_EPINFO_IS_NULL;
        }

        // Save old pointer to EP_RECORD of address 0
        oldep_ptr = p->epinfo;

        // Temporary assign new pointer to epInfo to p->epinfo in order to avoid toggle inconsistence
        p->epinfo = epInfo;

        p->lowspeed = lowspeed;

        /*
         * During default-address enumeration only the first 8 bytes are
         * guaranteed to be readable with the provisional EP0 packet size.
         * Some composite devices, including QMX, NAK an immediate 18-byte
         * request here.  Read 8 bytes first, learn bMaxPacketSize0, assign an
         * address, then fetch the complete descriptor at the new address.
         */
        saved_addr0_nak_power = epInfo[0].bmNakPower;
        epInfo[0].bmNakPower = enum_ep0_nak_power;

        for(uint8_t retry = 0; retry < 20; retry++) {
                rcode = pUsb->getDevDescr(0, 0, 8, (uint8_t*)buf);
                if(rcode != hrNAK && rcode != hrJERR)
                        break;

                enum_retry_addr0++;
                USB_HOST_SERIAL.print("ACM getDevDescr addr=0 len=8 retry ");
                USB_HOST_SERIAL.print(retry + 1);
                USB_HOST_SERIAL.print(" rcode=0x");
                USB_HOST_SERIAL.println(rcode, HEX);
                delay(100);
        }

        USB_HOST_SERIAL.print("USB ACM enum addr0 retries=");
        USB_HOST_SERIAL.print(enum_retry_addr0);
        USB_HOST_SERIAL.print(" elapsed=");
        USB_HOST_SERIAL.print(millis() - enum_start_ms);
        USB_HOST_SERIAL.println(" ms");

        epInfo[0].bmNakPower = saved_addr0_nak_power;

        // Restore p->epinfo
        p->epinfo = oldep_ptr;

        if(rcode) {
                USB_HOST_SERIAL.print("ACM getDevDescr addr=0 len=8 failed rcode=0x");
                USB_HOST_SERIAL.println(rcode, HEX);
                goto FailGetDevDescr;
        }

        /*
         * A USB hub advertises bDeviceClass=0x09 in the first 8 bytes of the
         * device descriptor.  Reject it here while it is still at address 0.
         * Do not allocate/set/release an address in ACM; leave the device
         * untouched for USBHub::Init() in the same enumeration pass.
         */
        if(udd->bDeviceClass == 0x09) {
                USBTRACE("ACM skip USB hub at addr0\r\n");
                return USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED;
        }

        // Extract Max Packet Size from the first 8 descriptor bytes
        epInfo[0].maxPktSize = udd->bMaxPacketSize0;

        // Allocate new address according to device class
        bAddress = addrPool.AllocAddress(parent, false, port);

        if(!bAddress)
                return USB_ERROR_OUT_OF_ADDRESS_SPACE_IN_POOL;

        // Assign new address to the device
        rcode = pUsb->setAddr(0, 0, bAddress);

        if(rcode) {
                p->lowspeed = false;
                addrPool.FreeAddress(bAddress);
                bAddress = 0;
                USBTRACE2("cdcacm init setAddr:", rcode);
                return rcode;
        }

        USBTRACE2("Addr:", bAddress);

        p->lowspeed = false;

        p = addrPool.GetUsbDevicePtr(bAddress);

        if(!p)
                return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;

        p->lowspeed = lowspeed;

        // Read the complete descriptor now that EP0 size/address are known.
        // A reconnecting QMX can need additional settling time after SET_ADDRESS.
        // Until setEpInfoEntry() below, this address uses the shared default EP0
        // record.  Shorten only this descriptor phase, then restore it.
        saved_addrn_nak_power = p->epinfo[0].bmNakPower;
        p->epinfo[0].bmNakPower = enum_ep0_nak_power;

        for(uint8_t retry = 0; retry < 20; retry++) {
                rcode = pUsb->getDevDescr(bAddress, 0, constBufSize, (uint8_t*)buf);
                if(rcode != hrNAK && rcode != hrJERR)
                        break;

                USB_HOST_SERIAL.print("ACM getDevDescr addr=");
                USB_HOST_SERIAL.print(bAddress);
                USB_HOST_SERIAL.print(" len=");
                USB_HOST_SERIAL.print(constBufSize);
                enum_retry_addrn++;
                USB_HOST_SERIAL.print(" retry ");
                USB_HOST_SERIAL.print(retry + 1);
                USB_HOST_SERIAL.print(" rcode=0x");
                USB_HOST_SERIAL.println(rcode, HEX);
                delay(100);
        }

        USB_HOST_SERIAL.print("USB ACM enum addr");
        USB_HOST_SERIAL.print(bAddress);
        USB_HOST_SERIAL.print(" retries=");
        USB_HOST_SERIAL.print(enum_retry_addrn);
        USB_HOST_SERIAL.print(" elapsed=");
        USB_HOST_SERIAL.print(millis() - enum_start_ms);
        USB_HOST_SERIAL.println(" ms");

        p->epinfo[0].bmNakPower = saved_addrn_nak_power;

        if(rcode) {
                USB_HOST_SERIAL.print("ACM getDevDescr addr=");
                USB_HOST_SERIAL.print(bAddress);
                USB_HOST_SERIAL.print(" len=");
                USB_HOST_SERIAL.print(constBufSize);
                USB_HOST_SERIAL.print(" failed rcode=0x");
                USB_HOST_SERIAL.println(rcode, HEX);
                goto FailGetDevDescr;
        }

        num_of_conf = udd->bNumConfigurations;
        idVendor = udd->idVendor;
        idProduct = udd->idProduct;
        USBTRACE2("ACM VID:", idVendor);
        USBTRACE2("ACM PID:", idProduct);

        // Assign epInfo to epinfo pointer
        rcode = pUsb->setEpInfoEntry(bAddress, 1, epInfo);

        if(rcode)
                goto FailSetDevTblEntry;

        if (verbose & VERBOSE_USB) USBTRACE2("cdcacm init NC:", num_of_conf);

        for(uint8_t i = 0; i < num_of_conf; i++) {
                ConfigDescParser< USB_CLASS_COM_AND_CDC_CTRL,
                        CDC_SUBCLASS_ACM,
                        CDC_PROTOCOL_ITU_T_V_250,
                        CP_MASK_COMPARE_CLASS |
                        CP_MASK_COMPARE_SUBCLASS |
                        CP_MASK_COMPARE_PROTOCOL > CdcControlParser(this);

                ConfigDescParser<USB_CLASS_CDC_DATA, 0, 0,
                        CP_MASK_COMPARE_CLASS> CdcDataParser(this);

                /*
                 * QMX may temporarily NAK configuration-descriptor reads just
                 * after the address has been assigned.  Retry only NAK; other
                 * errors still abort immediately.  Both parsers need the same
                 * protection because each getConfDescr() performs a separate
                 * control transfer of the complete composite configuration.
                 */
                for(uint8_t retry = 0; retry < 10; retry++) {
                        rcode = pUsb->getConfDescr(bAddress, 0, i, &CdcControlParser);
                        if(rcode != hrNAK)
                                break;
                        USBTRACE2("ACM control getConf NAK retry:", retry + 1);
                        delay(50);
                }

                if(rcode)
                        goto FailGetConfDescr;

                for(uint8_t retry = 0; retry < 10; retry++) {
                        rcode = pUsb->getConfDescr(bAddress, 0, i, &CdcDataParser);
                        if(rcode != hrNAK)
                                break;
                        USBTRACE2("ACM data getConf NAK retry:", retry + 1);
                        delay(50);
                }

                if(rcode)
                        goto FailGetConfDescr;

                if(bNumEP > 1)
                        break;
        } // for

        /*
         * Normal CDC ACM devices expose EP0 + interrupt IN + bulk IN + bulk OUT
         * (bNumEP >= 4).  ATS Mini (ESP32-S3 TinyUSB, VID 0x303A PID 0x1001)
         * exposes only the two bulk data endpoints for its USB Ad hoc stream,
         * which is sufficient for DVPlogger's read/write use.
         *
         * Keep the original >=4 requirement for every other device so QMX and
         * the existing generic ACM paths are unaffected.
         */
        ats_mini_bulk_only =
                (idVendor == 0x303A && idProduct == 0x1001 &&
                 bNumEP >= 3 &&
                 epInfo[epDataInIndex].epAddr != 0 &&
                 epInfo[epDataOutIndex].epAddr != 0);

        if(bNumEP < 4 && !ats_mini_bulk_only) {
                USBTRACE2("cdcacm init, usb dev conf error dev not supported numep:", bNumEP);
                /*
                 * We already assigned a USB address while probing this device.
                 * Returning DEVICE_NOT_SUPPORTED without undoing that leaves this
                 * ACM instance consumed by a non-CDC device (for example the
                 * IC-705 external USB audio codec 08BB:2901), so a later CDC
                 * device can never use the instance.  Put the rejected device
                 * back at address 0, release our pool entry/state, and let the
                 * USB core try another class driver/default addressing.
                 */
                const uint8_t rejected_addr = bAddress;
                pUsb->setAddr(rejected_addr, 0, 0);
                Release();
                return USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED;
        }

        if(ats_mini_bulk_only) {
                USBTRACE2("cdcacm init ATS-MINI bulk-only accepted numep:", bNumEP);
        }

        // Assign epInfo to epinfo pointer
        rcode = pUsb->setEpInfoEntry(bAddress, bNumEP, epInfo);

        if (verbose & VERBOSE_USB) USBTRACE2("cdcacm init Conf:", bConfNum);

        // Set Configuration Value
        rcode = pUsb->setConf(bAddress, 0, bConfNum);

        if(rcode)
                goto FailSetConfDescr;

        // Set up features status
        _enhanced_status = enhanced_features();
        half_duplex(false);
        autoflowRTS(false);
        autoflowDSR(false);
        autoflowXON(false);
        wide(false); // Always false, because this is only available in custom mode.
        rcode = pAsync->OnInit(this);

        if(rcode)
                goto FailOnInit;

        if (verbose & VERBOSE_USB) USBTRACE("cdcacm init ACM configured\r\n");

        ready = true;

        USB_HOST_SERIAL.print("USB ACM enum done addr=");
        USB_HOST_SERIAL.print(bAddress);
        USB_HOST_SERIAL.print(" VID=");
        USB_HOST_SERIAL.print(idVendor, HEX);
        USB_HOST_SERIAL.print(" PID=");
        USB_HOST_SERIAL.print(idProduct, HEX);
        USB_HOST_SERIAL.print(" retries=");
        USB_HOST_SERIAL.print(enum_retry_addr0);
        USB_HOST_SERIAL.print("/");
        USB_HOST_SERIAL.print(enum_retry_addrn);
        USB_HOST_SERIAL.print(" total=");
        USB_HOST_SERIAL.print(millis() - enum_start_ms);
        USB_HOST_SERIAL.println(" ms");

        //bPollEnable = true;

        //USBTRACE("Poll enabled\r\n");
        return 0;

FailGetDevDescr:
#ifdef DEBUG_USB_HOST
        NotifyFailGetDevDescr();
        goto Fail;
#endif

FailSetDevTblEntry:
#ifdef DEBUG_USB_HOST
        NotifyFailSetDevTblEntry();
        goto Fail;
#endif

FailGetConfDescr:
#ifdef DEBUG_USB_HOST
        NotifyFailGetConfDescr();
        goto Fail;
#endif

FailSetConfDescr:
#ifdef DEBUG_USB_HOST
        NotifyFailSetConfDescr();
        goto Fail;
#endif

FailOnInit:
#ifdef DEBUG_USB_HOST
        USBTRACE("OnInit:");
#endif

#ifdef DEBUG_USB_HOST
Fail:
        NotifyFail(rcode);
#endif
        USB_HOST_SERIAL.print("USB ACM enum FAILED rcode=0x");
        USB_HOST_SERIAL.print(rcode, HEX);
        USB_HOST_SERIAL.print(" addr=");
        USB_HOST_SERIAL.print(bAddress);
        USB_HOST_SERIAL.print(" retries=");
        USB_HOST_SERIAL.print(enum_retry_addr0);
        USB_HOST_SERIAL.print("/");
        USB_HOST_SERIAL.print(enum_retry_addrn);
        USB_HOST_SERIAL.print(" total=");
        USB_HOST_SERIAL.print(millis() - enum_start_ms);
        USB_HOST_SERIAL.println(" ms");
        Release();
        return rcode;
}

void ACM::EndpointXtract(uint8_t conf, uint8_t iface, uint8_t alt __attribute__((unused)), uint8_t proto __attribute__((unused)), const USB_ENDPOINT_DESCRIPTOR *pep) {
        //ErrorMessage<uint8_t > (PSTR("Conf.Val"), conf);
        //ErrorMessage<uint8_t > (PSTR("Iface Num"), iface);
        //ErrorMessage<uint8_t > (PSTR("Alt.Set"), alt);
  //  if (iface != bControlIface && iface != bDataIface) return;  // to support multiple interface
  uint8_t index = 0; // endpoint index

        bConfNum = conf;

	//        uint8_t index;
	if (verbose & VERBOSE_USB) USBTRACE2("EndPointExtract() bmAttributes:",pep->bmAttributes);
	if (verbose & VERBOSE_USB) USBTRACE2("EndPointExtract() bmEndpointAddress:",pep->bEndpointAddress);
        if((pep->bmAttributes & bmUSB_TRANSFER_TYPE) == USB_TRANSFER_TYPE_INTERRUPT && (pep->bEndpointAddress & 0x80) == 0x80) {
          if ((pep->bEndpointAddress&0xf)<=3) {
            index = epInterruptInIndex;
            // First CDC ACM control interface.  IC-705 USB(A) is IF 0.
            bControlIface = iface;
          } else {
            index = epInterruptIn1Index;
          }
        }
        else if((pep->bmAttributes & bmUSB_TRANSFER_TYPE) == USB_TRANSFER_TYPE_BULK) {
          if ((pep->bEndpointAddress&0xf)<=3) {
            index = ((pep->bEndpointAddress & 0x80) == 0x80) ? epDataInIndex : epDataOutIndex;
            // First CDC ACM data interface.  IC-705 USB(A) is IF 1.
            bDataIface = iface;
          } else {
            index = ((pep->bEndpointAddress & 0x80) == 0x80) ? epDataIn1Index : epDataOut1Index;
          }
        }
        else {
	  USBTRACE("EndPointExtract return");
                return;
	}
	if (verbose & VERBOSE_USB) USBTRACE2("EndPointExtract() index:",index);

	
        // Fill in the endpoint info structure
        epInfo[index].epAddr = (pep->bEndpointAddress & 0x0F);
        epInfo[index].maxPktSize = (uint8_t)pep->wMaxPacketSize;
        epInfo[index].bmSndToggle = 0;
        epInfo[index].bmRcvToggle = 0;

        bNumEP++;
        if (verbose & VERBOSE_USB) USBTRACE2("EndPointExtractbNumEP:",bNumEP);
        PrintEndpointDescriptor(pep);
}

uint8_t ACM::Release() {
        ready = false;
        pUsb->GetAddressPool().FreeAddress(bAddress);

        bControlIface = 0;
        bDataIface = 0;
        bNumEP = 1;
        idVendor = 0;
        idProduct = 0;

        bAddress = 0;
        qNextPollTime = 0;
        bPollEnable = false;

        // Do not retain endpoint assignments from a previously attached
        // multi-port ACM device.  Otherwise a later one-port device can be
        // polled through a stale second-port endpoint.
        for(uint8_t i = 1; i < ACM_MAX_ENDPOINTS; i++) {
                epInfo[i].epAddr = 0;
                epInfo[i].maxPktSize = 0;
                epInfo[i].bmSndToggle = 0;
                epInfo[i].bmRcvToggle = 0;
        }
        return 0;
}

uint8_t ACM::Poll() {
        //uint8_t rcode = 0;
        //if(!bPollEnable)
        //        return 0;
        //return rcode;
        return 0;
}

uint8_t ACM::RcvData(uint16_t *bytes_rcvd, uint8_t *dataptr) {
        const bool qmx_phys_diag =
                ((verbose & VERBOSE_USB) &&
                 idVendor == 0x0483 && idProduct == 0xA34C);
        const uint8_t ep = epInfo[epDataInIndex].epAddr;
        const uint16_t requested = bytes_rcvd ? *bytes_rcvd : 0;

        uint8_t rv = pUsb->inTransfer(bAddress, ep, bytes_rcvd, dataptr);

        if(qmx_phys_diag && rv == 0 && bytes_rcvd && *bytes_rcvd) {
                USB_HOST_SERIAL.print("[USBPHYS-RX] addr=");
                USB_HOST_SERIAL.print(bAddress);
                USB_HOST_SERIAL.print(" ep=0x");
                USB_HOST_SERIAL.print(ep | 0x80, HEX);
                USB_HOST_SERIAL.print(" requested=");
                USB_HOST_SERIAL.print(requested);
                USB_HOST_SERIAL.print(" len=");
                USB_HOST_SERIAL.print(*bytes_rcvd);
                USB_HOST_SERIAL.print(" rcode=0x");
                USB_HOST_SERIAL.println(rv, HEX);

                USB_HOST_SERIAL.print("[USBPHYS-RX] HEX ");
                for(uint16_t i = 0; i < *bytes_rcvd; i++) {
                        if(dataptr[i] < 0x10)
                                USB_HOST_SERIAL.print('0');
                        USB_HOST_SERIAL.print(dataptr[i], HEX);
                        USB_HOST_SERIAL.print(' ');
                }
                USB_HOST_SERIAL.println();

                USB_HOST_SERIAL.print("[USBPHYS-RX] ASCII \"");
                for(uint16_t i = 0; i < *bytes_rcvd; i++) {
                        const uint8_t c = dataptr[i];
                        USB_HOST_SERIAL.print((c >= 0x20 && c <= 0x7e) ? (char)c : '.');
                }
                USB_HOST_SERIAL.println("\"");
        } else if(qmx_phys_diag && rv && rv != hrNAK) {
                USB_HOST_SERIAL.print("[USBPHYS-RX] addr=");
                USB_HOST_SERIAL.print(bAddress);
                USB_HOST_SERIAL.print(" ep=0x");
                USB_HOST_SERIAL.print(ep | 0x80, HEX);
                USB_HOST_SERIAL.print(" requested=");
                USB_HOST_SERIAL.print(requested);
                USB_HOST_SERIAL.print(" len=");
                USB_HOST_SERIAL.print(bytes_rcvd ? *bytes_rcvd : 0);
                USB_HOST_SERIAL.print(" rcode=0x");
                USB_HOST_SERIAL.println(rv, HEX);
        }

        if(rv && rv != hrNAK) {
                Release();
        }
        return rv;
}
uint8_t ACM::RcvData1(uint16_t *bytes_rcvd, uint8_t *dataptr) {
        uint8_t rv = pUsb->inTransfer(bAddress, epInfo[epDataIn1Index].epAddr, bytes_rcvd, dataptr);
        if(rv && rv != hrNAK) {
                Release();
        }
        return rv;
}

uint8_t ACM::SndData(uint16_t nbytes, uint8_t *dataptr) {
        const bool qmx_phys_diag =
                ((verbose & VERBOSE_USB) &&
                 idVendor == 0x0483 && idProduct == 0xA34C);
        const uint8_t ep = epInfo[epDataOutIndex].epAddr;

        if(qmx_phys_diag) {
                USB_HOST_SERIAL.print("[USBPHYS-TX] PRE addr=");
                USB_HOST_SERIAL.print(bAddress);
                USB_HOST_SERIAL.print(" ep=0x");
                USB_HOST_SERIAL.print(ep, HEX);
                USB_HOST_SERIAL.print(" len=");
                USB_HOST_SERIAL.println(nbytes);

                USB_HOST_SERIAL.print("[USBPHYS-TX] HEX ");
                for(uint16_t i = 0; i < nbytes; i++) {
                        if(dataptr[i] < 0x10)
                                USB_HOST_SERIAL.print('0');
                        USB_HOST_SERIAL.print(dataptr[i], HEX);
                        USB_HOST_SERIAL.print(' ');
                }
                USB_HOST_SERIAL.println();

                USB_HOST_SERIAL.print("[USBPHYS-TX] ASCII \"");
                for(uint16_t i = 0; i < nbytes; i++) {
                        const uint8_t c = dataptr[i];
                        USB_HOST_SERIAL.print((c >= 0x20 && c <= 0x7e) ? (char)c : '.');
                }
                USB_HOST_SERIAL.println("\"");
        }

        uint8_t rv = pUsb->outTransfer(bAddress, ep, nbytes, dataptr);

        if(qmx_phys_diag) {
                USB_HOST_SERIAL.print("[USBPHYS-TX] POST addr=");
                USB_HOST_SERIAL.print(bAddress);
                USB_HOST_SERIAL.print(" ep=0x");
                USB_HOST_SERIAL.print(ep, HEX);
                USB_HOST_SERIAL.print(" len=");
                USB_HOST_SERIAL.print(nbytes);
                USB_HOST_SERIAL.print(" rcode=0x");
                USB_HOST_SERIAL.println(rv, HEX);
        }


        /*
         * Timing experiment for QMX: the packet-dump diagnostics made
         * reconnect/switching reliable.  Reproduce only a small part of
         * that timing effect without changing CDC control state.
         */
        if(rv == 0 && idVendor == 0x0483 && idProduct == 0xA34C) {
	  ::delay(1);
	  
        }

        if(rv && rv != hrNAK) {
                Release();
        }
        return rv;
}
uint8_t ACM::SndData1(uint16_t nbytes, uint8_t *dataptr) {
        uint8_t rv = pUsb->outTransfer(bAddress, epInfo[epDataOut1Index].epAddr, nbytes, dataptr);
        if(rv && rv != hrNAK) {
                Release();
        }
        return rv;
}

uint8_t ACM::SetCommFeature(uint16_t fid, uint8_t nbytes, uint8_t *dataptr) {
        uint8_t rv = ( pUsb->ctrlReq(bAddress, 0, bmREQ_CDCOUT, CDC_SET_COMM_FEATURE, (fid & 0xff), (fid >> 8), bControlIface, nbytes, nbytes, dataptr, NULL));
        if(rv && rv != hrNAK) {
                Release();
        }
        return rv;
}

uint8_t ACM::GetCommFeature(uint16_t fid, uint8_t nbytes, uint8_t *dataptr) {
        uint8_t rv = ( pUsb->ctrlReq(bAddress, 0, bmREQ_CDCIN, CDC_GET_COMM_FEATURE, (fid & 0xff), (fid >> 8), bControlIface, nbytes, nbytes, dataptr, NULL));
        if(rv && rv != hrNAK) {
                Release();
        }
        return rv;
}

uint8_t ACM::ClearCommFeature(uint16_t fid) {
        uint8_t rv = ( pUsb->ctrlReq(bAddress, 0, bmREQ_CDCOUT, CDC_CLEAR_COMM_FEATURE, (fid & 0xff), (fid >> 8), bControlIface, 0, 0, NULL, NULL));
        if(rv && rv != hrNAK) {
                Release();
        }
        return rv;
}

uint8_t ACM::SetLineCoding(const LINE_CODING *dataptr) {
        uint8_t rv = ( pUsb->ctrlReq(bAddress, 0, bmREQ_CDCOUT, CDC_SET_LINE_CODING, 0x00, 0x00, bControlIface, sizeof (LINE_CODING), sizeof (LINE_CODING), (uint8_t*)dataptr, NULL));
        if(rv && rv != hrNAK) {
                Release();
        }
        return rv;
}

uint8_t ACM::GetLineCoding(LINE_CODING *dataptr) {
        uint8_t rv = ( pUsb->ctrlReq(bAddress, 0, bmREQ_CDCIN, CDC_GET_LINE_CODING, 0x00, 0x00, bControlIface, sizeof (LINE_CODING), sizeof (LINE_CODING), (uint8_t*)dataptr, NULL));
        if(rv && rv != hrNAK) {
                Release();
        }
        return rv;
}

uint8_t ACM::SetControlLineState(uint8_t state) {
        uint8_t rv = ( pUsb->ctrlReq(bAddress, 0, bmREQ_CDCOUT, CDC_SET_CONTROL_LINE_STATE, state, 0, bControlIface, 0, 0, NULL, NULL));
        if(rv && rv != hrNAK) {
                Release();
        }
        return rv;
}

uint8_t ACM::SetControlLineStateOnInterface(uint8_t iface, uint8_t state) {
        uint8_t rv = ( pUsb->ctrlReq(bAddress, 0, bmREQ_CDCOUT,
                CDC_SET_CONTROL_LINE_STATE, state, 0, iface,
                0, 0, NULL, NULL));
        // This routine is primarily a diagnostic probe.  Do not Release()
        // the whole ACM device when a candidate interface STALLs; the caller
        // may immediately try another interface number.
        return rv;
}

uint8_t ACM::SendBreak(uint16_t duration) {
        uint8_t rv = ( pUsb->ctrlReq(bAddress, 0, bmREQ_CDCOUT, CDC_SEND_BREAK, (duration & 0xff), (duration >> 8), bControlIface, 0, 0, NULL, NULL));
        if(rv && rv != hrNAK) {
                Release();
        }
        return rv;
}

void ACM::PrintEndpointDescriptor(const USB_ENDPOINT_DESCRIPTOR* ep_ptr) {
        Notify(PSTR("Endpoint descriptor:"), 0x80);
        Notify(PSTR("\r\nLength:\t\t"), 0x80);
        D_PrintHex<uint8_t > (ep_ptr->bLength, 0x80);
        Notify(PSTR("\r\nType:\t\t"), 0x80);
        D_PrintHex<uint8_t > (ep_ptr->bDescriptorType, 0x80);
        Notify(PSTR("\r\nAddress:\t"), 0x80);
        D_PrintHex<uint8_t > (ep_ptr->bEndpointAddress, 0x80);
        Notify(PSTR("\r\nAttributes:\t"), 0x80);
        D_PrintHex<uint8_t > (ep_ptr->bmAttributes, 0x80);
        Notify(PSTR("\r\nMaxPktSize:\t"), 0x80);
        D_PrintHex<uint16_t > (ep_ptr->wMaxPacketSize, 0x80);
        Notify(PSTR("\r\nPoll Intrv:\t"), 0x80);
        D_PrintHex<uint8_t > (ep_ptr->bInterval, 0x80);
        Notify(PSTR("\r\n"), 0x80);
}
