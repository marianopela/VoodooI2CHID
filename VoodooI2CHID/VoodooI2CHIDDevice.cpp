//
//  VoodooI2CHIDDevice.cpp
//  VoodooI2CHID
//
//  Created by Alexandre on 25/08/2017.
//  Copyright © 2017 Alexandre Daoud. All rights reserved.
//

#include <IOKit/hid/IOHIDDevice.h>
#include <kern/locks.h>
#include "VoodooI2CHIDDevice.hpp"
#include "../../../VoodooI2C/VoodooI2C/VoodooI2CDevice/VoodooI2CDeviceNub.hpp"

#define super IOHIDDevice
OSDefineMetaClassAndStructors(VoodooI2CHIDDevice, IOHIDDevice);

extern volatile AbsoluteTime last_multi_touch_event;

bool VoodooI2CHIDDevice::init(OSDictionary* properties) {
    if (!super::init(properties))
        return false;
    awake = true;
    read_in_progress = false;
    read_in_progress_mutex = IOLockAlloc();
    ready_for_input = false;
    reset_event = false;
    memset(&hid_descriptor, 0, sizeof(VoodooI2CHIDDeviceHIDDescriptor));
    acpi_device = NULL;
    api = NULL;
    command_gate = NULL;
    interrupt_simulator = NULL;
    ready_for_input = false;
    
    client_lock = IOLockAlloc();
    
    clients = OSArray::withCapacity(1);

    if (!client_lock || !clients) {
        OSSafeReleaseNULL(clients);
        return false;
    }
    
    buf_i2c_cnt_intr = 0;
    buf_i2c_cnt = 0;
    buf_i2c_pool_intr = (UInt8 *)IOMalloc(I2C_MAX_BUF_SIZE);
    buf_i2c_pool = (UInt8 *)IOMalloc(I2C_MAX_BUF_SIZE);

    return true;
}

void VoodooI2CHIDDevice::free() {
    if (client_lock)
        IOLockFree(client_lock);
    
    IOFree(buf_i2c_pool_intr, I2C_MAX_BUF_SIZE);
    IOFree(buf_i2c_pool, I2C_MAX_BUF_SIZE);
    if (read_in_progress_mutex) {
        IOLockFree(read_in_progress_mutex);
        read_in_progress_mutex = NULL;
    }

    super::free();
}

UInt8* VoodooI2CHIDDevice::getMallocI2CIntr(UInt16 size) {
    if ((buf_i2c_cnt_intr + size + 0x10) >= I2C_MAX_BUF_SIZE)
        buf_i2c_cnt_intr = 0;
    UInt8* retaddr = (buf_i2c_pool_intr + buf_i2c_cnt_intr);
    buf_i2c_cnt_intr += size + 0x10;

    return retaddr;
}

UInt8* VoodooI2CHIDDevice::getMallocI2C(UInt16 size) {
    if ((buf_i2c_cnt + size + 0x10) >= I2C_MAX_BUF_SIZE)
        buf_i2c_cnt = 0;
    UInt8* retaddr = (buf_i2c_pool + buf_i2c_cnt);
    buf_i2c_cnt += size + 0x10;

    return retaddr;
}

IOReturn VoodooI2CHIDDevice::getHIDDescriptor() {
    I2C_LOCK();
    read_in_progress = true;
    VoodooI2CHIDDeviceCommand* command = (VoodooI2CHIDDeviceCommand*)getMallocI2C(sizeof(VoodooI2CHIDDeviceCommand));
    command->c.reg = hid_descriptor_register;

    if (api->writeReadI2C(command->data, 2, (UInt8*)&hid_descriptor, (UInt16)sizeof(VoodooI2CHIDDeviceHIDDescriptor)) != kIOReturnSuccess) {
        IOLog("%s::%s Request for HID descriptor failed\n", getName(), name);
        read_in_progress = false;
        I2C_UNLOCK();
        return kIOReturnIOError;
    }
    
    IOReturn ret = parseHIDDescriptor();
    read_in_progress = false;
    I2C_UNLOCK();

    return ret;
}

