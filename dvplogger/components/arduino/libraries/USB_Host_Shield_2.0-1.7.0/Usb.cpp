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
/* USB functions */

#include "Usb.h"

static uint8_t usb_error = 0;
static uint8_t usb_task_state;

/* constructor */
USB::USB() : bmHubPre(0) {
        usb_task_state = USB_DETACHED_SUBSTATE_INITIALIZE; //set up state machine
        init();
}

/* Initialize data structures */
void USB::init() {
        //devConfigIndex = 0;
        bmHubPre = 0;
}

uint8_t USB::getUsbTaskState(void) {
        return ( usb_task_state);
}

void USB::setUsbTaskState(uint8_t state) {
        usb_task_state = state;
}

EpInfo* USB::getEpInfoEntry(uint8_t addr, uint8_t ep) {
        UsbDevice *p = addrPool.GetUsbDevicePtr(addr);

        if(!p || !p->epinfo)
                return NULL;

        EpInfo *pep = p->epinfo;

        for(uint8_t i = 0; i < p->epcount; i++) {
                if((pep)->epAddr == ep)
                        return pep;

                pep++;
        }
        return NULL;
}

/* set device table entry */

/* each device is different and has different number of endpoints. This function plugs endpoint record structure, defined in application, to devtable */
uint8_t USB::setEpInfoEntry(uint8_t addr, uint8_t epcount, EpInfo* eprecord_ptr) {
        if(!eprecord_ptr)
                return USB_ERROR_INVALID_ARGUMENT;

        UsbDevice *p = addrPool.GetUsbDevicePtr(addr);

        if(!p)
                return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;

        p->address.devAddress = addr;
        p->epinfo = eprecord_ptr;
        p->epcount = epcount;

        return 0;
}

uint8_t USB::SetAddress(uint8_t addr, uint8_t ep, EpInfo **ppep, uint16_t *nak_limit) {
        UsbDevice *p = addrPool.GetUsbDevicePtr(addr);

        if(!p)
                return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;

        if(!p->epinfo)
                return USB_ERROR_EPINFO_IS_NULL;

        *ppep = getEpInfoEntry(addr, ep);

        if(!*ppep)
                return USB_ERROR_EP_NOT_FOUND_IN_TBL;

        *nak_limit = (0x0001UL << (((*ppep)->bmNakPower > USB_NAK_MAX_POWER) ? USB_NAK_MAX_POWER : (*ppep)->bmNakPower));
        (*nak_limit)--;
        /*
          USBTRACE2("\r\nAddress: ", addr);
          USBTRACE2(" EP: ", ep);
          USBTRACE2(" NAK Power: ",(*ppep)->bmNakPower);
          USBTRACE2(" NAK Limit: ", nak_limit);
          USBTRACE("\r\n");
         */
        regWr(rPERADDR, addr); //set peripheral address

        uint8_t mode = regRd(rMODE);

        //Serial.print("\r\nMode: ");
        //Serial.println( mode, HEX);
        //Serial.print("\r\nLS: ");
        //Serial.println(p->lowspeed, HEX);



        // Set bmLOWSPEED and bmHUBPRE in case of low-speed device, reset them otherwise
        regWr(rMODE, (p->lowspeed) ? mode | bmLOWSPEED | bmHubPre : mode & ~(bmHUBPRE | bmLOWSPEED));

        return 0;
}

/* Control transfer. Sets address, endpoint, fills control packet with necessary data, dispatches control packet, and initiates bulk IN transfer,   */
/* depending on request. Actual requests are defined as inlines                                                                                      */
/* return codes:                */
/* 00       =   success         */

/* 01-0f    =   non-zero HRSLT  */
uint8_t USB::ctrlReq(uint8_t addr, uint8_t ep, uint8_t bmReqType, uint8_t bRequest, uint8_t wValLo, uint8_t wValHi,
        uint16_t wInd, uint16_t total, uint16_t nbytes, uint8_t* dataptr, USBReadParser *p) {
        bool direction = false; //request direction, IN or OUT
        uint8_t rcode;
        SETUP_PKT setup_pkt;

        EpInfo *pep = NULL;
        uint16_t nak_limit = 0;

        rcode = SetAddress(addr, ep, &pep, &nak_limit);

        if(rcode)
                return rcode;

        direction = ((bmReqType & 0x80) > 0);

        /* fill in setup packet */
        setup_pkt.ReqType_u.bmRequestType = bmReqType;
        setup_pkt.bRequest = bRequest;
        setup_pkt.wVal_u.wValueLo = wValLo;
        setup_pkt.wVal_u.wValueHi = wValHi;
        setup_pkt.wIndex = wInd;
        setup_pkt.wLength = total;

        bytesWr(rSUDFIFO, 8, (uint8_t*) & setup_pkt); //transfer to setup packet FIFO

        rcode = dispatchPkt(tokSETUP, ep, nak_limit); //dispatch packet

        if(rcode) //return HRSLT if not zero
                return ( rcode);

        if(dataptr != NULL) //data stage, if present
        {
                if(direction) //IN transfer
                {
                        uint16_t left = total;

                        pep->bmRcvToggle = 1; //bmRCVTOG1;

                        while(left) {
                                // Bytes read into buffer
#if defined(ESP8266) || defined(ESP32)
                        yield(); // needed in order to reset the watchdog timer on the ESP8266
#endif
                                uint16_t read = nbytes;
                                //uint16_t read = (left<nbytes) ? left : nbytes;

                                rcode = InTransfer(pep, nak_limit, &read, dataptr);
                                if(rcode == hrTOGERR) {
                                        // yes, we flip it wrong here so that next time it is actually correct!
                                        pep->bmRcvToggle = (regRd(rHRSL) & bmSNDTOGRD) ? 0 : 1;
                                        continue;
                                }

                                if(rcode)
                                        return rcode;

                                // Invoke callback function if inTransfer completed successfully and callback function pointer is specified
                                if(!rcode && p)
                                        ((USBReadParser*)p)->Parse(read, dataptr, total - left);

                                left -= read;

                                /*
                                 * Some full-speed composite devices (QMX in
                                 * particular) need one or more SOF intervals
                                 * before they can supply the next packet of a
                                 * long control-IN transfer.  Without this pause
                                 * the first 64-byte descriptor packet succeeds
                                 * but the following IN token repeatedly returns
                                 * NAK.  This path is used by streaming descriptor
                                 * parsers; a 2 ms pause is harmless during USB
                                 * enumeration and avoids restarting the entire
                                 * GET_DESCRIPTOR request.
                                 */
                                if(p && left && read == nbytes)
                                        delay(2);

                                if(read < nbytes)
                                        break;
                        }
                } else //OUT transfer
                {
                        pep->bmSndToggle = 1; //bmSNDTOG1;
                        rcode = OutTransfer(pep, nak_limit, nbytes, dataptr);
                }
                if(rcode) //return error
                        return ( rcode);
        }
        // Status stage
        return dispatchPkt((direction) ? tokOUTHS : tokINHS, ep, nak_limit); //GET if direction
}

