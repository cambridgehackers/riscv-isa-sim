#include "devices.h"

void bus_t::add_device(reg_t addr, abstract_device_t* dev)
{
  devices[-addr] = dev;
}

bool bus_t::has_interrupt()
{
  for (auto device: devices) {
    if (device.second->has_interrupt())
      return true;
  }
  return false;
}

bool bus_t::load(reg_t addr, size_t len, uint8_t* bytes)
{
  auto it = devices.lower_bound(-addr);
  if (it == devices.end()) {
    return false;
  } else {
    bool b = it->second->load(addr - -it->first, len, bytes);
    if (!b) {
      fprintf(stderr, "bus_t::load addr=%08lx len=%08lx it->first=%08lx it=>second=%p b=%d\n", addr, len, -it->first, it->second, b);
      for (auto p:  devices) {
	fprintf(stderr, "        addr=%08lx len=%08lx it->first=%08lx it=>second=%p\n", -addr, len, p.first, p.second);
      }
      for (auto p:  devices) {
	fprintf(stderr, "        addr=%08lx len=%08lx it->first=%08lx it=>second=%p\n", addr, len, -p.first, p.second);
      }
      for (auto p:  devices) {
	fprintf(stderr, "        addr=%ld len=%08lx it->first=%ld it=>second=%p\n", -addr, len, p.first, p.second);
      }
      exit(-1);
    }
    return b;
  }
}

bool bus_t::store(reg_t addr, size_t len, const uint8_t* bytes)
{
  auto it = devices.lower_bound(-addr);
  if (it == devices.end())
    return false;
  return it->second->store(addr - -it->first, len, bytes);
}

rom_device_t::rom_device_t(std::vector<char> data)
  : data(data)
{
}

bool rom_device_t::load(reg_t addr, size_t len, uint8_t* bytes)
{
  if (addr + len > data.size())
    return false;
  memcpy(bytes, &data[addr], len);
  return true;
}

bool rom_device_t::store(reg_t addr, size_t len, const uint8_t* bytes)
{
  return false;
}

std::map<reg_t, std::function<abstract_device_t*()>>& devices()
{
  static std::map<reg_t, std::function<abstract_device_t*()>> v;
  return v;
}

void register_device(reg_t addr, std::function<abstract_device_t*()> f)
{
  devices()[addr] = f;
}
