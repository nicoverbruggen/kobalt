#pragma once
#include "platform/api/reader_host_services.h"
namespace kobalt::kindle {
class Device;
ReaderHostServices readerServices(std::shared_ptr<Device> device);
}