/* IN transfer to arbitrary endpoint. Assumes PERADDR is set. Handles multiple packets if necessary. Transfers 'nbytes' bytes. */
/* Keep sending INs and writes data to memory area pointed by 'data'                                                           */

/* rcode 0 if no errors. rcode 01-0f is relayed from dispatchPkt(). Rcode f0 means RCVDAVIRQ error,
            fe USB xfer timeout */
uint8_t USB::inTransfer(uint8_t addr, uint8_t ep, uint16_t *nbytesptr, uint8_t* data, uint8_t bInterval /*= 0*/) {
        EpInfo *pep = NULL;
        uint16_t nak_limit = 0;

        uint8_t rcode = SetAddress(addr, ep, &pep, &nak_limit);

        if(rcode) {
                USBTRACE3("(USB::InTransfer) SetAddress Failed ", rcode, 0x81);
                USBTRACE3("(USB::InTransfer) addr requested ", addr, 0x81);
                USBTRACE3("(USB::InTransfer) ep requested ", ep, 0x81);
                return rcode;
        }
        return InTransfer(pep, nak_limit, nbytesptr, data, bInterval);
}

uint8_t USB::InTransfer(EpInfo *pep, uint16_t nak_limit, uint16_t *nbytesptr, uint8_t* data, uint8_t bInterval /*= 0*/) {
        uint8_t rcode = 0;
        uint8_t pktsize;

        uint16_t nbytes = *nbytesptr;
        //printf("Requesting %i bytes ", nbytes);
        uint8_t maxpktsize = pep->maxPktSize;

        *nbytesptr = 0;
        regWr(rHCTL, (pep->bmRcvToggle) ? bmRCVTOG1 : bmRCVTOG0); //set toggle value

        // use a 'break' to exit this loop
        while(1) {
#if defined(ESP8266) || defined(ESP32)
                        yield(); // needed in order to reset the watchdog timer on the ESP8266
#endif
                rcode = dispatchPkt(tokIN, pep->epAddr, nak_limit); //IN packet to EP-'endpoint'. Function takes care of NAKS.
                if(rcode == hrTOGERR) {
                        // yes, we flip it wrong here so that next time it is actually correct!
                        pep->bmRcvToggle = (regRd(rHRSL) & bmRCVTOGRD) ? 0 : 1;
                        regWr(rHCTL, (pep->bmRcvToggle) ? bmRCVTOG1 : bmRCVTOG0); //set toggle value
                        continue;
                }
                if(rcode) {
                        //printf(">>>>>>>> Problem! dispatchPkt %2.2x\r\n", rcode);
                        break; //should be 0, indicating ACK. Else return error code.
                }
                /* check for RCVDAVIRQ and generate error if not present
                 * the only case when absence of RCVDAVIRQ makes sense is when toggle error occurred.
                 * Need to add handling for that
                 *
                 * NOTE: I've seen this happen with SPI corruption -- xxxajk
                 */
                if((regRd(rHIRQ) & bmRCVDAVIRQ) == 0) {
                        //printf(">>>>>>>> Problem! NO RCVDAVIRQ!\r\n");
                        rcode = 0xf0; //receive error
                        break;
                }
                pktsize = regRd(rRCVBC); //number of received bytes
                //printf("Got %i bytes \r\n", pktsize);
                // This would be OK, but...
                //assert(pktsize <= nbytes);
                if(pktsize > nbytes) {
                        // This can happen. Use of assert on Arduino locks up the Arduino.
                        // So I will trim the value, and hope for the best.
                        //printf(">>>>>>>> Problem! Wanted %i bytes but got %i.\r\n", nbytes, pktsize);
                        pktsize = nbytes;
                }

                int16_t mem_left = (int16_t)nbytes - *((int16_t*)nbytesptr);

                if(mem_left < 0)
                        mem_left = 0;

                data = bytesRd(rRCVFIFO, ((pktsize > mem_left) ? mem_left : pktsize), data);

                regWr(rHIRQ, bmRCVDAVIRQ); // Clear the IRQ & free the buffer
                *nbytesptr += pktsize; // add this packet's byte count to total transfer length

                /* The transfer is complete under two conditions:           */
                /* 1. The device sent a short packet (L.T. maxPacketSize)   */
                /* 2. 'nbytes' have been transferred.                       */
                if((pktsize < maxpktsize) || (*nbytesptr >= nbytes)) // have we transferred 'nbytes' bytes?
                {
                        // Save toggle value
                        pep->bmRcvToggle = ((regRd(rHRSL) & bmRCVTOGRD)) ? 1 : 0;
                        //printf("\r\n");
                        rcode = 0;
                        break;
                } else if(bInterval > 0)
                        delay(bInterval); // Delay according to polling interval
        } //while( 1 )
        return ( rcode);
}

/* OUT transfer to arbitrary endpoint. Handles multiple packets if necessary. Transfers 'nbytes' bytes. */
/* Handles NAK bug per Maxim Application Note 4000 for single buffer transfer   */

/* rcode 0 if no errors. rcode 01-0f is relayed from HRSL                       */
uint8_t USB::outTransfer(uint8_t addr, uint8_t ep, uint16_t nbytes, uint8_t* data) {
        EpInfo *pep = NULL;
        uint16_t nak_limit = 0;

        uint8_t rcode = SetAddress(addr, ep, &pep, &nak_limit);

        if(rcode)
                return rcode;

        return OutTransfer(pep, nak_limit, nbytes, data);
}

