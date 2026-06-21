#ifndef _DEVICE_IDENTITY_H_
#define _DEVICE_IDENTITY_H_

#include <string>

class DeviceIdentity {
public:
    static const std::string& GetSerialNumber();
    static const std::string& GetSecret();
};

#endif // _DEVICE_IDENTITY_H_
