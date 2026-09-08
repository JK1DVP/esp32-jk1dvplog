/*
 * CP2105 dual USB-UART driver for USB Host Shield Library 2.0
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef __CP2105_H__
#define __CP2105_H__

#include "Usb.h"

#define CP2105_VID                 0x10C4
#define CP2105_PID_FACTORY         0xEA70
#define CP2105_PID_WINDOWS         0xEA7A
#define CP2102_PID                 0xEA60

#define CP210X_REQTYPE_OUT         0x41
#define CP210X_REQTYPE_IN          0xC1
#define CP210X_IFC_ENABLE          0x00
#define CP210X_SET_LINE_CTL        0x03
#define CP210X_SET_MHS             0x07
#define CP210X_PURGE               0x12
#define CP210X_SET_FLOW            0x13
#define CP210X_GET_FLOW            0x14
#define CP210X_SET_BAUDRATE        0x1E

#define CP210X_UART_ENABLE         0x0001
#define CP210X_UART_DISABLE        0x0000
#define CP210X_BITS_8              0x0800
#define CP210X_PARITY_NONE         0x0000
#define CP210X_STOP_1              0x0000
#define CP210X_CONTROL_DTR         0x0001
#define CP210X_CONTROL_RTS         0x0002
#define CP210X_CONTROL_WRITE_DTR   0x0100
#define CP210X_CONTROL_WRITE_RTS   0x0200
#define CP210X_PURGE_ALL           0x000F

class CP2105 : public USBDeviceConfig, public UsbConfigXtracter {
public:
        static const uint8_t PORTS = 2;

        CP2105(USB *pusb);

        uint8_t Init(uint8_t parent, uint8_t port, bool lowspeed);
        uint8_t Release();
        uint8_t Poll();

        void EndpointXtract(uint8_t conf, uint8_t iface, uint8_t alt,
                            uint8_t proto, const USB_ENDPOINT_DESCRIPTOR *ep);

        bool VIDPIDOK(uint16_t vid, uint16_t pid) {
                return vid == CP2105_VID &&
                       (pid == CP2105_PID_FACTORY || pid == CP2105_PID_WINDOWS ||
                        pid == CP2102_PID);
        }

        bool isReady() const { return ready; }
        bool isSinglePort() const { return singlePort; }
        uint16_t GetVid() const { return deviceVid; }
        uint16_t GetPid() const { return devicePid; }
        bool portReady(uint8_t port) const;
        uint8_t GetAddress() { return bAddress; }

        uint8_t ConfigurePort(uint8_t port, uint32_t baudrate,
                              uint16_t lineCtl = CP210X_BITS_8 |
                                                 CP210X_PARITY_NONE |
                                                 CP210X_STOP_1);
        uint8_t EnablePort(uint8_t port, bool enable);
        uint8_t SetBaudRate(uint8_t port, uint32_t baudrate);
        uint8_t SetLineControl(uint8_t port, uint16_t lineCtl);
        uint8_t SetModemControl(uint8_t port, uint16_t state, uint16_t mask);
        uint8_t SetModemHandshake(uint8_t port, bool dtr, bool rts);
        uint8_t SetDTR(uint8_t port, bool on);
        uint8_t SetRTS(uint8_t port, bool on);
        uint8_t GetFlow(uint8_t port, uint8_t data[16]);
        uint8_t SetFlow(uint8_t port, const uint8_t data[16]);
        uint8_t Purge(uint8_t port);

        uint8_t RcvData(uint8_t port, uint16_t *bytes_rcvd, uint8_t *data);
        uint8_t SndData(uint8_t port, uint16_t nbytes, uint8_t *data);

        uint8_t interfaceNumber(uint8_t port) const;
        uint8_t inEndpoint(uint8_t port) const;
        uint8_t outEndpoint(uint8_t port) const;
        uint16_t inMaxPacket(uint8_t port) const;
        uint16_t outMaxPacket(uint8_t port) const;
        uint32_t baudRate(uint8_t port) const;

private:
        enum {
                epControlIndex = 0,
                epPort0InIndex = 1,
                epPort0OutIndex = 2,
                epPort1InIndex = 3,
                epPort1OutIndex = 4,
                maxEndpoints = 5
        };

        USB *pUsb;
        uint8_t bAddress;
        uint8_t bConfNum;
        uint8_t bNumEP;
        bool ready;
        bool singlePort;
        uint16_t deviceVid;
        uint16_t devicePid;
        bool ifaceFound[PORTS];
        uint8_t ifaceNumber[PORTS];
        uint32_t configuredBaud[PORTS];
        EpInfo epInfo[maxEndpoints];

        uint8_t portFromInterface(uint8_t iface) const;
        uint8_t controlOut(uint8_t port, uint8_t request,
                           uint16_t value, uint16_t length = 0,
                           uint8_t *data = NULL);
        uint8_t controlIn(uint8_t port, uint8_t request,
                          uint16_t value, uint16_t length, uint8_t *data);
        void resetState();
};

#endif