uint8_t USB::OutTransfer(EpInfo *pep, uint16_t nak_limit, uint16_t nbytes, uint8_t *data) {
        uint8_t rcode = hrSUCCESS, retry_count;
        uint8_t *data_p = data; //local copy of the data pointer
        uint16_t bytes_tosend, nak_count;
        uint16_t bytes_left = nbytes;

        uint8_t maxpktsize = pep->maxPktSize;

        if(maxpktsize < 1 || maxpktsize > 64)
                return USB_ERROR_INVALID_MAX_PKT_SIZE;

        uint32_t timeout = (uint32_t)millis() + USB_XFER_TIMEOUT;

        regWr(rHCTL, (pep->bmSndToggle) ? bmSNDTOG1 : bmSNDTOG0); //set toggle value

        while(bytes_left) {
#if defined(ESP8266) || defined(ESP32)
                        yield(); // needed in order to reset the watchdog timer on the ESP8266
#endif
                retry_count = 0;
                nak_count = 0;
                bytes_tosend = (bytes_left >= maxpktsize) ? maxpktsize : bytes_left;
                bytesWr(rSNDFIFO, bytes_tosend, data_p); //filling output FIFO
                regWr(rSNDBC, bytes_tosend); //set number of bytes
                regWr(rHXFR, (tokOUT | pep->epAddr)); //dispatch packet
                while(!(regRd(rHIRQ) & bmHXFRDNIRQ)){
#if defined(ESP8266) || defined(ESP32)
                        yield(); // needed in order to reset the watchdog timer on the ESP8266
#endif
                } //wait for the completion IRQ
                regWr(rHIRQ, bmHXFRDNIRQ); //clear IRQ
                rcode = (regRd(rHRSL) & 0x0f);

                while(rcode && ((int32_t)((uint32_t)millis() - timeout) < 0L)) {
#if defined(ESP8266) || defined(ESP32)
                        yield(); // needed in order to reset the watchdog timer on the ESP8266
#endif
                        switch(rcode) {
                                case hrNAK:
                                        nak_count++;
                                        if(nak_limit && (nak_count == nak_limit))
                                                goto breakout;
                                        //return ( rcode);
                                        break;
                                case hrTIMEOUT:
                                        retry_count++;
                                        if(retry_count == USB_RETRY_LIMIT)
                                                goto breakout;
                                        //return ( rcode);
                                        break;
                                case hrTOGERR:
                                        // yes, we flip it wrong here so that next time it is actually correct!
                                        pep->bmSndToggle = (regRd(rHRSL) & bmSNDTOGRD) ? 0 : 1;
                                        regWr(rHCTL, (pep->bmSndToggle) ? bmSNDTOG1 : bmSNDTOG0); //set toggle value
                                        break;
                                default:
                                        goto breakout;
                        }//switch( rcode

                        /* process NAK according to Host out NAK bug */
                        regWr(rSNDBC, 0);
                        regWr(rSNDFIFO, *data_p);
                        regWr(rSNDBC, bytes_tosend);
                        regWr(rHXFR, (tokOUT | pep->epAddr)); //dispatch packet
                        while(!(regRd(rHIRQ) & bmHXFRDNIRQ)){
#if defined(ESP8266) || defined(ESP32)
                        yield(); // needed in order to reset the watchdog timer on the ESP8266
#endif
                        } //wait for the completion IRQ
                        regWr(rHIRQ, bmHXFRDNIRQ); //clear IRQ
                        rcode = (regRd(rHRSL) & 0x0f);
                }//while( rcode && ....
                bytes_left -= bytes_tosend;
                data_p += bytes_tosend;
        }//while( bytes_left...
breakout:
        /* If rcode(=rHRSL) is non-zero, untransmitted data remains in the SNDFIFO. */
        if(rcode != 0) {
            //Switch the FIFO containing the OUT data back under microcontroller control and reset pointer.
            regWr(rSNDBC, 0);
        }

        pep->bmSndToggle = (regRd(rHRSL) & bmSNDTOGRD) ? 1 : 0; //bmSNDTOG1 : bmSNDTOG0;  //update toggle
        return ( rcode); //should be 0 in all cases
}
/* dispatch USB packet. Assumes peripheral address is set and relevant buffer is loaded/empty       */
/* If NAK, tries to re-send up to nak_limit times                                                   */
/* If nak_limit == 0, do not count NAKs, exit after timeout                                         */
/* If bus timeout, re-sends up to USB_RETRY_LIMIT times                                             */

/* return codes 0x00-0x0f are HRSLT( 0x00 being success ), 0xff means timeout                       */
uint8_t USB::dispatchPkt(uint8_t token, uint8_t ep, uint16_t nak_limit) {
        uint32_t timeout = (uint32_t)millis() + USB_XFER_TIMEOUT;
        uint8_t tmpdata;
        uint8_t rcode = hrSUCCESS;
        uint8_t retry_count = 0;
        uint16_t nak_count = 0;

        while((int32_t)((uint32_t)millis() - timeout) < 0L) {
#if defined(ESP8266) || defined(ESP32)
                        yield(); // needed in order to reset the watchdog timer on the ESP8266
#endif
                regWr(rHXFR, (token | ep)); //launch the transfer
                rcode = USB_ERROR_TRANSFER_TIMEOUT;

                while((int32_t)((uint32_t)millis() - timeout) < 0L) //wait for transfer completion
                {
#if defined(ESP8266) || defined(ESP32)
                        yield(); // needed in order to reset the watchdog timer on the ESP8266
#endif
                        tmpdata = regRd(rHIRQ);

                        if(tmpdata & bmHXFRDNIRQ) {
                                regWr(rHIRQ, bmHXFRDNIRQ); //clear the interrupt
                                rcode = 0x00;
                                break;
                        }//if( tmpdata & bmHXFRDNIRQ

                }//while ( millis() < timeout

                //if (rcode != 0x00) //exit if timeout
                //        return ( rcode);

                rcode = (regRd(rHRSL) & 0x0f); //analyze transfer result

                switch(rcode) {
                        case hrNAK:
                                nak_count++;
                                if(nak_limit && (nak_count == nak_limit))
                                        return (rcode);
                                break;
                        case hrTIMEOUT:
                                retry_count++;
                                if(retry_count == USB_RETRY_LIMIT)
                                        return (rcode);
                                break;
                        default:
                                return (rcode);
                }//switch( rcode

        }//while( timeout > millis()
        return ( rcode);
}

