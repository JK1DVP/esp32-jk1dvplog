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
#include "usbhub.h"

bool USBHub::bResetInitiated = false;

USBHub::USBHub(USB *p) :
pUsb(p),
bAddress(0),
bNbrPorts(0),
bParent(0),
bPort(0),
//bInitState(0),
qNextPollTime(0),
bPollEnable(false) {
        epInfo[0].epAddr = 0;
        epInfo[0].maxPktSize = 8;
        epInfo[0].bmSndToggle = 0;
        epInfo[0].bmRcvToggle = 0;
        epInfo[0].bmNakPower = USB_NAK_MAX_POWER;

        epInfo[1].epAddr = 1;
        epInfo[1].maxPktSize = 8; //kludge
        epInfo[1].bmSndToggle = 0;
        epInfo[1].bmRcvToggle = 0;
        epInfo[1].bmNakPower = USB_NAK_NOWAIT;

        if(pUsb)
                pUsb->RegisterDeviceClass(this);
}

uint8_t USBHub::Init(uint8_t parent, uint8_t port, bool lowspeed) {
        USB_HOST_SERIAL.print("USBHub Init enter addr=");
        USB_HOST_SERIAL.print(bAddress);
        USB_HOST_SERIAL.print(" parent=");
        USB_HOST_SERIAL.print(parent);
        USB_HOST_SERIAL.print(" port=");
        USB_HOST_SERIAL.println(port);

        uint8_t buf[32];
        USB_DEVICE_DESCRIPTOR * udd = reinterpret_cast<USB_DEVICE_DESCRIPTOR*>(buf);
        HubDescriptor* hd = reinterpret_cast<HubDescriptor*>(buf);
        USB_CONFIGURATION_DESCRIPTOR * ucd = reinterpret_cast<USB_CONFIGURATION_DESCRIPTOR*>(buf);
        uint8_t rcode;
        UsbDevice *p = NULL;
        EpInfo *oldep_ptr = NULL;
        uint8_t len = 0;
        uint16_t cd_len = 0;
        const char *fail_step = "none";

        //USBTRACE("\r\nHub Init Start ");
        //D_PrintHex<uint8_t > (bInitState, 0x80);

        AddressPool &addrPool = pUsb->GetAddressPool();

        //switch (bInitState) {
        //        case 0:
        if(bAddress)
                return USB_ERROR_CLASS_INSTANCE_ALREADY_IN_USE;

        // Get pointer to pseudo device with address 0 assigned
        p = addrPool.GetUsbDevicePtr(0);

        if(!p)
                return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;

        if(!p->epinfo)
                return USB_ERROR_EPINFO_IS_NULL;

        // Save old pointer to EP_RECORD of address 0
        oldep_ptr = p->epinfo;

        // Temporary assign new pointer to epInfo to p->epinfo in order to avoid toggle inconsistence
        p->epinfo = epInfo;

        p->lowspeed = lowspeed;

        // Get device descriptor
        fail_step = "getDevDescr_addr0_8";
        rcode = pUsb->getDevDescr(0, 0, 8, (uint8_t*)buf);

        p->lowspeed = false;

        if(!rcode)
                len = (buf[0] > 32) ? 32 : buf[0];

        if(rcode) {
                // Restore p->epinfo
                p->epinfo = oldep_ptr;
                USB_HOST_SERIAL.print("[HUBINITFAIL] step=");
                USB_HOST_SERIAL.print(fail_step);
                USB_HOST_SERIAL.print(" addr=");
                USB_HOST_SERIAL.print(bAddress);
                USB_HOST_SERIAL.print(" parent=");
                USB_HOST_SERIAL.print(parent);
                USB_HOST_SERIAL.print(" port=");
                USB_HOST_SERIAL.print(port);
                USB_HOST_SERIAL.print(" rcode=0x");
                USB_HOST_SERIAL.print(rcode, HEX);
                USB_HOST_SERIAL.print(" HRSL=0x");
                USB_HOST_SERIAL.print(pUsb->regRd(rHRSL), HEX);
                USB_HOST_SERIAL.print(" HIRQ=0x");
                USB_HOST_SERIAL.print(pUsb->regRd(rHIRQ), HEX);
                USB_HOST_SERIAL.print(" USBIRQ=0x");
                USB_HOST_SERIAL.print(pUsb->regRd(rUSBIRQ), HEX);
                USB_HOST_SERIAL.print(" HCTL=0x");
                USB_HOST_SERIAL.print(pUsb->regRd(rHCTL), HEX);
                USB_HOST_SERIAL.print(" MODE=0x");
                USB_HOST_SERIAL.print(pUsb->regRd(rMODE), HEX);
                USB_HOST_SERIAL.print(" PERADDR=");
                USB_HOST_SERIAL.println(pUsb->regRd(rPERADDR));
                return rcode;
        }

        USB_HOST_SERIAL.print("USBHub addr0 class=0x");
        USB_HOST_SERIAL.print(udd->bDeviceClass, HEX);
        USB_HOST_SERIAL.print(" maxpkt=");
        USB_HOST_SERIAL.println(udd->bMaxPacketSize0);

        // Extract device class from device descriptor
        // If device class is not a hub return
        if(udd->bDeviceClass != 0x09) {
                p->epinfo = oldep_ptr;
                USB_HOST_SERIAL.println("USBHub reject: not hub");
                return USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED;
        }

        // Allocate new address according to device class
        bAddress = addrPool.AllocAddress(parent, (udd->bDeviceClass == 0x09) ? true : false, port);

        if(!bAddress)
                return USB_ERROR_OUT_OF_ADDRESS_SPACE_IN_POOL;

        // Extract Max Packet Size from the device descriptor
        epInfo[0].maxPktSize = udd->bMaxPacketSize0;

        // Assign new address to the device
        fail_step = "setAddr";
        rcode = pUsb->setAddr(0, 0, bAddress);

        if(rcode) {
                // Restore p->epinfo
                p->epinfo = oldep_ptr;
                USB_HOST_SERIAL.print("[HUBINITFAIL] step=");
                USB_HOST_SERIAL.print(fail_step);
                USB_HOST_SERIAL.print(" addr=");
                USB_HOST_SERIAL.print(bAddress);
                USB_HOST_SERIAL.print(" parent=");
                USB_HOST_SERIAL.print(parent);
                USB_HOST_SERIAL.print(" port=");
                USB_HOST_SERIAL.print(port);
                USB_HOST_SERIAL.print(" rcode=0x");
                USB_HOST_SERIAL.print(rcode, HEX);
                USB_HOST_SERIAL.print(" HRSL=0x");
                USB_HOST_SERIAL.print(pUsb->regRd(rHRSL), HEX);
                USB_HOST_SERIAL.print(" HIRQ=0x");
                USB_HOST_SERIAL.print(pUsb->regRd(rHIRQ), HEX);
                USB_HOST_SERIAL.print(" USBIRQ=0x");
                USB_HOST_SERIAL.print(pUsb->regRd(rUSBIRQ), HEX);
                USB_HOST_SERIAL.print(" HCTL=0x");
                USB_HOST_SERIAL.print(pUsb->regRd(rHCTL), HEX);
                USB_HOST_SERIAL.print(" MODE=0x");
                USB_HOST_SERIAL.print(pUsb->regRd(rMODE), HEX);
                USB_HOST_SERIAL.print(" PERADDR=");
                USB_HOST_SERIAL.println(pUsb->regRd(rPERADDR));
                addrPool.FreeAddress(bAddress);
                bAddress = 0;
                return rcode;
        }

        //USBTRACE2("\r\nHub address: ", bAddress );

        // Restore p->epinfo
        p->epinfo = oldep_ptr;

        if(len) {
                fail_step = "getDevDescr_newaddr";
                rcode = pUsb->getDevDescr(bAddress, 0, len, (uint8_t*)buf);
        }

        if(rcode)
                goto FailGetDevDescr;

        // Assign epInfo to epinfo pointer
        fail_step = "setEpInfoEntry";
        rcode = pUsb->setEpInfoEntry(bAddress, 2, epInfo);

        if(rcode)
                goto FailSetDevTblEntry;

        //                bInitState = 1;

        //        case 1:
        // Get hub descriptor
        fail_step = "GetHubDescriptor";
        rcode = GetHubDescriptor(0, 8, buf);

        if(rcode)
                goto FailGetHubDescr;

        // Save number of ports for future use
        bNbrPorts = hd->bNbrPorts;

        //                bInitState = 2;

        //        case 2:
        // Read configuration Descriptor in Order To Obtain Proper Configuration Value
        fail_step = "getConfDescr_8";
        rcode = pUsb->getConfDescr(bAddress, 0, 8, 0, buf);

        if(!rcode) {
                cd_len = ucd->wTotalLength;
                fail_step = "getConfDescr_full";
                rcode = pUsb->getConfDescr(bAddress, 0, cd_len, 0, buf);
        }
        if(rcode)
                goto FailGetConfDescr;

        // The following code is of no practical use in real life applications.
        // It only intended for the usb protocol sniffer to properly parse hub-class requests.
        {
                uint8_t buf2[24];

                fail_step = "getConfDescr_sniffer";
                rcode = pUsb->getConfDescr(bAddress, 0, buf[0], 0, buf2);

                if(rcode)
                        goto FailGetConfDescr;
        }

        // Set Configuration Value
        fail_step = "setConf";
        rcode = pUsb->setConf(bAddress, 0, buf[5]);

        if(rcode)
                goto FailSetConfDescr;

        //                bInitState = 3;

        //        case 3:
        // Power on all ports
        for(uint8_t j = 1; j <= bNbrPorts; j++)
                SetPortFeature(HUB_FEATURE_PORT_POWER, j, 0); //HubPortPowerOn(j);

        pUsb->SetHubPreMask();
        bPollEnable = true;
        bParent = parent;
        bPort = port;

        /*
         * Nested hubs are polled through EP0 GetPortStatus() instead of the
         * interrupt endpoint. A short settle time avoids touching child ports
         * immediately after the hub itself has been configured.
         */
        if(parent != 0)
                qNextPollTime = (uint32_t)millis() + 100;

        USB_HOST_SERIAL.print("USBHub configured addr=");
        USB_HOST_SERIAL.print(bAddress);
        USB_HOST_SERIAL.print(" ports=");
        USB_HOST_SERIAL.println(bNbrPorts);

        //                bInitState = 0;
        //}
        //bInitState = 0;
        //USBTRACE("...OK\r\n");
        return 0;

        // Oleg, No debugging?? -- xxxajk
FailGetDevDescr:
        goto Fail;

FailSetDevTblEntry:
        goto Fail;

FailGetHubDescr:
        goto Fail;

FailGetConfDescr:
        goto Fail;

FailSetConfDescr:
        goto Fail;

Fail:
        USB_HOST_SERIAL.print("[HUBINITFAIL] step=");
        USB_HOST_SERIAL.print(fail_step);
        USB_HOST_SERIAL.print(" addr=");
        USB_HOST_SERIAL.print(bAddress);
        USB_HOST_SERIAL.print(" parent=");
        USB_HOST_SERIAL.print(parent);
        USB_HOST_SERIAL.print(" port=");
        USB_HOST_SERIAL.print(port);
        USB_HOST_SERIAL.print(" rcode=0x");
        USB_HOST_SERIAL.print(rcode, HEX);
        USB_HOST_SERIAL.print(" HRSL=0x");
        USB_HOST_SERIAL.print(pUsb->regRd(rHRSL), HEX);
        USB_HOST_SERIAL.print(" HIRQ=0x");
        USB_HOST_SERIAL.print(pUsb->regRd(rHIRQ), HEX);
        USB_HOST_SERIAL.print(" USBIRQ=0x");
        USB_HOST_SERIAL.print(pUsb->regRd(rUSBIRQ), HEX);
        USB_HOST_SERIAL.print(" HCTL=0x");
        USB_HOST_SERIAL.print(pUsb->regRd(rHCTL), HEX);
        USB_HOST_SERIAL.print(" MODE=0x");
        USB_HOST_SERIAL.print(pUsb->regRd(rMODE), HEX);
        USB_HOST_SERIAL.print(" PERADDR=");
        USB_HOST_SERIAL.println(pUsb->regRd(rPERADDR));

        USB_HOST_SERIAL.print("USBHub Init FAIL addr=");
        USB_HOST_SERIAL.print(bAddress);
        USB_HOST_SERIAL.print(" rcode=0x");
        USB_HOST_SERIAL.println(rcode, HEX);
        USBTRACE("...FAIL\r\n");
        return rcode;
}