IOReturn VoodooI2CHIDDevice::parseHIDDescriptor() {
    if (hid_descriptor.bcdVersion != 0x0100) {
        IOLog("%s::%s Incorrect BCD version %d\n", getName(), name, hid_descriptor.bcdVersion);
        return kIOReturnInvalid;
    }
    
    if (hid_descriptor.wHIDDescLength != sizeof(VoodooI2CHIDDeviceHIDDescriptor)) {
        IOLog("%s::%s Unexpected size of HID descriptor\n", getName(), name);
        return kIOReturnInvalid;
    }

    OSDictionary* property_array = OSDictionary::withCapacity(1);
    property_array->setObject("HIDDescLength", OSNumber::withNumber(hid_descriptor.wHIDDescLength, 32));
    property_array->setObject("BCDVersion", OSNumber::withNumber(hid_descriptor.bcdVersion, 32));
    property_array->setObject("ReportDescLength", OSNumber::withNumber(hid_descriptor.wReportDescLength, 32));
    property_array->setObject("ReportDescRegister", OSNumber::withNumber(hid_descriptor.wReportDescRegister, 32));
    property_array->setObject("MaxInputLength", OSNumber::withNumber(hid_descriptor.wMaxInputLength, 32));
    property_array->setObject("InputRegister", OSNumber::withNumber(hid_descriptor.wInputRegister, 32));
    property_array->setObject("MaxOutputLength", OSNumber::withNumber(hid_descriptor.wMaxOutputLength, 32));
    property_array->setObject("OutputRegister", OSNumber::withNumber(hid_descriptor.wOutputRegister, 32));
    property_array->setObject("CommandRegister", OSNumber::withNumber(hid_descriptor.wCommandRegister, 32));
    property_array->setObject("DataRegister", OSNumber::withNumber(hid_descriptor.wDataRegister, 32));
    property_array->setObject("VendorID", OSNumber::withNumber(hid_descriptor.wVendorID, 32));
    property_array->setObject("ProductID", OSNumber::withNumber(hid_descriptor.wProductID, 32));
    property_array->setObject("VersionID", OSNumber::withNumber(hid_descriptor.wVersionID, 32));

    setProperty("HIDDescriptor", property_array);

    property_array->release();

    return kIOReturnSuccess;
}

IOReturn VoodooI2CHIDDevice::getHIDDescriptorAddress() {
    UInt32 guid_1 = 0x3CDFF6F7;
    UInt32 guid_2 = 0x45554267;
    UInt32 guid_3 = 0x0AB305AD;
    UInt32 guid_4 = 0xDE38893D;
    
    OSObject *result = NULL;
    OSObject *params[4];
    char buffer[16];
    
    memcpy(buffer, &guid_1, 4);
    memcpy(buffer + 4, &guid_2, 4);
    memcpy(buffer + 8, &guid_3, 4);
    memcpy(buffer + 12, &guid_4, 4);
    
    
    params[0] = OSData::withBytes(buffer, 16);
    params[1] = OSNumber::withNumber(0x1, 8);
    params[2] = OSNumber::withNumber(0x1, 8);
    params[3] = OSNumber::withNumber((unsigned long long)0x0, 8);
    
    acpi_device->evaluateObject("_DSM", &result, params, 4);
    if (!result)
        acpi_device->evaluateObject("XDSM", &result, params, 4);
    if (!result) {
        IOLog("%s::%s Could not find suitable _DSM or XDSM method in ACPI tables\n", getName(), name);
        return kIOReturnNotFound;
    }
    
    OSNumber* number = OSDynamicCast(OSNumber, result);
    if (number) {
        setProperty("HIDDescriptorAddress", number);
        hid_descriptor_register = number->unsigned16BitValue();
    }

    if (result)
        result->release();
    
    params[0]->release();
    params[1]->release();
    params[2]->release();
    params[3]->release();
    
    if (!number) {
        IOLog("%s::%s HID descriptor register invalid\n", getName(), name);
        return kIOReturnInvalid;
    }
    
    return kIOReturnSuccess;
}

void VoodooI2CHIDDevice::getInputReport() {
    IOBufferMemoryDescriptor* buffer;
    IOReturn ret;
    int return_size;
    unsigned char* report;
    
    if (I2C_TRYLOCK() == false) {
        IOLog("%s::%s Skipping a HID read interrupt while other thread read/write I2C\n", getName(), name);
        return;
    }
    
    read_in_progress = true;
    
    report = getMallocI2CIntr(hid_descriptor.wMaxInputLength);
    report[0] = report[1] = 0;
    ret = api->readI2C(report, hid_descriptor.wMaxInputLength);
    if (ret != kIOReturnSuccess) {
        goto exit;
    }

    return_size = report[0] | report[1] << 8;
    /*
     * "return_size" can be 0 when resetHIDDevice() is called; booting or waking up from sleep.
     * Since ready_for_input can still be FALSE when booting,
     * It needs to be checked before checking ready_for_input.
     */
    if (!return_size) {
        command_gate->commandWakeup(&reset_event);
        goto exit;
    }

    if (!ready_for_input || return_size > hid_descriptor.wMaxInputLength) {
        goto exit;
    }

    buffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, 0, return_size);
    buffer->writeBytes(0, report + 2, return_size - 2);

    ret = handleReport(buffer, kIOHIDReportTypeInput);

    if (ret != kIOReturnSuccess)
        IOLog("%s::%s Error handling input report: 0x%.8x\n", getName(), name, ret);
    
    buffer->release();