/* USB main task. Performs enumeration/cleanup */
void USB::Task(void) //USB state machine
{
        uint8_t rcode;
        uint8_t tmpdata;
        static uint32_t delay = 0;

        /*
         * Root-port SE0 diagnostics only.
         * Do not debounce, resample, delay, or otherwise alter the
         * existing disconnect decision.
         */
        static uint32_t se0_diag_event = 0;
        static uint32_t se0_diag_last_non_se0_ms = 0;
        static uint8_t se0_diag_prev_vbus = 0xff;

        //USB_DEVICE_DESCRIPTOR buf;
        bool lowspeed = false;

        MAX3421E::Task();
	/*
	 * Some USB devices are already connected when the MAX3421E starts.
	 * In that case the initial connection-detect interrupt may be missed,
	 * leaving the state machine in WAIT_FOR_DEVICE forever.
	 *
	 * While waiting for a device, periodically resample D+/D- and update
	 * the bus state so that enumeration can start without requiring the
	 * user to unplug/replug the USB device.
	 */
	
	static uint32_t last_startup_probe = 0;

	if (usb_task_state == USB_DETACHED_SUBSTATE_WAIT_FOR_DEVICE) {
	    uint32_t now = millis();

	    if ((uint32_t)(now - last_startup_probe) >= 250) {
		last_startup_probe = now;

		regWr(rHCTL, bmSAMPLEBUS);

		uint32_t probe_started = millis();

		while (!(regRd(rHCTL) & bmSAMPLEBUS)) {
		    if ((uint32_t)(millis() - probe_started) >= 10) {
			USB_HOST_SERIAL.println(
			    "USB SAMPLEBUS timeout"
			);
			break;
		    }
		}

		busprobe();
	    }
	}

	tmpdata = getVbusState();
        const uint32_t se0_diag_now = millis();

        /*
         * A single transient SE0 has been observed while a QMX remains
         * physically connected.  Before tearing down a running USB session,
         * resample D+/D- up to three times.  Read J/K directly from HRSL first
         * so that a transient SE0 does not make busprobe() switch MODE into the
         * disconnected state.
         */
        if(tmpdata == SE0 &&
           (usb_task_state & USB_STATE_MASK) != USB_STATE_DETACHED) {
                bool se0_resample_recovered = false;

                for(uint8_t attempt = 1; attempt <= 3; ++attempt) {
                        ::delay(2);
                        regWr(rHCTL, bmSAMPLEBUS);

                        const uint32_t sample_started = millis();
                        while(!(regRd(rHCTL) & bmSAMPLEBUS)) {
                                if((uint32_t)(millis() - sample_started) >= 10U)
                                        break;
                        }

                        const uint8_t sample_hrsl = regRd(rHRSL);
                        const uint8_t sample_jk =
                            sample_hrsl & (bmJSTATUS | bmKSTATUS);

                        USB_HOST_SERIAL.print("[SE0RESAMPLE] attempt=");
                        USB_HOST_SERIAL.print(attempt);
                        USB_HOST_SERIAL.print(" t=");
                        USB_HOST_SERIAL.print(millis());
                        USB_HOST_SERIAL.print(" state=0x");
                        USB_HOST_SERIAL.print(usb_task_state, HEX);
                        USB_HOST_SERIAL.print(" HRSL=0x");
                        USB_HOST_SERIAL.print(sample_hrsl, HEX);
                        USB_HOST_SERIAL.print(" JK=0x");
                        USB_HOST_SERIAL.println(sample_jk, HEX);

                        if(sample_jk == bmJSTATUS || sample_jk == bmKSTATUS) {
                                busprobe();
                                tmpdata = getVbusState();
                                se0_resample_recovered =
                                    (tmpdata == FSHOST || tmpdata == LSHOST);
                                if(se0_resample_recovered)
                                        break;
                        }
                }

                if(se0_resample_recovered) {
                        USB_HOST_SERIAL.print(
                            "[SE0RESAMPLE] recovered: ignore transient SE0, vbus=0x");
                        USB_HOST_SERIAL.println(tmpdata, HEX);
                } else {
                        USB_HOST_SERIAL.println(
                            "[SE0RESAMPLE] confirmed SE0 after 3 samples");
                }
        }

        if(tmpdata != SE0)
                se0_diag_last_non_se0_ms = millis();

	/*
	static uint32_t debug_last = 0;

	if (millis() - debug_last >= 1000) {
	    debug_last = millis();

	    USB_HOST_SERIAL.print(
		"REAL USB::Task file=" __FILE__
		" state=0x"
	    );
	    USB_HOST_SERIAL.print(usb_task_state, HEX);

	    USB_HOST_SERIAL.print(" vbus=0x");
	    USB_HOST_SERIAL.println(tmpdata, HEX);
	    }
	*/	

        /* modify USB task state if Vbus changed */
        switch(tmpdata) {
                case SE1: //illegal state
                        usb_task_state = USB_DETACHED_SUBSTATE_ILLEGAL;
                        lowspeed = false;
                        break;
                case SE0: //disconnected
		  if((usb_task_state & USB_STATE_MASK) != USB_STATE_DETACHED) {
                    ++se0_diag_event;

                    /* Capture MAX3421E state before init()/Release(). */
                    const uint8_t se0_hrsl = regRd(rHRSL);
                    const uint8_t se0_hirq = regRd(rHIRQ);
                    const uint8_t se0_usbirq = regRd(rUSBIRQ);
                    const uint8_t se0_hctl = regRd(rHCTL);
                    const uint8_t se0_mode = regRd(rMODE);
                    const uint8_t se0_peraddr = regRd(rPERADDR);

                    USB_HOST_SERIAL.print("[SE0DIAG] event=");
                    USB_HOST_SERIAL.print(se0_diag_event);
                    USB_HOST_SERIAL.print(" t=");
                    USB_HOST_SERIAL.print(se0_diag_now);
                    USB_HOST_SERIAL.print(" state=0x");
                    USB_HOST_SERIAL.print(usb_task_state, HEX);
                    USB_HOST_SERIAL.print(" vbus=0x");
                    USB_HOST_SERIAL.print(tmpdata, HEX);
                    USB_HOST_SERIAL.print(" prev_vbus=0x");
                    USB_HOST_SERIAL.print(se0_diag_prev_vbus, HEX);
                    USB_HOST_SERIAL.print(" since_non_se0_ms=");
                    USB_HOST_SERIAL.print((uint32_t)(se0_diag_now - se0_diag_last_non_se0_ms));
                    USB_HOST_SERIAL.print(" HRSL=0x");
                    USB_HOST_SERIAL.print(se0_hrsl, HEX);
                    USB_HOST_SERIAL.print(" HRSL_result=0x");
                    USB_HOST_SERIAL.print(se0_hrsl & 0x0f, HEX);
                    USB_HOST_SERIAL.print(" HIRQ=0x");
                    USB_HOST_SERIAL.print(se0_hirq, HEX);
                    USB_HOST_SERIAL.print(" USBIRQ=0x");
                    USB_HOST_SERIAL.print(se0_usbirq, HEX);
                    USB_HOST_SERIAL.print(" HCTL=0x");
                    USB_HOST_SERIAL.print(se0_hctl, HEX);
                    USB_HOST_SERIAL.print(" MODE=0x");
                    USB_HOST_SERIAL.print(se0_mode, HEX);
                    USB_HOST_SERIAL.print(" PERADDR=");
                    USB_HOST_SERIAL.println(se0_peraddr);

		    USB_HOST_SERIAL.print(		    
					  "USB::Task SE0: returning to INITIALIZE from 0x"
							    );
		    USB_HOST_SERIAL.println(usb_task_state, HEX);
		    
		    usb_task_state = USB_DETACHED_SUBSTATE_INITIALIZE;
		  }
		  lowspeed = false;
		  break;
                case LSHOST:

                        lowspeed = true;

			//			USB_HOST_SERIAL.print("USB::Task LSHOST state=0x");
			//			USB_HOST_SERIAL.println(usb_task_state, HEX);
			
			// intentional fallthrough			
                case FSHOST: //attached
                        if((usb_task_state & USB_STATE_MASK) == USB_STATE_DETACHED) {
			  USB_HOST_SERIAL.print(
						"USB::Task attached: state 0x"
						);
			  USB_HOST_SERIAL.print(usb_task_state, HEX);
			  
			  delay = (uint32_t)millis() + USB_SETTLE_DELAY;
			  usb_task_state = USB_ATTACHED_SUBSTATE_SETTLE;
			  
			  USB_HOST_SERIAL.print(" -> 0x");
			  USB_HOST_SERIAL.println(usb_task_state, HEX);
                        }
                        break;
        }// switch( tmpdata

        se0_diag_prev_vbus = tmpdata;

        for(uint8_t i = 0; i < USB_NUMDEVICES; i++)
                if(devConfig[i])
                        rcode = devConfig[i]->Poll();

        switch(usb_task_state) {
                case USB_DETACHED_SUBSTATE_INITIALIZE:
                        init();

                        for(uint8_t i = 0; i < USB_NUMDEVICES; i++)
                                if(devConfig[i])
                                        rcode = devConfig[i]->Release();

                        usb_task_state = USB_DETACHED_SUBSTATE_WAIT_FOR_DEVICE;
                        break;
                case USB_DETACHED_SUBSTATE_WAIT_FOR_DEVICE: //just sit here
                        break;
                case USB_DETACHED_SUBSTATE_ILLEGAL: //just sit here
                        break;
                case USB_ATTACHED_SUBSTATE_SETTLE: //settle time for just attached device
                        if((int32_t)((uint32_t)millis() - delay) >= 0L)
                                usb_task_state = USB_ATTACHED_SUBSTATE_RESET_DEVICE;
                        else break; // don't fall through
                case USB_ATTACHED_SUBSTATE_RESET_DEVICE:
                        regWr(rHCTL, bmBUSRST); //issue bus reset
                        usb_task_state = USB_ATTACHED_SUBSTATE_WAIT_RESET_COMPLETE;
                        break;
                case USB_ATTACHED_SUBSTATE_WAIT_RESET_COMPLETE:
                        if((regRd(rHCTL) & bmBUSRST) == 0) {
                                tmpdata = regRd(rMODE) | bmSOFKAENAB; //start SOF generation
                                regWr(rMODE, tmpdata);
                                usb_task_state = USB_ATTACHED_SUBSTATE_WAIT_SOF;
                                //delay = (uint32_t)millis() + 20; //20ms wait after reset per USB spec
                        }
                        break;
                case USB_ATTACHED_SUBSTATE_WAIT_SOF: //todo: change check order
                        if(regRd(rHIRQ) & bmFRAMEIRQ) {
                                //when first SOF received _and_ 20ms has passed we can continue
                                /*
                                if (delay < (uint32_t)millis()) //20ms passed
                                        usb_task_state = USB_STATE_CONFIGURING;
                                 */
                                usb_task_state = USB_ATTACHED_SUBSTATE_WAIT_RESET;
                                delay = (uint32_t)millis() + 20;
                        }
                        break;
                case USB_ATTACHED_SUBSTATE_WAIT_RESET:
                        if((int32_t)((uint32_t)millis() - delay) >= 0L) usb_task_state = USB_STATE_CONFIGURING;
                        else break; // don't fall through
                case USB_STATE_CONFIGURING:

                        //Serial.print("\r\nConf.LS: ");
                        //Serial.println(lowspeed, HEX);

                        rcode = Configuring(0, 0, lowspeed);

                        if(rcode) {
                                if(rcode != USB_DEV_CONFIG_ERROR_DEVICE_INIT_INCOMPLETE) {
                                        usb_error = rcode;
                                        usb_task_state = USB_STATE_ERROR;
                                }
                        } else
                                usb_task_state = USB_STATE_RUNNING;
                        break;
                case USB_STATE_RUNNING:
                        break;
                case USB_STATE_ERROR:
                        //MAX3421E::Init();
                        break;
        } // switch( usb_task_state )
}