uint8_t USBHub::Release() {
        pUsb->GetAddressPool().FreeAddress(bAddress);

        if(bAddress == 0x41)
                pUsb->SetHubPreMask();

        bAddress = 0;
        bNbrPorts = 0;
        bParent = 0;
        bPort = 0;
        qNextPollTime = 0;
        bPollEnable = false;
        return 0;
}

uint8_t USBHub::Poll() {
        uint8_t rcode = 0;

        if(!bPollEnable)
                return 0;

        if(((int32_t)((uint32_t)millis() - qNextPollTime) < 0L))
                return 0;

        /*
         * Nested hubs: avoid the interrupt endpoint entirely. Poll all hub
         * ports with class control requests and feed change events into the
         * existing PortStatusChange() state machine.
         */
        if(bParent != 0) {
                for(uint8_t port = 1; port <= bNbrPorts; port++) {
                        HubEvent evt;
                        evt.bmEvent = 0;
                        const uint8_t scan_rc =
                                GetPortStatus(port, 4, evt.evtBuff);

                        if(scan_rc)
                                continue;

                        if(evt.bmChange &
                           bmHUB_PORT_STATUS_C_PORT_CONNECTION) {
                                rcode = PortStatusChange(port, evt);

                                if(rcode == HUB_ERROR_PORT_HAS_BEEN_RESET) {
                                        qNextPollTime =
                                                (uint32_t)millis() + 100;
                                        return 0;
                                }

                                if(rcode) {
                                        qNextPollTime =
                                                (uint32_t)millis() + 100;
                                        return rcode;
                                }

                                qNextPollTime =
                                        (uint32_t)millis() + 100;
                                return 0;
                        }

                        if(evt.bmChange &
                           bmHUB_PORT_STATUS_C_PORT_RESET) {
                                evt.bmEvent =
                                        bmHUB_PORT_EVENT_RESET_COMPLETE;
                                rcode = PortStatusChange(port, evt);
                                qNextPollTime =
                                        (uint32_t)millis() + 100;
                                return rcode;
                        }
                }

                qNextPollTime = (uint32_t)millis() + 100;
                return 0;
        }

        rcode = CheckHubStatus();
        qNextPollTime = (uint32_t)millis() + 100;
        return rcode;
}