exit:
    read_in_progress = false;
    I2C_UNLOCK();
}

IOReturn VoodooI2CHIDDevice::getReport(IOMemoryDescriptor* report, IOHIDReportType reportType, IOOptionBits options) {
    if (reportType != kIOHIDReportTypeFeature && reportType != kIOHIDReportTypeInput)
        return kIOReturnBadArgument;

    UInt8 args[3];
    IOReturn ret;
    int args_len = 0;
    UInt16 read_register = hid_descriptor.wDataRegister;
    UInt8 report_id = options & 0xFF;
    UInt8 raw_report_type = (reportType == kIOHIDReportTypeFeature) ? 0x03 : 0x01;
    
    if (report_id >= 0x0F) {
        args[args_len++] = report_id;
        report_id = 0x0F;
    }

    args[args_len++] = read_register & 0xFF;
    args[args_len++] = read_register >> 8;
    
    I2C_LOCK();
    read_in_progress = true;

    UInt8 length = sizeof(VoodooI2CHIDDeviceCommand);
    UInt8* buffer = (UInt8*) getMallocI2C(report->getLength());
    memset(buffer, 0, report->getLength());

    VoodooI2CHIDDeviceCommand* command = (VoodooI2CHIDDeviceCommand*) getMallocI2C(length + args_len);
    memset(command, 0, length + args_len);
    command->c.reg = hid_descriptor.wCommandRegister;
    command->c.opcode = 0x02;
    command->c.report_type_id = report_id | raw_report_type << 4;
    
    UInt8* raw_command = (UInt8*)command;
    
    memcpy(raw_command + length, args, args_len);
    ret = api->writeReadI2C(raw_command, length+args_len, buffer, report->getLength());
    
    report->writeBytes(0, buffer+2, report->getLength()-2);
    
    read_in_progress = false;
    I2C_UNLOCK();

    return ret;
}

void VoodooI2CHIDDevice::interruptOccured(OSObject* owner, IOInterruptEventSource* src, int intCount) {
    if (read_in_progress)
        return;
    if (!awake)
        return;

    command_gate->attemptAction(OSMemberFunctionCast(IOCommandGate::Action, this, &VoodooI2CHIDDevice::getInputReport));
}

VoodooI2CHIDDevice* VoodooI2CHIDDevice::probe(IOService* provider, SInt32* score) {
    if (!super::probe(provider, score))
        return NULL;

    name = getMatchedName(provider);
    
    acpi_device = OSDynamicCast(IOACPIPlatformDevice, provider->getProperty("acpi-device"));
    //acpi_device->retain();
    
    if (!acpi_device) {
        IOLog("%s::%s Could not get ACPI device\n", getName(), name);
        return NULL;
    }
    
    // Sometimes an I2C HID will have power state methods, lets turn it on in case
    
    acpi_device->evaluateObject("_PS0");

    api = OSDynamicCast(VoodooI2CDeviceNub, provider);
    //api->retain();
    
    if (!api) {
        IOLog("%s::%s Could not get VoodooI2C API access\n", getName(), name);
        return NULL;
    }
    
    if (getHIDDescriptorAddress() != kIOReturnSuccess) {
        IOLog("%s::%s Could not get HID descriptor\n", getName(), name);
        return NULL;
    }

    if (getHIDDescriptor() != kIOReturnSuccess) {
        IOLog("%s::%s Could not get HID descriptor\n", getName(), name);
        return NULL;
    }

    return this;
}

void VoodooI2CHIDDevice::releaseResources() {
    if (command_gate) {
        command_gate->disable();
        work_loop->removeEventSource(command_gate);
        command_gate->release();
        command_gate = NULL;
    }
    
    if (interrupt_simulator) {
        interrupt_simulator->disable();
        work_loop->removeEventSource(interrupt_simulator);
        interrupt_simulator->release();
        interrupt_simulator = NULL;
    }

    api->disableInterrupt(0);
    api->unregisterInterrupt(0);

    if (work_loop) {
        work_loop->release();
        work_loop = NULL;
    }
    
    if (acpi_device) {
        acpi_device->release();
        acpi_device = NULL;
    }
    
    if (api) {
        if (api->isOpen(this))
            api->close(this);
        api->release();
        api = NULL;
    }
}