uint8_t USB::DefaultAddressing(uint8_t parent, uint8_t port, bool lowspeed) {
        //uint8_t                buf[12];
        uint8_t rcode;
        UsbDevice *p0 = NULL, *p = NULL;

        // Get pointer to pseudo device with address 0 assigned
        p0 = addrPool.GetUsbDevicePtr(0);

        if(!p0)
                return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;

        if(!p0->epinfo)
                return USB_ERROR_EPINFO_IS_NULL;

        p0->lowspeed = (lowspeed) ? true : false;

        // Allocate new address according to device class
        uint8_t bAddress = addrPool.AllocAddress(parent, false, port);

        if(!bAddress)
                return USB_ERROR_OUT_OF_ADDRESS_SPACE_IN_POOL;

        p = addrPool.GetUsbDevicePtr(bAddress);

        if(!p)
                return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;

        p->lowspeed = lowspeed;

        // Assign new address to the device
        rcode = setAddr(0, 0, bAddress);

        if(rcode) {
                addrPool.FreeAddress(bAddress);
                bAddress = 0;
                return rcode;
        }
        return 0;
};

uint8_t USB::AttemptConfig(uint8_t driver, uint8_t parent, uint8_t port, bool lowspeed) {
        //printf("AttemptConfig: parent = %i, port = %i\r\n", parent, port);
        uint8_t retries = 0;

again:
        uint8_t rcode = devConfig[driver]->ConfigureDevice(parent, port, lowspeed);
        if(rcode == USB_ERROR_CONFIG_REQUIRES_ADDITIONAL_RESET) {
                if(parent == 0) {
                        // Send a bus reset on the root interface.
                        regWr(rHCTL, bmBUSRST); //issue bus reset
                        delay(102); // delay 102ms, compensate for clock inaccuracy.
                } else {
                        // reset parent port
                        devConfig[parent]->ResetHubPort(port);
                }
        } else if(rcode == hrJERR && retries < 3) { // Some devices returns this when plugged in - trying to initialize the device again usually works
                delay(100);
                retries++;
                goto again;
        } else if(rcode)
                return rcode;

        rcode = devConfig[driver]->Init(parent, port, lowspeed);
        if(rcode == hrJERR && retries < 3) { // Some devices returns this when plugged in - trying to initialize the device again usually works
                delay(100);
                retries++;
                goto again;
        }
        if(rcode) {
                // Issue a bus reset, because the device may be in a limbo state
                if(parent == 0) {
                        // Send a bus reset on the root interface.
                        regWr(rHCTL, bmBUSRST); //issue bus reset
                        delay(102); // delay 102ms, compensate for clock inaccuracy.
                } else {
                        // reset parent port
                        devConfig[parent]->ResetHubPort(port);
                }
        }
        return rcode;
}

