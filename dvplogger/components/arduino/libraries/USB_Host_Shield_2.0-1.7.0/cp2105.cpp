/*
 * CP2105 dual USB-UART driver for USB Host Shield Library 2.0
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "cp2105.h"

CP2105::CP2105(USB *usb)
        : pUsb(usb), bAddress(0), bConfNum(0), bNumEP(1), ready(false),
          singlePort(false), deviceVid(0), devicePid(0) {
        resetState();
        if(pUsb)
                pUsb->RegisterDeviceClass(this);
}

void CP2105::resetState() {
        bConfNum = 0;
        bNumEP = 1;
        ready = false;
        singlePort = false;
        deviceVid = 0;
        devicePid = 0;
        for(uint8_t i = 0; i < PORTS; i++) {
                ifaceFound[i] = false;
                ifaceNumber[i] = i;
                configuredBaud[i] = 0;
        }
        for(uint8_t i = 0; i < maxEndpoints; i++) {
                epInfo[i].epAddr = 0;
                epInfo[i].maxPktSize = (i == epControlIndex) ? 8 : 0;
                epInfo[i].bmSndToggle = 0;
                epInfo[i].bmRcvToggle = 0;
                epInfo[i].bmNakPower =
                        (i == epPort0InIndex || i == epPort1InIndex)
                        ? USB_NAK_NOWAIT : USB_NAK_MAX_POWER;
        }
}

uint8_t CP2105::portFromInterface(uint8_t iface) const {
        if(iface == 0) return 0;
        if(iface == 1) return 1;
        return 0xff;
}

void CP2105::EndpointXtract(uint8_t conf, uint8_t iface, uint8_t alt,
                            uint8_t proto __attribute__((unused)),
                            const USB_ENDPOINT_DESCRIPTOR *ep) {
        if(alt != 0 || (ep->bmAttributes & bmUSB_TRANSFER_TYPE) != USB_TRANSFER_TYPE_BULK)
                return;

        uint8_t port = portFromInterface(iface);
        if(port >= PORTS)
                return;

        bConfNum = conf;
        ifaceFound[port] = true;
        ifaceNumber[port] = iface;

        bool input = (ep->bEndpointAddress & 0x80) != 0;
        uint8_t index;
        if(port == 0)
                index = input ? epPort0InIndex : epPort0OutIndex;
        else
                index = input ? epPort1InIndex : epPort1OutIndex;

        if(epInfo[index].epAddr == 0)
                bNumEP++;

        epInfo[index].epAddr = ep->bEndpointAddress & 0x0f;
        epInfo[index].maxPktSize = ep->wMaxPacketSize ? ep->wMaxPacketSize : 64;
        epInfo[index].bmSndToggle = 0;
        epInfo[index].bmRcvToggle = 0;
}

uint8_t CP2105::Init(uint8_t parent, uint8_t port, bool lowspeed) {
        USB_DEVICE_DESCRIPTOR descriptor;
        UsbDevice *device = NULL;
        EpInfo *oldEpInfo = NULL;
        AddressPool &pool = pUsb->GetAddressPool();
        uint8_t rcode;

        if(bAddress)
                return USB_ERROR_CLASS_INSTANCE_ALREADY_IN_USE;

        device = pool.GetUsbDevicePtr(0);
        if(!device)
                return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;
        if(!device->epinfo)
                return USB_ERROR_EPINFO_IS_NULL;

        oldEpInfo = device->epinfo;
        device->epinfo = epInfo;
        device->lowspeed = lowspeed;
        rcode = pUsb->getDevDescr(0, 0, sizeof(descriptor),
                                  reinterpret_cast<uint8_t *>(&descriptor));
        device->epinfo = oldEpInfo;
        if(rcode)
                return rcode;

        if(!VIDPIDOK(descriptor.idVendor, descriptor.idProduct))
                return USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED;

        deviceVid = descriptor.idVendor;
        devicePid = descriptor.idProduct;
        singlePort = (descriptor.idProduct == CP2102_PID);

        bAddress = pool.AllocAddress(parent, false, port);
        if(!bAddress)
                return USB_ERROR_OUT_OF_ADDRESS_SPACE_IN_POOL;

        epInfo[epControlIndex].maxPktSize = descriptor.bMaxPacketSize0;
        if(epInfo[epControlIndex].maxPktSize == 0)
                epInfo[epControlIndex].maxPktSize = 64;

        rcode = pUsb->setAddr(0, 0, bAddress);
        if(rcode) {
                pool.FreeAddress(bAddress);
                bAddress = 0;
                return rcode;
        }

        device->lowspeed = false;
        device = pool.GetUsbDevicePtr(bAddress);
        if(!device) {
                Release();
                return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;
        }
        device->lowspeed = lowspeed;

        rcode = pUsb->setEpInfoEntry(bAddress, 1, epInfo);
        if(rcode) {
                Release();
                return rcode;
        }

        for(uint8_t i = 0; i < descriptor.bNumConfigurations; i++) {
                ConfigDescParser<0xff, 0, 0, CP_MASK_COMPARE_CLASS> parser(this);
                rcode = pUsb->getConfDescr(bAddress, 0, i, &parser);
                if(rcode) {
                        Release();
                        return rcode;
                }
                if(bNumEP == maxEndpoints)
                        break;
        }

        const uint8_t expectedEndpoints = singlePort ? 3 : maxEndpoints;
        if(bNumEP != expectedEndpoints || !ifaceFound[0] ||
           (!singlePort && !ifaceFound[1])) {
                Release();
                return USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED;
        }

        rcode = pUsb->setEpInfoEntry(bAddress, bNumEP, epInfo);
        if(rcode) {
                Release();
                return rcode;
        }

        rcode = pUsb->setConf(bAddress, 0, bConfNum);
        if(rcode) {
                Release();
                return rcode;
        }

        /* FT-991A defaults can be changed later with ConfigurePort(). */
        rcode = ConfigurePort(0, 38400);
        if(rcode) {
                Release();
                return rcode;
        }
        if(!singlePort) {
                rcode = ConfigurePort(1, 38400);
                if(rcode) {
                        Release();
                        return rcode;
                }
        }

        ready = true;
        USBTRACE("CP2105 configured\r\n");
        return 0;
}