IOReturn VoodooI2CHIDDevice::resetHIDDevice() {
    return command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &VoodooI2CHIDDevice::resetHIDDeviceGated));
}

IOReturn VoodooI2CHIDDevice::resetHIDDeviceGated() {
    setHIDPowerState(kVoodooI2CStateOn);

    I2C_LOCK();
    read_in_progress = true;
    VoodooI2CHIDDeviceCommand* command = (VoodooI2CHIDDeviceCommand*) getMallocI2C(sizeof(VoodooI2CHIDDeviceCommand));
    command->c.reg = hid_descriptor.wCommandRegister;
    command->c.opcode = 0x01;
    command->c.report_type_id = 0;

    api->writeI2C(command->data, sizeof(VoodooI2CHIDDeviceCommand));
    IOSleep(100);

    AbsoluteTime absolute_time, deadline;

    // Device is required to complete a host-initiated reset in at most 6 seconds.

    nanoseconds_to_absolutetime(6000000000, &absolute_time);

    clock_absolutetime_interval_to_deadline(absolute_time, &deadline);
    IOReturn sleep = command_gate->commandSleep(&reset_event, deadline, THREAD_ABORTSAFE);

    if (sleep == THREAD_TIMED_OUT) {
        IOLog("%s::%s Timeout waiting for device to complete host initiated reset\n", getName(), name);
        read_in_progress = false;
        I2C_UNLOCK();
        return kIOReturnTimeout;
    }
    
    read_in_progress = false;
    I2C_UNLOCK();

    return kIOReturnSuccess;
}

IOReturn VoodooI2CHIDDevice::setHIDPowerState(VoodooI2CState state) {
    I2C_LOCK();
    read_in_progress = true;
    
    IOReturn ret = kIOReturnSuccess;
    int attempts = 5;
    do {
        VoodooI2CHIDDeviceCommand* command = (VoodooI2CHIDDeviceCommand*) getMallocI2C(sizeof(VoodooI2CHIDDeviceCommand));
        command->c.reg = hid_descriptor.wCommandRegister;
        command->c.opcode = 0x08;
        command->c.report_type_id = state ? I2C_HID_PWR_ON : I2C_HID_PWR_SLEEP;

        ret = api->writeI2C(command->data, sizeof(VoodooI2CHIDDeviceCommand));
        IOSleep(100);
    } while (ret != kIOReturnSuccess && --attempts >= 0);
    
    read_in_progress = false;
    I2C_UNLOCK();
    
    return ret;
}

IOReturn VoodooI2CHIDDevice::setReport(IOMemoryDescriptor* report, IOHIDReportType reportType, IOOptionBits options) {
    if (reportType != kIOHIDReportTypeFeature && reportType != kIOHIDReportTypeOutput)
        return kIOReturnBadArgument;
    
    UInt16 data_register = hid_descriptor.wDataRegister;
    UInt8 raw_report_type = (reportType == kIOHIDReportTypeFeature) ? 0x03 : 0x02;
    UInt8 idx = 0;
    UInt16 size;
    UInt16 arguments_length;
    UInt8 report_id = options & 0xFF;
    UInt8* buffer = (UInt8*)IOMalloc(report->getLength());
    report->readBytes(0, buffer, report->getLength());
    
    size = 2 +
    (report_id ? 1 : 0)     /* reportID */ +
    report->getLength()     /* buf */;

    arguments_length = (report_id >= 0x0F ? 1 : 0)  /* optional third byte */ +
    2                                               /* dataRegister */ +
    size                                            /* args */;
    
    UInt8* arguments = (UInt8*)IOMalloc(arguments_length);
    memset(arguments, 0, arguments_length);
    
    if (report_id >= 0x0F) {
        arguments[idx++] = report_id;
        report_id = 0x0F;
    }
    
    arguments[idx++] = data_register & 0xFF;
    arguments[idx++] = data_register >> 8;
    
    arguments[idx++] = size & 0xFF;
    arguments[idx++] = size >> 8;
    
    if (report_id)
        arguments[idx++] = report_id;
    
    memcpy(&arguments[idx], buffer, report->getLength());
    
    I2C_LOCK();
    read_in_progress = true;

    UInt8 length = sizeof(VoodooI2CHIDDeviceCommand);
    VoodooI2CHIDDeviceCommand* command = (VoodooI2CHIDDeviceCommand*) getMallocI2C(length + arguments_length);
    memset(command, 0, length + arguments_length);
    command->c.reg = hid_descriptor.wCommandRegister;
    command->c.opcode = 0x03;
    command->c.report_type_id = report_id | raw_report_type << 4;
    
    UInt8* raw_command = (UInt8*)command;
    
    memcpy(raw_command + length, arguments, arguments_length);
    IOReturn ret = api->writeI2C(raw_command, length+arguments_length);
    IOSleep(10);
    
    read_in_progress = false;
    I2C_UNLOCK();
    
    IOFree(arguments, arguments_length);
    IOFree(buffer, report->getLength());

    return ret;
}