/*
 * This is broken. We need to enumerate differently.
 * It causes major problems with several devices if detected in an unexpected order.
 *
 *
 * Oleg - I wouldn't do anything before the newly connected device is considered sane.
 * i.e.(delays are not indicated for brevity):
 * 1. reset
 * 2. GetDevDescr();
 * 3a. If ACK, continue with allocating address, addressing, etc.
 * 3b. Else reset again, count resets, stop at some number (5?).
 * 4. When max.number of resets is reached, toggle power/fail
 * If desired, this could be modified by performing two resets with GetDevDescr() in the middle - however, from my experience, if a device answers to GDD()
 * it doesn't need to be reset again
 * New steps proposal:
 * 1: get address pool instance. exit on fail
 * 2: pUsb->getDevDescr(0, 0, constBufSize, (uint8_t*)buf). exit on fail.
 * 3: bus reset, 100ms delay
 * 4: set address
 * 5: pUsb->setEpInfoEntry(bAddress, 1, epInfo), exit on fail
 * 6: while (configurations) {
 *              for(each configuration) {
 *                      for (each driver) {
 *                              6a: Ask device if it likes configuration. Returns 0 on OK.
 *                                      If successful, the driver configured device.
 *                                      The driver now owns the endpoints, and takes over managing them.
 *                                      The following will need codes:
 *                                          Everything went well, instance consumed, exit with success.
 *                                          Instance already in use, ignore it, try next driver.
 *                                          Not a supported device, ignore it, try next driver.
 *                                          Not a supported configuration for this device, ignore it, try next driver.
 *                                          Could not configure device, fatal, exit with fail.
 *                      }
 *              }
 *    }
 * 7: for(each driver) {
 *      7a: Ask device if it knows this VID/PID. Acts exactly like 6a, but using VID/PID
 * 8: if we get here, no driver likes the device plugged in, so exit failure.
 *
 */