uint8_t CP2105::Release() {
        if(bAddress)
                pUsb->GetAddressPool().FreeAddress(bAddress);
        bAddress = 0;
        resetState();
        return 0;
}

uint8_t CP2105::Poll() {
        return 0;
}

bool CP2105::portReady(uint8_t port) const {
        if(!ready || port >= PORTS || !ifaceFound[port])
                return false;
        uint8_t inIndex = port == 0 ? epPort0InIndex : epPort1InIndex;
        uint8_t outIndex = port == 0 ? epPort0OutIndex : epPort1OutIndex;
        return epInfo[inIndex].epAddr != 0 && epInfo[outIndex].epAddr != 0;
}

uint8_t CP2105::controlOut(uint8_t port, uint8_t request,
                           uint16_t value, uint16_t length, uint8_t *data) {
        if(bAddress == 0 || port >= PORTS || !ifaceFound[port])
	  //return USB_ERROR_DEVICE_NOT_SUPPORTED;
	  return USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED;
        return pUsb->ctrlReq(bAddress, 0, CP210X_REQTYPE_OUT, request,
                             value & 0xff, value >> 8, ifaceNumber[port],
                             length, length, data, NULL);
}

uint8_t CP2105::controlIn(uint8_t port, uint8_t request,
                          uint16_t value, uint16_t length, uint8_t *data) {
        if(bAddress == 0 || port >= PORTS || !ifaceFound[port] || !data)
                return USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED;
        return pUsb->ctrlReq(bAddress, 0, CP210X_REQTYPE_IN, request,
                             value & 0xff, value >> 8, ifaceNumber[port],
                             length, length, data, NULL);
}

uint8_t CP2105::EnablePort(uint8_t port, bool enable) {
        return controlOut(port, CP210X_IFC_ENABLE,
                          enable ? CP210X_UART_ENABLE : CP210X_UART_DISABLE);
}

uint8_t CP2105::SetBaudRate(uint8_t port, uint32_t baudrate) {
        uint8_t data[4] = {
                static_cast<uint8_t>(baudrate),
                static_cast<uint8_t>(baudrate >> 8),
                static_cast<uint8_t>(baudrate >> 16),
                static_cast<uint8_t>(baudrate >> 24)
        };
        uint8_t rcode = controlOut(port, CP210X_SET_BAUDRATE, 0,
                                   sizeof(data), data);
        if(!rcode)
                configuredBaud[port] = baudrate;
        return rcode;
}

uint8_t CP2105::SetLineControl(uint8_t port, uint16_t lineCtl) {
        return controlOut(port, CP210X_SET_LINE_CTL, lineCtl);
}

/*
 * CP210x SET_MHS follows the VCP-driver model: bits 8/9 select which
 * modem-control outputs are to be changed, while bits 0/1 carry the
 * requested DTR/RTS state.  Do not rewrite an unchanged output.
 */
uint8_t CP2105::SetModemControl(uint8_t port, uint16_t state, uint16_t mask) {
        uint16_t value = 0;

        if(mask & CP210X_CONTROL_DTR) {
                value |= CP210X_CONTROL_WRITE_DTR;
                if(state & CP210X_CONTROL_DTR)
                        value |= CP210X_CONTROL_DTR;
        }
        if(mask & CP210X_CONTROL_RTS) {
                value |= CP210X_CONTROL_WRITE_RTS;
                if(state & CP210X_CONTROL_RTS)
                        value |= CP210X_CONTROL_RTS;
        }

        if((value & (CP210X_CONTROL_WRITE_DTR |
                     CP210X_CONTROL_WRITE_RTS)) == 0)
                return 0;

        return controlOut(port, CP210X_SET_MHS, value);
}

