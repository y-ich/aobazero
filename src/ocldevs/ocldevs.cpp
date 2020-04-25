// 2019 Team AobaZero
// This source code is in the public domain.
#if defined(USE_OPENCL_AOBA)
#include "err.hpp"
#include "opencl.hpp"
#include <exception>
#include <iostream>
#include <memory>
#include <tuple>
#include <vector>
#include <cstdlib>
using uint = unsigned int;
using std::unique_ptr;
using std::cout;
using std::cerr;
using std::endl;
using std::exception;
using std::get;
using std::make_tuple;
using std::tuple;
using std::vector;
using ErrAux::die;
using namespace OCL;

static void out_platform_info(uint id, const Platform &pf) noexcept {
  cout << "Platform ID: " << id << "\n";
  cout << pf.gen_info(); }

static void out_device_info(uint id, const Device &dev) {
  cout << "- Device ID: " << id << "\n";
  cout << dev.gen_info();

  OCL::Queue queue = dev.gen_queue();
  OCL::Program pg  = queue.gen_program(R"(
__kernel void hw() { printf("  TEST-RUN:             OK\n"); })");
  OCL::Kernel ker  = pg.gen_kernel("hw");
  const size_t size_g[3] = { 1U, 1U, 1U };
  const size_t size_l[3] = { 1U, 1U, 1U };
  queue.push_ndrange_kernel(ker, 3, size_g, size_l);
  queue.finish(); }

int main() {
  uint platform_id = 0;
  uint device_id   = 0;
  vector<Platform> platforms = gen_platforms();
  for (auto &platform : platforms) {
    out_platform_info(platform_id++, platform);
    
    vector<Device> devices = platform.gen_devices_all();
    for (auto &device : devices) {
      try { out_device_info(device_id, device); }
      catch (const exception &e) { cout << e.what() << endl; }
      device_id += 1U; } }
  return 0; }
#else
#include <iostream>
int main() {
  std::cout << "This program cannot use OpenCL." << std::endl;
  return 0; }
#endif