uint8_t USB::Configuring(uint8_t parent, uint8_t port, bool lowspeed) {
        //uint8_t bAddress = 0;
        //printf("Configuring: parent = %i, port = %i\r\n", parent, port);
        uint8_t devConfigIndex;
        uint8_t rcode = 0;
        uint8_t buf[sizeof (USB_DEVICE_DESCRIPTOR)];
        USB_DEVICE_DESCRIPTOR *udd = reinterpret_cast<USB_DEVICE_DESCRIPTOR *>(buf);
        UsbDevice *p = NULL;
        EpInfo *oldep_ptr = NULL;
        EpInfo epInfo;

        epInfo.epAddr = 0;
        epInfo.maxPktSize = 8;
        epInfo.bmSndToggle = 0;
        epInfo.bmRcvToggle = 0;
        epInfo.bmNakPower = USB_NAK_MAX_POWER;

        //delay(2000);
        AddressPool &addrPool = GetAddressPool();
        // Get pointer to pseudo device with address 0 assigned
        p = addrPool.GetUsbDevicePtr(0);
        if(!p) {
                //printf("Configuring error: USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL\r\n");
                return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;
        }

        // Save old pointer to EP_RECORD of address 0
        oldep_ptr = p->epinfo;

        // Temporary assign new pointer to epInfo to p->epinfo in order to
        // avoid toggle inconsistence

        p->epinfo = &epInfo;

        p->lowspeed = lowspeed;

        /*
         * At the default USB address the host only knows EP0's provisional
         * 8-byte packet size.  Some composite devices (notably QMX) NAK an
         * immediate 18-byte GET_DESCRIPTOR after reconnect.  Read the first
         * 8 bytes first, learn bMaxPacketSize0, then request the complete
         * device descriptor.  Retry transient NAKs during both stages.
         */
        for(uint8_t retry = 0; retry < 10; retry++) {
                rcode = getDevDescr(0, 0, 8, (uint8_t*)buf);
                if(rcode != hrNAK)
                        break;
                USB_HOST_SERIAL.print("USB Configuring getDevDescr(8) NAK retry ");
                USB_HOST_SERIAL.println(retry + 1);
                delay(50);
        }

        if(!rcode) {
                epInfo.maxPktSize = udd->bMaxPacketSize0;
                p->epinfo = &epInfo;

                for(uint8_t retry = 0; retry < 10; retry++) {
                        rcode = getDevDescr(0, 0, sizeof (USB_DEVICE_DESCRIPTOR),
                                           (uint8_t*)buf);
                        if(rcode != hrNAK)
                                break;
                        USB_HOST_SERIAL.print("USB Configuring getDevDescr(18) NAK retry ");
                        USB_HOST_SERIAL.println(retry + 1);
                        delay(50);
                }
        }

        // Restore p->epinfo
        p->epinfo = oldep_ptr;

        if(rcode) {
                USB_HOST_SERIAL.print("USB Configuring getDevDescr failed rcode=0x");
                USB_HOST_SERIAL.println(rcode, HEX);
                return rcode;
        }

        // to-do?
        // Allocate new address according to device class
        //bAddress = addrPool.AllocAddress(parent, false, port);

        uint16_t vid = udd->idVendor;
        uint16_t pid = udd->idProduct;
        uint8_t klass = udd->bDeviceClass;
        uint8_t subklass = udd->bDeviceSubClass;

        USB_HOST_SERIAL.print("USB Configuring dev VID=");
        USB_HOST_SERIAL.print(vid, HEX);
        USB_HOST_SERIAL.print(" PID=");
        USB_HOST_SERIAL.print(pid, HEX);
        USB_HOST_SERIAL.print(" class=0x");
        USB_HOST_SERIAL.print(klass, HEX);
        USB_HOST_SERIAL.print(" subclass=0x");
        USB_HOST_SERIAL.print(subklass, HEX);
        USB_HOST_SERIAL.print(" parent=");
        USB_HOST_SERIAL.print(parent);
        USB_HOST_SERIAL.print(" port=");
        USB_HOST_SERIAL.println(port);

        // Attempt to configure if VID/PID or device class matches with a driver
        // Qualify with subclass too.
        //
        // VID/PID & class tests default to false for drivers not yet ported
        // subclass defaults to true, so you don't have to define it if you don't have to.
        //
        for(devConfigIndex = 0; devConfigIndex < USB_NUMDEVICES; devConfigIndex++) {
                if(!devConfig[devConfigIndex]) continue; // no driver
                if(devConfig[devConfigIndex]->GetAddress()) continue; // consumed
                if(devConfig[devConfigIndex]->DEVSUBCLASSOK(subklass) && (devConfig[devConfigIndex]->VIDPIDOK(vid, pid) || devConfig[devConfigIndex]->DEVCLASSOK(klass))) {
                        USB_HOST_SERIAL.print("USB Configuring class-match driver=");
                        USB_HOST_SERIAL.print(devConfigIndex);
                        USB_HOST_SERIAL.print(" current_addr=");
                        USB_HOST_SERIAL.println(devConfig[devConfigIndex]->GetAddress());
                        rcode = AttemptConfig(devConfigIndex, parent, port, lowspeed);
                        USB_HOST_SERIAL.print("USB Configuring class-match result driver=");
                        USB_HOST_SERIAL.print(devConfigIndex);
                        USB_HOST_SERIAL.print(" rcode=0x");
                        USB_HOST_SERIAL.println(rcode, HEX);
                        if(rcode != USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED)
                                break;
                }
        }

        if(devConfigIndex < USB_NUMDEVICES) {
                return rcode;
        }


        // blindly attempt to configure
        for(devConfigIndex = 0; devConfigIndex < USB_NUMDEVICES; devConfigIndex++) {
                if(!devConfig[devConfigIndex]) continue;
                if(devConfig[devConfigIndex]->GetAddress()) continue; // consumed
                if(devConfig[devConfigIndex]->DEVSUBCLASSOK(subklass) && (devConfig[devConfigIndex]->VIDPIDOK(vid, pid) || devConfig[devConfigIndex]->DEVCLASSOK(klass))) continue; // If this is true it means it must have returned USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED above
                rcode = AttemptConfig(devConfigIndex, parent, port, lowspeed);

                //printf("ERROR ENUMERATING %2.2x\r\n", rcode);
                if(!(rcode == USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED || rcode == USB_ERROR_CLASS_INSTANCE_ALREADY_IN_USE)) {
                        // in case of an error dev_index should be reset to 0
                        //                in order to start from the very beginning the
                        //                next time the program gets here
                        //if (rcode != USB_DEV_CONFIG_ERROR_DEVICE_INIT_INCOMPLETE)
                        //        devConfigIndex = 0;
                        return rcode;
                }
        }
        // if we get here that means that the device class is not supported by any of registered classes
        rcode = DefaultAddressing(parent, port, lowspeed);

        return rcode;
}

uint8_t USB::ReleaseDeviceAtPort(uint8_t parent, uint8_t port) {
        /*
         * First look for a hub driver that explicitly owns this physical
         * parent/port.  Hub USB addresses do not encode the upstream port.
         */
        for(uint8_t i = 0; i < USB_NUMDEVICES; i++) {
                if(!devConfig[i])
                        continue;
                if(!devConfig[i]->HUBPORTOK(parent, port))
                        continue;

                const uint8_t addr = devConfig[i]->GetAddress();
                USB_HOST_SERIAL.print("[USBREL] hub parent=");
                USB_HOST_SERIAL.print(parent);
                USB_HOST_SERIAL.print(" port=");
                USB_HOST_SERIAL.print(port);
                USB_HOST_SERIAL.print(" addr=");
                USB_HOST_SERIAL.println(addr);
                return ReleaseDevice(addr);
        }

        /*
         * Ordinary non-hub device addresses do encode parent+port.
         */
        UsbDeviceAddress a;
        a.devAddress = 0;
        a.bmHub = 0;
        a.bmParent = parent;
        a.bmAddress = port;

        AddressPool &addrPool = GetAddressPool();
        if(!addrPool.GetUsbDevicePtr(a.devAddress)) {
                USB_HOST_SERIAL.print("[USBREL] no-device parent=");
                USB_HOST_SERIAL.print(parent);
                USB_HOST_SERIAL.print(" port=");
                USB_HOST_SERIAL.print(port);
                USB_HOST_SERIAL.print(" addr=");
                USB_HOST_SERIAL.println(a.devAddress);
                return 0;
        }

        USB_HOST_SERIAL.print("[USBREL] device parent=");
        USB_HOST_SERIAL.print(parent);
        USB_HOST_SERIAL.print(" port=");
        USB_HOST_SERIAL.print(port);
        USB_HOST_SERIAL.print(" addr=");
        USB_HOST_SERIAL.println(a.devAddress);
        return ReleaseDevice(a.devAddress);
}