IOReturn VoodooI2CHIDDevice::setPowerState(unsigned long whichState, IOService* whatDevice) {
    if (whatDevice != this)
        return kIOReturnInvalid;
    if (whichState == 0) {
        if (awake) {
            awake = false;

            setHIDPowerState(kVoodooI2CStateOff);
            
            IOLog("%s::%s Going to sleep\n", getName(), name);
        }
    } else {
        if (!awake) {
            setHIDPowerState(kVoodooI2CStateOn);
            
            IOSleep(1);
            
            I2C_LOCK();
            read_in_progress = true;
            
            VoodooI2CHIDDeviceCommand *command = (VoodooI2CHIDDeviceCommand*) getMallocI2C(sizeof(VoodooI2CHIDDeviceCommand));
            command->c.reg = hid_descriptor.wCommandRegister;
            command->c.opcode = 0x01;
            command->c.report_type_id = 0;
            
            api->writeI2C(command->data, sizeof(VoodooI2CHIDDeviceCommand));
            
            awake = true;
            read_in_progress = false;
            I2C_UNLOCK();
            
            IOLog("%s::%s Woke up\n", getName(), name);
        }
    }
    return kIOPMAckImplied;
}

bool VoodooI2CHIDDevice::handleStart(IOService* provider) {
    if (!IOHIDDevice::handleStart(provider)) {
        return false;
    }

    work_loop = getWorkLoop();
    
    if (!work_loop) {
        IOLog("%s::%s Could not get work loop\n", getName(), name);
        goto exit;
    }

    work_loop->retain();

    command_gate = IOCommandGate::commandGate(this);
    if (!command_gate || (work_loop->addEventSource(command_gate) != kIOReturnSuccess)) {
        IOLog("%s::%s Could not open command gate\n", getName(), name);
        goto exit;
    }

    acpi_device->retain();
    api->retain();

    if (!api->open(this)) {
        IOLog("%s::%s Could not open API\n", getName(), name);
        goto exit;
    }
    
    /* ISR should work in VoodooGPIO's workloop to block the level interrupt, so use direct interrupt here. */
    if (api->registerInterrupt(0, this, OSMemberFunctionCast(IOInterruptAction, this, &VoodooI2CHIDDevice::interruptOccured), 0) != kIOReturnSuccess) {
        IOLog("%s::%s Warning: Could not get interrupt event source, using polling instead\n", getName(), name);
        interrupt_simulator = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &VoodooI2CHIDDevice::simulateInterrupt));
        
        if (!interrupt_simulator) {
            IOLog("%s::%s Could not get timer event source\n", getName(), name);
            goto exit;
        }
        work_loop->addEventSource(interrupt_simulator);
        interrupt_simulator->setTimeoutMS(200);
    } else {
        api->enableInterrupt(0);
    }

    resetHIDDevice();


    PMinit();
    api->joinPMtree(this);
    registerPowerDriver(this, VoodooI2CIOPMPowerStates, kVoodooI2CIOPMNumberPowerStates);
    
    // Give the reset a bit of time so that IOHIDDevice doesnt happen to start requesting the report
    // descriptor before the driver is ready

    IOSleep(100);

    return true;
exit:
    releaseResources();
    return false;
}

bool VoodooI2CHIDDevice::start(IOService* provider) {
    if (!super::start(provider))
        return false;

    ready_for_input = true;

    setProperty("VoodooI2CServices Supported", kOSBooleanTrue);

    return true;
}

void VoodooI2CHIDDevice::stop(IOService* provider) {
    IOLockLock(client_lock);
    for(;;) {
        if (!clients->getCount()) {
            break;
        }
        
        IOLockSleep(client_lock, &client_lock, THREAD_UNINT);
    }
    IOLockUnlock(client_lock);
    
    releaseResources();
    OSSafeReleaseNULL(clients);
    PMstop();
    super::stop(provider);
}

