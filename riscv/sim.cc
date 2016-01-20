// See LICENSE for license details.

#include "sim.h"
#include "htif.h"
#include <map>
#include <iostream>
#include <climits>
#include <cstdlib>
#include <cassert>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <portal.h>
#include <sys/mman.h>

volatile bool ctrlc_pressed = false;
static void handle_signal(int sig)
{
  if (ctrlc_pressed)
    exit(-1);
  ctrlc_pressed = true;
  signal(sig, &handle_signal);
}

sim_t::sim_t(const char* isa, size_t nprocs, size_t mem_mb,
             const std::vector<std::string>& args)
  : //htif(new htif_isasim_t(this, args)), // [sizhuo] create HTIF later 
        mem(0), memfd(0),
	procs(std::max(nprocs, size_t(1))),
	rtc(0), current_step(0), current_proc(0), debug(false), bootrom(0), bootromsz(0), dtb(0), dtbsz(0)
{
  fprintf(stderr, "sim_t::sim_t()\n");

  //signal(SIGINT, &handle_signal); // register this later

  // allocate target machine's memory, shrinking it as necessary
  // until the allocation succeeds
  size_t memsz0 = (size_t)mem_mb << 20;
  size_t quantum = 1L << 20;
  if (memsz0 == 0)
    memsz0 = 1L << (sizeof(size_t) == 8 ? 32 : 30);

  memsz = memsz0;
  if (0) {
    while ((mem = (char*)calloc(1, memsz)) == NULL)
      memsz = memsz*10/11/quantum*quantum;

    if (memsz != memsz0)
      fprintf(stderr, "warning: only got %lu bytes of target mem (wanted %lu)\n",
	      (unsigned long)memsz, (unsigned long)memsz0);
  } else {
    memfd = portalAlloc(memsz, 1);
    mem = (char *)portalMmap(memfd, memsz);
    if (mem == MAP_FAILED)
      fprintf(stderr, "warning: only got %lu bytes of target mem (wanted %lu)\n",
	      (unsigned long)memsz, (unsigned long)memsz0);
  }

  debug_mmu = new mmu_t(mem, memsz);

  // [sizhuo] create HTIF
  htif.reset(new htif_isasim_t(this, args));

  for (size_t i = 0; i < procs.size(); i++) {
    procs[i] = new processor_t(isa, this, i);
	// [sizhuo] manually reset processors (we don't reset by waiting for write on MRESET)
	procs[i]->reset(false);
  }

  int fd = open("bootrom.bin", O_RDONLY);
  if (fd > 0) {
    struct stat statbuf;
    int status = fstat(fd, &statbuf);
    fprintf(stderr, "fstat status %d size %ld\n", status, statbuf.st_size);
    if (status == 0) {
      bootrom = (const char *)mmap(0, statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
      bootromsz = statbuf.st_size;
      fprintf(stderr, "mapped bootrom at %p (%ld bytes) physaddr %lx\n", bootrom, bootromsz, memsz);
    }
    close(fd);
  } else {
    fprintf(stderr, "Could not open bootrom.bin\n");
  }

  fd = open("devicetree.dtb", O_RDONLY);
  if (fd > 0) {
    struct stat statbuf;
    int status = fstat(fd, &statbuf);
    fprintf(stderr, "fstat status %d size %ld\n", status, statbuf.st_size);
    if (status == 0) {
      dtb = (const char *)mmap(0, statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
      dtbsz = statbuf.st_size;
      fprintf(stderr, "mapped dtb at %p (%ld bytes) physaddr %lx\n", dtb, dtbsz, memsz);
    }
    close(fd);
  } else {
    fprintf(stderr, "Could not open dtb.bin\n");
  }

  spikeHw = new SpikeHw();
  spikeHw->setupDma(memfd);
  spikeHw->status();

  // [sizhuo] register enq fromhost FIFOs (must be done after procs created)
  // and start HTIF by loading programs etc.
  htif->register_enq_fromhost();
  htif->start();

  fprintf(stderr, ">> INFO: spike: HTIF started\n");

  // [sizhuo] register handler after htif is created, 
  // override handler registered by fesvr/htif_t
  signal(SIGINT, &handle_signal);
}

sim_t::~sim_t()
{
  for (size_t i = 0; i < procs.size(); i++)
    delete procs[i];
  delete debug_mmu;
  free(mem);
}

void sim_t::send_ipi(reg_t who)
{
  if (who < procs.size())
    procs[who]->deliver_ipi();
}

reg_t sim_t::get_scr(int which)
{
  switch (which)
  {
    case 0: return procs.size();
    case 1: return memsz >> 20;
    default: return -1;
  }
}

int sim_t::run()
{
  if (!debug && log)
    set_procs_debug(true);

  while (!htif->done()) //(htif->tick()) // [sizhuo] use done() instead
  {
    if (debug || ctrlc_pressed)
      interactive();
    else 
      step(INTERLEAVE);
  }
  return htif->exit_code();
}

void sim_t::step(size_t n)
{
  /*
  for (size_t i = 0, steps = 0; i < n; i += steps)
  {
    steps = std::min(n - i, INTERLEAVE - current_step);
    procs[current_proc]->step(steps);

    current_step += steps;
    if (current_step == INTERLEAVE)
    {
      current_step = 0;
      // procs[current_proc]->yield_load_reservation();
      if (++current_proc == procs.size()) {
        current_proc = 0;
        rtc += INTERLEAVE / INSNS_PER_RTC_TICK;
      }

      htif->tick();
    }
  }
  */
  // [sizhuo] use a simpler way, tick HTIF after every step
  for(size_t i = 0; i < n; i++) {
    procs[current_proc]->step(1);
    htif->host_tick(current_proc);
	htif->device_tick(); // [sizhuo] feed bcd with stdin if it needs
    htif->target_tick(current_proc);

    current_step++;
    if (current_step == INTERLEAVE) {
      current_step = 0;
      if (++current_proc == procs.size()) {
        current_proc = 0;
        rtc += INTERLEAVE / INSNS_PER_RTC_TICK;
      }
	}
  }
}

void sim_t::single_step_no_stdin()
{
  // [sizhuo] use a simpler way, tick HTIF after every step
  procs[current_proc]->step(1);
  htif->host_tick(current_proc);
  // [sizhuo] no tick for device (bcd)
  // someone else should feed bcd with stdin and tick target
  htif->target_tick(current_proc);

  current_step++;
  if (current_step == INTERLEAVE) {
    current_step = 0;
    if (++current_proc == procs.size()) {
      current_proc = 0;
      rtc += INTERLEAVE / INSNS_PER_RTC_TICK;
    }
  }
}

bool sim_t::running()
{
  for (size_t i = 0; i < procs.size(); i++)
    if (procs[i]->running())
      return true;
  return false;
}

void sim_t::stop()
{
  //procs[0]->state.tohost = 1;
  //while (htif->tick())
  //  ;
  // [sizhuo] this function is actually never called
  procs[0]->set_csr(CSR_MTOHOST, 1);
}

void sim_t::set_debug(bool value)
{
  debug = value;
}

void sim_t::set_log(bool value)
{
  log = value;
}

void sim_t::set_histogram(bool value)
{
  histogram_enabled = value;
  for (size_t i = 0; i < procs.size(); i++) {
    procs[i]->set_histogram(histogram_enabled);
  }
}

void sim_t::set_procs_debug(bool value)
{
  for (size_t i=0; i< procs.size(); i++)
    procs[i]->set_debug(value);
}

bool sim_t::mmio_load(reg_t addr, size_t len, uint8_t* bytes)
{

  //fprintf(stderr, "mmio_load addr=%llx len=%ld memsz=%x\n", (long long)addr, len, memsz);
  if (bootrom && bootrom != MAP_FAILED) {
    if ((addr >= 0x0400000) && (addr < (0x04000000 + bootromsz)) && (bytes != 0)) {
      memcpy(bytes, bootrom + (addr - 0x04000000), len);
      return true;
    }
  }
  if (dtb && dtb != MAP_FAILED) {
    if ((addr >= 0x04100000) && (addr < (0x04100000 + dtbsz)) && (bytes != 0)) {
      memcpy(bytes, dtb + (addr - 0x04100000), len);
      return true;
    }
  }

  // devices
  if (addr >= 0x04200000 && addr < 0x04300000 && spikeHw) {
    reg_t offset = addr - 0x04200000 + 0x100000;
    bool unaligned = (offset & 3) || (len & 3);
    if (unaligned) {
      uint8_t tmp[2];
      fprintf(stderr, "%s:%d unaligned addr=%x len=%d\n", __FUNCTION__, __LINE__, addr, len);
      spikeHw->read(offset & ~1, tmp);
      bytes[0] = tmp[1];
    } else {
      for (size_t i = 0; i < len; i += 4) {
	spikeHw->read(offset + i, bytes + i);
      }
    }
    return true;
  }

  return false;
}

bool sim_t::mmio_store(reg_t addr, size_t len, const uint8_t* bytes)
{
  //fprintf(stderr, "mmio_store addr=%llx len=%ld memsz=%x\n", (long long)addr, len, memsz);
  if (bootrom && bootrom != MAP_FAILED) {
    if ((addr >= 0x04000000) && (addr < (0x04000000 + bootromsz)) && (bytes != 0)) {
      //memcpy(bytes, bootrom + (addr - 0x04000000), len);
      return true;
    }
  }
  if (dtb && dtb != MAP_FAILED) {
    if ((addr >= 0x04100000) && (addr < (0x04100000 + dtbsz)) && (bytes != 0)) {
      //memcpy(bytes, dtb + (addr - 0x04100000), len);
      return true;
    }
  }
  // devices
  if (addr >= 0x04200000 && addr < 0x04300000 && spikeHw) {
    reg_t offset = addr - 0x04200000 + 0x100000;
    if ((offset & 3) || (len & 3))
      fprintf(stderr, "%s:%d unaligned addr=%x len=%d\n", __FUNCTION__, __LINE__, addr, len);
    fprintf(stderr, "spikeHw write %08x %08x len=%d\n", offset, *(int *)bytes, len);
    for (size_t i = 0; i < len; i += 4) {
      spikeHw->write(offset + i, bytes + i);
    }
    return true;
  }

  return false;
}