uint8_t USB::ReleaseDevice(uint8_t addr) {
        if(!addr)
                return 0;

        /*
         * If a hub disappears, release class-driver instances attached below
         * it before releasing the hub itself.  AddressPool::FreeAddress()
         * already frees descendant address records, but the corresponding
         * USBDeviceConfig objects used to retain their old bAddress/ready
         * state.  On reconnect those stale instances were therefore skipped,
         * and a nested hub (for example the IC-705 internal 0451:2046 hub
         * behind an external hub) could fall through to ACM/BTD probing and
         * fail to enumerate.
         *
         * USB addresses encode the immediate parent hub in bmParent.  Release
         * direct children one at a time; recursion handles any nested hubs.
         * Restart the scan after each release because Release() changes the
         * child's GetAddress() value to zero.
         */
        UsbDeviceAddress parent_addr;
        parent_addr.devAddress = addr;
        if(parent_addr.bmHub) {
                bool released_child;
                do {
                        released_child = false;
                        for(uint8_t i = 0; i < USB_NUMDEVICES; i++) {
                                if(!devConfig[i]) continue;

                                const uint8_t child_addr_raw = devConfig[i]->GetAddress();
                                if(!child_addr_raw || child_addr_raw == addr) continue;

                                UsbDeviceAddress child_addr;
                                child_addr.devAddress = child_addr_raw;
                                if(child_addr.bmParent == parent_addr.bmAddress) {
                                        ReleaseDevice(child_addr_raw);
                                        released_child = true;
                                        break;
                                }
                        }
                } while(released_child);
        }

        for(uint8_t i = 0; i < USB_NUMDEVICES; i++) {
                if(!devConfig[i]) continue;
                if(devConfig[i]->GetAddress() == addr)
                        return devConfig[i]->Release();
        }
        return 0;
}

#if 1 //!defined(USB_METHODS_INLINE)
//get device descriptor

uint8_t USB::getDevDescr(uint8_t addr, uint8_t ep, uint16_t nbytes, uint8_t* dataptr) {
        return ( ctrlReq(addr, ep, bmREQ_GET_DESCR, USB_REQUEST_GET_DESCRIPTOR, 0x00, USB_DESCRIPTOR_DEVICE, 0x0000, nbytes, nbytes, dataptr, NULL));
}
//get configuration descriptor

uint8_t USB::getConfDescr(uint8_t addr, uint8_t ep, uint16_t nbytes, uint8_t conf, uint8_t* dataptr) {
        return ( ctrlReq(addr, ep, bmREQ_GET_DESCR, USB_REQUEST_GET_DESCRIPTOR, conf, USB_DESCRIPTOR_CONFIGURATION, 0x0000, nbytes, nbytes, dataptr, NULL));
}

/* Requests Configuration Descriptor. Sends two Get Conf Descr requests. The first one gets the total length of all descriptors, then the second one requests this
 total length. The length of the first request can be shorter ( 4 bytes ), however, there are devices which won't work unless this length is set to 9 */
uint8_t USB::getConfDescr(uint8_t addr, uint8_t ep, uint8_t conf, USBReadParser *p) {
        uint8_t header[sizeof(USB_CONFIGURATION_DESCRIPTOR)];
        USB_CONFIGURATION_DESCRIPTOR *ucd = reinterpret_cast<USB_CONFIGURATION_DESCRIPTOR *>(header);

        uint8_t ret = getConfDescr(addr, ep, sizeof(header), conf, header);
        if(ret)
                return ret;

        const uint16_t total = ucd->wTotalLength;
        if(total < sizeof(USB_CONFIGURATION_DESCRIPTOR))
                return USB_ERROR_INVALID_ARGUMENT;

        /*
         * Keep the complete descriptor transfer inside one InTransfer() call.
         *
         * The former streaming path asked ctrlReq() to fetch only 64 bytes at
         * a time.  ctrlReq() then re-entered InTransfer() for every packet.
         * Some composite devices, notably QMX (0483:a34c), NAK the second IN
         * token when the data stage is split this way even though the USB data
         * toggle is restored.  A single continuous control-IN data stage is
         * also closer to the USB Host Shield library's normal transfer model.
         *
         * Configuration descriptors are small during enumeration (QMX is 268
         * bytes), so temporarily buffering the complete descriptor is a safe
         * and simple solution.  Parse it only after the status stage succeeds.
         */
        uint8_t *buf = (uint8_t*)malloc(total);
        if(!buf)
                return USB_ERROR_OUT_OF_ADDRESS_SPACE_IN_POOL;

        ret = ctrlReq(addr, ep, bmREQ_GET_DESCR, USB_REQUEST_GET_DESCRIPTOR,
                      conf, USB_DESCRIPTOR_CONFIGURATION, 0x0000,
                      total, total, buf, NULL);

        if(!ret && p)
                p->Parse(total, buf, 0);

        free(buf);
        return ret;
}

//get string descriptor

uint8_t USB::getStrDescr(uint8_t addr, uint8_t ep, uint16_t ns, uint8_t index, uint16_t langid, uint8_t* dataptr) {
        return ( ctrlReq(addr, ep, bmREQ_GET_DESCR, USB_REQUEST_GET_DESCRIPTOR, index, USB_DESCRIPTOR_STRING, langid, ns, ns, dataptr, NULL));
}
//set address

uint8_t USB::setAddr(uint8_t oldaddr, uint8_t ep, uint8_t newaddr) {
        uint8_t rcode = ctrlReq(oldaddr, ep, bmREQ_SET, USB_REQUEST_SET_ADDRESS, newaddr, 0x00, 0x0000, 0x0000, 0x0000, NULL, NULL);
        //delay(2); //per USB 2.0 sect.9.2.6.3
        delay(300); // Older spec says you should wait at least 200ms
        return rcode;
        //return ( ctrlReq(oldaddr, ep, bmREQ_SET, USB_REQUEST_SET_ADDRESS, newaddr, 0x00, 0x0000, 0x0000, 0x0000, NULL, NULL));
}
//set configuration

uint8_t USB::setConf(uint8_t addr, uint8_t ep, uint8_t conf_value) {
        return ( ctrlReq(addr, ep, bmREQ_SET, USB_REQUEST_SET_CONFIGURATION, conf_value, 0x00, 0x0000, 0x0000, 0x0000, NULL, NULL));
}

#endif // defined(USB_METHODS_INLINE)