uint8_t CP2105::SetModemHandshake(uint8_t port, bool dtr, bool rts) {
        uint16_t state = 0;
        if(dtr) state |= CP210X_CONTROL_DTR;
        if(rts) state |= CP210X_CONTROL_RTS;
        return SetModemControl(port, state,
                               CP210X_CONTROL_DTR | CP210X_CONTROL_RTS);
}

uint8_t CP2105::SetDTR(uint8_t port, bool on) {
        return SetModemControl(port, on ? CP210X_CONTROL_DTR : 0,
                               CP210X_CONTROL_DTR);
}

uint8_t CP2105::SetRTS(uint8_t port, bool on) {
        return SetModemControl(port, on ? CP210X_CONTROL_RTS : 0,
                               CP210X_CONTROL_RTS);
}

uint8_t CP2105::GetFlow(uint8_t port, uint8_t data[16]) {
        return controlIn(port, CP210X_GET_FLOW, 0, 16, data);
}

uint8_t CP2105::SetFlow(uint8_t port, const uint8_t data[16]) {
        if(!data) return USB_ERROR_INVALID_ARGUMENT;
        return controlOut(port, CP210X_SET_FLOW, 0, 16,
                          const_cast<uint8_t *>(data));
}

uint8_t CP2105::Purge(uint8_t port) {
        return controlOut(port, CP210X_PURGE, CP210X_PURGE_ALL);
}

uint8_t CP2105::ConfigurePort(uint8_t port, uint32_t baudrate,
                              uint16_t lineCtl) {
        uint8_t rcode = EnablePort(port, true);
        if(rcode) return rcode;
        rcode = SetBaudRate(port, baudrate);
        if(rcode) return rcode;
        rcode = SetLineControl(port, lineCtl);
        if(rcode) return rcode;
        //rcode = SetModemHandshake(port, true, true);
        rcode = SetModemHandshake(port, false, false);	 // DTR RTS off
        if(rcode) return rcode;
        return Purge(port);
}

uint8_t CP2105::RcvData(uint8_t port, uint16_t *bytes_rcvd, uint8_t *data) {
        if(!portReady(port) || !bytes_rcvd || !data)
                return USB_ERROR_INVALID_ARGUMENT;

    uint8_t index =
        port == 0 ? epPort0InIndex : epPort1InIndex;

    return pUsb->inTransfer(
        bAddress,
        epInfo[index].epAddr,
        bytes_rcvd,
        data);
    
    /*        uint8_t index = port == 0 ? epPort0InIndex : epPort1InIndex;
        uint8_t rcode = pUsb->inTransfer(bAddress, epInfo[index].epAddr,
                                         bytes_rcvd, data);
        if(rcode && rcode != hrNAK)
                Release();
		return rcode;*/
}

uint8_t CP2105::SndData(uint8_t port, uint16_t nbytes, uint8_t *data) {
        if(!portReady(port) || !data)
                return USB_ERROR_INVALID_ARGUMENT;
	uint8_t index =
	  port == 0 ? epPort0OutIndex : epPort1OutIndex;
	
	return pUsb->outTransfer(
				 bAddress,
				 epInfo[index].epAddr,
				 nbytes,
				 data);
    
    /*        uint8_t index = port == 0 ? epPort0OutIndex : epPort1OutIndex;
        uint8_t rcode = pUsb->outTransfer(bAddress, epInfo[index].epAddr,
                                          nbytes, data);
        if(rcode && rcode != hrNAK)
                Release();
		return rcode;
    */
}

uint8_t CP2105::interfaceNumber(uint8_t port) const {
        return port < PORTS ? ifaceNumber[port] : 0xff;
}
uint8_t CP2105::inEndpoint(uint8_t port) const {
        if(port >= PORTS) return 0;
        return epInfo[port == 0 ? epPort0InIndex : epPort1InIndex].epAddr;
}
uint8_t CP2105::outEndpoint(uint8_t port) const {
        if(port >= PORTS) return 0;
        return epInfo[port == 0 ? epPort0OutIndex : epPort1OutIndex].epAddr;
}
uint16_t CP2105::inMaxPacket(uint8_t port) const {
        if(port >= PORTS) return 0;
        return epInfo[port == 0 ? epPort0InIndex : epPort1InIndex].maxPktSize;
}
uint16_t CP2105::outMaxPacket(uint8_t port) const {
        if(port >= PORTS) return 0;
        return epInfo[port == 0 ? epPort0OutIndex : epPort1OutIndex].maxPktSize;
}
uint32_t CP2105::baudRate(uint8_t port) const {
        return port < PORTS ? configuredBaud[port] : 0;
}