uint8_t USBHub::CheckHubStatus() {
        uint8_t rcode;
        uint8_t buf[8];
        uint16_t read = 1;

        rcode = pUsb->inTransfer(bAddress, 1, &read, buf);

        /*
         * A hub interrupt endpoint normally returns NAK when there is no
         * pending change bitmap.  Do not return early on NAK: the code below
         * also contains a fallback scan for CONNECTED-but-disabled ports.
         * Returning here made that recovery path unreachable exactly when a
         * child connection-change IRQ was missed.
         */
        if(rcode && rcode != hrNAK)
                return rcode;

        if(rcode == hrNAK) {
                buf[0] = 0;
                read = 0;
                rcode = 0;
        }

        //if (buf[0] & 0x01) // Hub Status Change
        //{
        //        pUsb->PrintHubStatus(addr);
        //        rcode = GetHubStatus(1, 0, 1, 4, buf);
        //        if (rcode)
        //        {
        //                USB_HOST_SERIAL.print("GetHubStatus Error");
        //                USB_HOST_SERIAL.println(rcode, HEX);
        //                return rcode;
        //        }
        //}
        for(uint8_t port = 1, mask = 0x02; port < 8; mask <<= 1, port++) {
                if(buf[0] & mask) {
                        HubEvent evt;
                        evt.bmEvent = 0;

                        rcode = GetPortStatus(port, 4, evt.evtBuff);

                        if(rcode)
                                continue;

                        rcode = PortStatusChange(port, evt);

                        if(rcode == HUB_ERROR_PORT_HAS_BEEN_RESET)
                                return 0;

                        if(rcode)
                                return rcode;
                }
        } // for

        for(uint8_t port = 1; port <= bNbrPorts; port++) {
                HubEvent evt;
                evt.bmEvent = 0;

                rcode = GetPortStatus(port, 4, evt.evtBuff);

                if(rcode)
                        continue;

                if((evt.bmStatus & bmHUB_PORT_STATE_CHECK_DISABLED) != bmHUB_PORT_STATE_DISABLED)
                        continue;

                // Emulate connection event for the port
                evt.bmChange |= bmHUB_PORT_STATUS_C_PORT_CONNECTION;

                rcode = PortStatusChange(port, evt);

                if(rcode == HUB_ERROR_PORT_HAS_BEEN_RESET)
                        return 0;

                if(rcode)
                        return rcode;
        } // for
        return 0;
}