IOReturn VoodooI2CHIDDevice::newReportDescriptor(IOMemoryDescriptor** descriptor) const {
    if (!hid_descriptor.wReportDescLength) {
        IOLog("%s::%s Invalid report descriptor size\n", getName(), name);
        return kIOReturnDeviceError;
    }
    
    I2C_LOCK();

    VoodooI2CHIDDeviceCommand command;
    command.c.reg = hid_descriptor.wReportDescRegister;
    
    UInt8* buffer = reinterpret_cast<UInt8*>(IOMalloc(hid_descriptor.wReportDescLength));
    memset(buffer, 0, hid_descriptor.wReportDescLength);

    if (api->writeReadI2C(command.data, 2, buffer, hid_descriptor.wReportDescLength) != kIOReturnSuccess) {
        I2C_UNLOCK();
        IOFree(buffer, hid_descriptor.wReportDescLength);
        IOLog("%s::%s Could not get report descriptor\n", getName(), name);
        return kIOReturnIOError;
    }

    IOBufferMemoryDescriptor* report_descriptor = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, 0, hid_descriptor.wReportDescLength);

    if (!report_descriptor) {
        I2C_UNLOCK();
        IOFree(buffer, hid_descriptor.wReportDescLength);
        IOLog("%s::%s Could not allocated buffer for report descriptor\n", getName(), name);
        return kIOReturnNoResources;
    }

    report_descriptor->writeBytes(0, buffer, hid_descriptor.wReportDescLength);
    *descriptor = report_descriptor;
    
    I2C_UNLOCK();

    IOFree(buffer, hid_descriptor.wReportDescLength);

    return kIOReturnSuccess;
}

OSNumber* VoodooI2CHIDDevice::newVendorIDNumber() const {
    return OSNumber::withNumber(hid_descriptor.wVendorID, 16);
}

OSNumber* VoodooI2CHIDDevice::newProductIDNumber() const {
    return OSNumber::withNumber(hid_descriptor.wProductID, 16);
}

OSNumber* VoodooI2CHIDDevice::newVersionNumber() const {
    return OSNumber::withNumber(hid_descriptor.wVersionID, 16);
}

OSString* VoodooI2CHIDDevice::newTransportString() const {
    return OSString::withCString("I2C");
}

OSString* VoodooI2CHIDDevice::newManufacturerString() const {
    return OSString::withCString("Apple");
}

void VoodooI2CHIDDevice::simulateInterrupt(OSObject* owner, IOTimerEventSource* timer) {
    AbsoluteTime prev_time = last_multi_touch_event;
    if (!read_in_progress && awake) {
        VoodooI2CHIDDevice::getInputReport();
    }
    
    if (last_multi_touch_event == 0) {
        interrupt_simulator->setTimeoutMS(INTERRUPT_SIMULATOR_TIMEOUT);
        return;
    }
    
    IOSleep(1);
    if (last_multi_touch_event != prev_time) {
        interrupt_simulator->setTimeoutMS(INTERRUPT_SIMULATOR_TIMEOUT_BUSY);
        return;
    }
        
    uint64_t        nsecs;
    AbsoluteTime    cur_time;
    clock_get_uptime(&cur_time);
    SUB_ABSOLUTETIME(&cur_time, &last_multi_touch_event);
    absolutetime_to_nanoseconds(cur_time, &nsecs);
    interrupt_simulator->setTimeoutMS((nsecs > 1500000000) ? INTERRUPT_SIMULATOR_TIMEOUT_IDLE : INTERRUPT_SIMULATOR_TIMEOUT_BUSY);
}

bool VoodooI2CHIDDevice::open(IOService *forClient, IOOptionBits options, void *arg) {
    IOLockLock(client_lock);
    clients->setObject(forClient);
    IOUnlock(client_lock);
    
    return super::open(forClient, options, arg);
}

void VoodooI2CHIDDevice::close(IOService *forClient, IOOptionBits options) {
    IOLockLock(client_lock);
    
    for(int i = 0; i < clients->getCount(); i++) {
        OSObject* service = clients->getObject(i);
        
        if (service == forClient) {
            clients->removeObject(i);
            break;
        }
    }
    
    IOUnlock(client_lock);

    IOLockWakeup(client_lock, &client_lock, true);
    
    super::close(forClient, options);
}