void USBHub::ResetHubPort(uint8_t port) {
        HubEvent evt;
        evt.bmEvent = 0;
        uint8_t rcode;

        ClearPortFeature(HUB_FEATURE_C_PORT_ENABLE, port, 0);
        ClearPortFeature(HUB_FEATURE_C_PORT_CONNECTION, port, 0);
        SetPortFeature(HUB_FEATURE_PORT_RESET, port, 0);


        for(int i = 0; i < 3; i++) {
                rcode = GetPortStatus(port, 4, evt.evtBuff);
                if(rcode) break; // Some kind of error, bail.
                if(evt.bmEvent == bmHUB_PORT_EVENT_RESET_COMPLETE || evt.bmEvent == bmHUB_PORT_EVENT_LS_RESET_COMPLETE) {
                        break;
                }
                delay(100); // simulate polling.
        }
        ClearPortFeature(HUB_FEATURE_C_PORT_RESET, port, 0);
        ClearPortFeature(HUB_FEATURE_C_PORT_CONNECTION, port, 0);
        delay(20);
}

uint8_t USBHub::PortStatusChange(uint8_t port, HubEvent &evt) {
        switch(evt.bmEvent) {
                        // Device connected event
                case bmHUB_PORT_EVENT_CONNECT:
                case bmHUB_PORT_EVENT_LS_CONNECT:
                        if(bResetInitiated)
                                return 0;

                        ClearPortFeature(HUB_FEATURE_C_PORT_ENABLE, port, 0);
                        ClearPortFeature(HUB_FEATURE_C_PORT_CONNECTION, port, 0);

                        /*
                         * USB attach debounce.
                         *
                         * The IC-705 contains an internal USB hub.  With a
                         * mechanical unplug/replug, the upstream hub can report
                         * CONNECT before the contact is stable.  Resetting and
                         * enumerating immediately lets the internal hub appear,
                         * only to be followed by a DISCONNECT a few ms later.
                         *
                         * Require about 100 ms of continuously stable
                         * CONNECTION before issuing PORT_RESET.  Limit the
                         * observation window to about 300 ms; if it never
                         * stabilizes, leave the port alone and let the next
                         * real/synthetic connection event retry it.
                         */
                        {
                                uint8_t stable_samples = 0;
                                for(uint8_t attempt = 0;
                                    attempt < 90 && stable_samples < 30;
                                    attempt++) {
                                        delay(10);

                                        HubEvent db_evt;
                                        db_evt.bmEvent = 0;
                                        const uint8_t db_rcode =
                                                GetPortStatus(port, 4, db_evt.evtBuff);

                                        if(db_rcode) {
                                                stable_samples = 0;
                                                continue;
                                        }

                                        const bool connected =
                                                (db_evt.bmStatus &
                                                 bmHUB_PORT_STATUS_PORT_CONNECTION) != 0;
                                        const bool connection_changed =
                                                (db_evt.bmChange &
                                                 bmHUB_PORT_STATUS_C_PORT_CONNECTION) != 0;

                                        if(connection_changed) {
                                                ClearPortFeature(
                                                        HUB_FEATURE_C_PORT_CONNECTION,
                                                        port, 0);
                                                stable_samples = 0;
                                        } else if(connected) {
                                                stable_samples++;
                                        } else {
                                                stable_samples = 0;
                                        }
                                }

                                if(stable_samples < 30)
                                        return 0;
                        }

                        SetPortFeature(HUB_FEATURE_PORT_RESET, port, 0);
                        bResetInitiated = true;
                        return HUB_ERROR_PORT_HAS_BEEN_RESET;

                        // Device disconnected event
                case bmHUB_PORT_EVENT_DISCONNECT: {
                        HubEvent before_clear_evt;
                        before_clear_evt.bmEvent = 0;
                        const uint8_t before_status_rc =
                                GetPortStatus(port, 4, before_clear_evt.evtBuff);

                        /*
                         * External-hub DISCONNECT debounce.
                         *
                         * Keep the current session unless the upstream port
                         * remains continuously disconnected for ~100 ms.
                         * CONNECT-side debounce is left unchanged.
                         */
                        if(bParent == 0) {
                                bool reconnected = false;
                                uint8_t last_rc = before_status_rc;
                                HubEvent last_evt = before_clear_evt;

                                for(uint8_t si = 0; si < 10; si++) {
                                        delay(10);

                                        last_evt.bmEvent = 0;
                                        last_rc = GetPortStatus(port, 4,
                                                last_evt.evtBuff);

                                        if(last_rc == 0 &&
                                           (last_evt.bmStatus &
                                            bmHUB_PORT_STATUS_PORT_CONNECTION)) {
                                                reconnected = true;
                                                break;
                                        }
                                }

                                if(reconnected) {
                                        ClearPortFeature(
                                                HUB_FEATURE_C_PORT_CONNECTION,
                                                port, 0);
                                        return 0;
                                }
                        }

                        /*
                         * A hub change interrupt reports that a connection
                         * changed, but by the time we process it the device may
                         * already have reconnected.  In that case the queued
                         * DISCONNECT is stale.  Releasing the current device
                         * here races with the new session and can destroy a
                         * just-started enumeration.
                         *
                         * If the current port status is CONNECTED, acknowledge
                         * the old change bit and immediately re-enter the normal
                         * CONNECT path using the fresh status.  Do not release
                         * any address from the old DISCONNECT event.
                         */
                        if(before_status_rc == 0 &&
                           (before_clear_evt.bmStatus &
                            bmHUB_PORT_STATUS_PORT_CONNECTION)) {
                                ClearPortFeature(
                                        HUB_FEATURE_C_PORT_CONNECTION,
                                        port, 0);

                                bResetInitiated = false;

                                HubEvent fresh_connect_evt = before_clear_evt;
                                fresh_connect_evt.bmChange |=
                                        bmHUB_PORT_STATUS_C_PORT_CONNECTION;
                                return PortStatusChange(port, fresh_connect_evt);
                        }

                        ClearPortFeature(HUB_FEATURE_C_PORT_ENABLE, port, 0);
                        ClearPortFeature(HUB_FEATURE_C_PORT_CONNECTION, port, 0);

                        bResetInitiated = false;

                        /*
                         * A non-hub child address encodes parent+port, but a
                         * child hub address encodes parent+hub-number instead.
                         * Release by physical parent/port so nested hubs (e.g.
                         * the IC-705 internal hub) are not left consumed.
                         */
                        UsbDeviceAddress parent_addr;
                        parent_addr.devAddress = bAddress;
                        pUsb->ReleaseDeviceAtPort(parent_addr.bmAddress, port);
                        return 0;
                }

                        // Reset complete event
                case bmHUB_PORT_EVENT_RESET_COMPLETE:
                case bmHUB_PORT_EVENT_LS_RESET_COMPLETE:
                        ClearPortFeature(HUB_FEATURE_C_PORT_RESET, port, 0);
                        ClearPortFeature(HUB_FEATURE_C_PORT_CONNECTION, port, 0);

                        delay(20);

                        UsbDeviceAddress reset_parent;
                        reset_parent.devAddress = bAddress;

                        pUsb->Configuring(reset_parent.bmAddress, port,
                                          (evt.bmStatus & bmHUB_PORT_STATUS_PORT_LOW_SPEED));
                        bResetInitiated = false;
                        break;

        } // switch (evt.bmEvent)
        return 0;
}

void PrintHubPortStatus(USBHub *hubptr, uint8_t addr __attribute__((unused)), uint8_t port, bool print_changes) {
        uint8_t rcode = 0;
        HubEvent evt;

        rcode = hubptr->GetPortStatus(port, 4, evt.evtBuff);

        if(rcode) {
                USB_HOST_SERIAL.println("ERROR!");
                return;
        }
        USB_HOST_SERIAL.print("\r\nPort ");
        USB_HOST_SERIAL.println(port, DEC);

        USB_HOST_SERIAL.println("Status");
        USB_HOST_SERIAL.print("CONNECTION:\t");
        USB_HOST_SERIAL.println((evt.bmStatus & bmHUB_PORT_STATUS_PORT_CONNECTION) > 0, DEC);
        USB_HOST_SERIAL.print("ENABLE:\t\t");
        USB_HOST_SERIAL.println((evt.bmStatus & bmHUB_PORT_STATUS_PORT_ENABLE) > 0, DEC);
        USB_HOST_SERIAL.print("SUSPEND:\t");
        USB_HOST_SERIAL.println((evt.bmStatus & bmHUB_PORT_STATUS_PORT_SUSPEND) > 0, DEC);
        USB_HOST_SERIAL.print("OVER_CURRENT:\t");
        USB_HOST_SERIAL.println((evt.bmStatus & bmHUB_PORT_STATUS_PORT_OVER_CURRENT) > 0, DEC);
        USB_HOST_SERIAL.print("RESET:\t\t");
        USB_HOST_SERIAL.println((evt.bmStatus & bmHUB_PORT_STATUS_PORT_RESET) > 0, DEC);
        USB_HOST_SERIAL.print("POWER:\t\t");
        USB_HOST_SERIAL.println((evt.bmStatus & bmHUB_PORT_STATUS_PORT_POWER) > 0, DEC);
        USB_HOST_SERIAL.print("LOW_SPEED:\t");
        USB_HOST_SERIAL.println((evt.bmStatus & bmHUB_PORT_STATUS_PORT_LOW_SPEED) > 0, DEC);
        USB_HOST_SERIAL.print("HIGH_SPEED:\t");
        USB_HOST_SERIAL.println((evt.bmStatus & bmHUB_PORT_STATUS_PORT_HIGH_SPEED) > 0, DEC);
        USB_HOST_SERIAL.print("TEST:\t\t");
        USB_HOST_SERIAL.println((evt.bmStatus & bmHUB_PORT_STATUS_PORT_TEST) > 0, DEC);
        USB_HOST_SERIAL.print("INDICATOR:\t");
        USB_HOST_SERIAL.println((evt.bmStatus & bmHUB_PORT_STATUS_PORT_INDICATOR) > 0, DEC);

        if(!print_changes)
                return;

        USB_HOST_SERIAL.println("\r\nChange");
        USB_HOST_SERIAL.print("CONNECTION:\t");
        USB_HOST_SERIAL.println((evt.bmChange & bmHUB_PORT_STATUS_C_PORT_CONNECTION) > 0, DEC);
        USB_HOST_SERIAL.print("ENABLE:\t\t");
        USB_HOST_SERIAL.println((evt.bmChange & bmHUB_PORT_STATUS_C_PORT_ENABLE) > 0, DEC);
        USB_HOST_SERIAL.print("SUSPEND:\t");
        USB_HOST_SERIAL.println((evt.bmChange & bmHUB_PORT_STATUS_C_PORT_SUSPEND) > 0, DEC);
        USB_HOST_SERIAL.print("OVER_CURRENT:\t");
        USB_HOST_SERIAL.println((evt.bmChange & bmHUB_PORT_STATUS_C_PORT_OVER_CURRENT) > 0, DEC);
        USB_HOST_SERIAL.print("RESET:\t\t");
        USB_HOST_SERIAL.println((evt.bmChange & bmHUB_PORT_STATUS_C_PORT_RESET) > 0, DEC);
}
