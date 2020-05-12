// 2019 Team AobaZero
// This source code is in the public domain.
#include "err.hpp"
#include "nnet-cpu.hpp"
#include "nnet-ocl.hpp"
#include "nnet-srv.hpp"
#include "param.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <cassert>
#include <cstdio>
using std::copy_n;
using std::fill_n;
using std::lock_guard;
using std::move;
using std::mt19937_64;
using std::mutex;
using std::string;
using std::thread;
using std::to_string;
using std::unique_lock;
using std::unique_ptr;
using ErrAux::die;
using uint = unsigned int;

SeqPRNService::SeqPRNService() noexcept {
  char fn[IOAux::maxlen_path + 256U];
  uint pid = OSI::get_pid();
  sprintf(fn, "%s.%07u", Param::name_seq_prn, pid);
  _mmap.open(fn, true, sizeof(uint64_t) * Param::len_seq_prn);
  uint64_t *p = static_cast<uint64_t *>(_mmap());
  mt19937_64 mt(7);
  for (uint u = 0; u < Param::len_seq_prn; ++u) p[u] = mt(); }

class Entry {
  unique_ptr<float []> _input;
  unique_ptr<uint []> _sizes_nnmove;
  unique_ptr<ushort []> _nnmoves;
  unique_ptr<float []> _probs;
  unique_ptr<float []> _values;
  unique_ptr<uint []> _ids;
  uint _ubatch, _size_batch, _wait_id;

public:
  explicit Entry(uint size_batch) noexcept :
  _input(new float [size_batch * NNAux::size_input]),
    _sizes_nnmove(new uint [size_batch]),
    _nnmoves(new ushort [size_batch * SAux::maxsize_moves]),
    _probs(new float [size_batch * SAux::maxsize_moves]),
    _values(new float [size_batch]), _ids(new uint [size_batch]),
    _ubatch(0), _size_batch(size_batch), _wait_id(0) {
    fill_n(_input.get(), size_batch * NNAux::size_input, 0.0f);
    fill_n(_sizes_nnmove.get(), size_batch, 0); }

  void add(const float *input, uint size_nnmove, const ushort *nnmoves,
	   uint id) noexcept {
    assert(_ubatch < _size_batch && input && nnmoves
	   && id < NNAux::maxnum_nipc);
    copy_n(input, NNAux::size_input, &(_input[_ubatch * NNAux::size_input]));
    copy_n(nnmoves, size_nnmove, &(_nnmoves[_ubatch * SAux::maxsize_moves]));
    _sizes_nnmove[_ubatch] = size_nnmove;
    _ids[_ubatch]          = id;
    _ubatch += 1U; }

  void push_ff(NNet &nnet) noexcept {
    assert(0 < _ubatch);
    _wait_id = nnet.push_ff(_ubatch, _input.get(), _sizes_nnmove.get(),
			    _nnmoves.get(), _probs.get(), _values.get()); }

  void wait_ff(NNet &nnet, OSI::Semaphore sem_ipc[NNAux::maxnum_nipc],
	       SharedIPC *pipc[NNAux::maxnum_nipc]) noexcept {
    nnet.wait_ff(_wait_id);
    for (uint u = 0; u < _ubatch; ++u) {
      uint id = _ids[u];
      assert(pipc[id] && sem_ipc[id].ok());
      pipc[id]->value = _values[u];
      copy_n(&(_probs[u * SAux::maxsize_moves]), _sizes_nnmove[u],
	     pipc[id]->probs);
      sem_ipc[id].inc(); }
    _ubatch = 0; }

  bool is_full() const noexcept { return _ubatch == _size_batch; }
  bool is_empty() const noexcept { return _ubatch == 0; } };

void NNetService::worker_push() noexcept {
  unique_ptr<Entry> pe;
  while (true) {
    unique_lock<mutex> lock(_m_entries);
    _cv_entries_push.wait(lock, [&]{
	if (_flag_quit) return true;
	if (_entries_push.empty()) return false;
	if (16U < _entries_wait.size()) return false;
	if (_entries_wait.size() < 1U) return true;
	if (_entries_push.front()->is_full()) return true;
	return false; });
    if (_flag_quit) return;
    pe = move(_entries_push.front());
    _entries_push.pop_front();
    lock.unlock();
    
    pe->push_ff(*_pnnet);

    lock.lock();
    _entries_wait.push_back(move(pe));
    lock.unlock();
    _cv_entries_wait.notify_one(); } }

void NNetService::worker_wait() noexcept {
  SharedIPC *pipc[NNAux::maxnum_nipc];
  for (uint u = 0; u < _nipc; ++u)
    pipc[u] = static_cast<SharedIPC *>(_mmap_ipc[u]());

  unique_ptr<Entry> pe;
  while (true) {
    unique_lock<mutex> lock(_m_entries);
    _cv_entries_wait.wait(lock, [&]{
	return (0 < _entries_wait.size() || _flag_quit); });
    if (_flag_quit) return;
    pe = move(_entries_wait.front());
    _entries_wait.pop_front();
    lock.unlock();

    pe->wait_ff(*_pnnet, _sem_ipc, pipc);

    lock.lock();
    _entries_pool.push_back(move(pe));
    lock.unlock();
    _cv_entries_push.notify_one(); } }

void NNetService::worker_srv() noexcept {
  SharedService *pservice = static_cast<SharedService *>(_mmap_service());
  SharedIPC *pipc[NNAux::maxnum_nipc];
  for (uint u = 0; u < _nipc; ++u)
    pipc[u] = static_cast<SharedIPC *>(_mmap_ipc[u]());

  uint id;
  SrvType type;
  while (true) {
    assert(_sem_service.ok() && _sem_service_lock.ok());
    _sem_service.dec_wait();
    _sem_service_lock.dec_wait();
    assert(0 < pservice->njob);
    pservice->njob -= 1U;
    id   = pservice->jobs[0].id;
    type = pservice->jobs[0].type;
    for (uint u = 0; u <  pservice->njob; u += 1U)
      pservice->jobs[u] = pservice->jobs[u + 1U];
    _sem_service_lock.inc();
    assert(id < _nipc || id == NNAux::maxnum_nipc);

    if (type == SrvType::End) {
      string s("Terminate (nnet_id: ");
      s += to_string(_nnet_id) + string(")");
      std::cerr << s << std::endl;
      return; }

    if (type == SrvType::Register) {
      assert(pipc[id] && pipc[id]->nnet_id == _nnet_id && _sem_ipc[id].ok());
      _sem_ipc[id].inc();
      string s("Register id: ");
      s += to_string(id) + string(" nnet_id: ") + to_string(_nnet_id);
      std::cerr << s << std::endl;
      continue; }

    if (type == SrvType::NNReset) {
      FName fn;
      lock_guard<mutex> lock_push(_m_entries);
      if (!_entries_push.empty()) die(ERR_INT("Internal Error"));
      if (!_entries_wait.empty()) die(ERR_INT("Internal Error"));
	
      _sem_service_lock.dec_wait();
      pservice->id_ipc_next = 0;
      uint njob = pservice->njob;
      _sem_service_lock.inc();
      if (0 < njob) die(ERR_INT("Internal Error"));

      {
	lock_guard<mutex> lock(_m_nnreset);
	_flag_cv_nnreset = true;
	fn = _fname; }
      _cv_nnreset.notify_one();
      
      uint version;
      uint64_t digest;
      NNAux::wght_t wght = NNAux::read(fn, version, digest);
      if (_impl == NNet::cpublas) {
#if defined(USE_OPENBLAS) || defined(USE_MKL)
	_pnnet.reset(new NNetCPU);
	dynamic_cast<NNetCPU *>(_pnnet.get())->reset(_size_batch, wght,
						     _thread_num);
#else
	die(ERR_INT("No CPU BLAS support"));
#endif
      } else if (_impl == NNet::opencl) {
#if defined(USE_OPENCL_AOBA)
	string s("Tuning feed-forward engine of device ");
	s += to_string(static_cast<int>(_device_id)) + string(" for ");
	s += string(fn.get_fname()) + string("\n");
	std::cout << s << std::flush;
	
	_pnnet.reset(new NNetOCL);
	NNetOCL *p = dynamic_cast<NNetOCL *>(_pnnet.get());
	std::cout << p->reset(_size_batch, wght, _device_id, _use_half,
			      false, true)
		  << std::flush;
#else
	die(ERR_INT("No OpenCL support"));
#endif
      } else die(ERR_INT("INTERNAL ERROR"));

      continue; }
      
    if (type == SrvType::FeedForward) {
      assert(pipc[id]);
      bool flag_do_notify = false;
      {
	lock_guard<mutex> lock(_m_entries);
	if (_entries_push.empty()) flag_do_notify = true;
	if (_entries_push.empty() || _entries_push.back()->is_full()) {
	  if (_entries_pool.empty())
	    _entries_pool.emplace_back(new Entry(_size_batch));

	  unique_ptr<Entry> p = move(_entries_pool.back());
	  _entries_pool.pop_back();
	  _entries_push.push_back(move(p)); }

	_entries_push.back()->add(pipc[id]->input, pipc[id]->size_nnmove,
				  pipc[id]->nnmoves, id);
	if (_entries_push.size() == 1 && _entries_push.back()->is_full())
	  flag_do_notify = true; }

      if (flag_do_notify) _cv_entries_push.notify_one();
      continue; }

    die(ERR_INT("INTERNAL ERROR")); } }

void NNetService::nnreset(const FName &fname) noexcept {
  assert(_mmap_service.ok() && fname.ok());
  auto p = static_cast<SharedService *>(_mmap_service());
  assert(p && _sem_service.ok() && _sem_service_lock.ok());

  unique_lock<mutex> lock(_m_nnreset);
  _fname = fname;
  _sem_service_lock.dec_wait();
  assert(p->njob <= NNAux::maxnum_nipc);
  p->jobs[p->njob].id   = NNAux::maxnum_nipc;
  p->jobs[p->njob].type = SrvType::NNReset;
  p->njob += 1U;
  _sem_service_lock.inc();
  _sem_service.inc();

  _cv_nnreset.wait(lock, [&]{ return _flag_cv_nnreset; });
  _flag_cv_nnreset = false;
  lock.unlock(); }

NNetService::NNetService(NNet::Impl impl, uint nnet_id, uint nipc,
			 uint size_batch, uint device_id, uint use_half,
			 uint thread_num) noexcept
: _impl(impl), _flag_cv_nnreset(false), _flag_quit(false), _nnet_id(nnet_id),
  _nipc(nipc), _size_batch(size_batch), _device_id(device_id),
  _use_half(use_half), _thread_num(thread_num) {
  if (NNAux::maxnum_nnet <= nnet_id) die(ERR_INT("too many nnets"));
  if (NNAux::maxnum_nipc < nipc)     die(ERR_INT("too many processes"));

  char fn[IOAux::maxlen_path + 256U];
  uint pid = OSI::get_pid();
  sprintf(fn, "%s.%07u.%03u", Param::name_sem_lock_nnet, pid, nnet_id);
  _sem_service_lock.open(fn, true, 1U);
  sprintf(fn, "%s.%07u.%03u", Param::name_sem_nnet, pid, nnet_id);
  _sem_service.open(fn, true, 0U);
  sprintf(fn, "%s.%07u.%03u", Param::name_mmap_nnet, pid, nnet_id);
  _mmap_service.open(fn, true, sizeof(SharedService));
  auto p = static_cast<SharedService *>(_mmap_service());
  assert(p);
  p->id_ipc_next = 0;
  p->njob = 0;
  for (uint u = 0; u < nipc; ++u) {
    sprintf(fn, "%s.%07u.%03u.%03u", Param::name_sem_nnet, pid, nnet_id, u);
    _sem_ipc[u].open(fn, true, 0);
    sprintf(fn, "%s.%07u.%03u.%03u", Param::name_mmap_nnet, pid, nnet_id, u);
    _mmap_ipc[u].open(fn, true, sizeof(SharedIPC)); }

  _th_worker_srv  = thread(&NNetService::worker_srv,  this);
  _th_worker_push = thread(&NNetService::worker_push, this);
  _th_worker_wait = thread(&NNetService::worker_wait, this); }

NNetService::NNetService(NNet::Impl impl, uint nnet_id, uint nipc,
			 uint size_batch, uint device_id, uint use_half,
			 uint thread_num, const FName &fname) noexcept
: NNetService(impl, nnet_id, nipc, size_batch, device_id, use_half,
	      thread_num) { nnreset(fname); }

NNetService::~NNetService() noexcept {
  assert(_mmap_service.ok());
  auto p = static_cast<SharedService *>(_mmap_service());
  assert(p && _sem_service.ok() && _sem_service_lock.ok());
  _sem_service_lock.dec_wait();
  assert(p->njob <= NNAux::maxnum_nipc);
  p->jobs[p->njob].id   = NNAux::maxnum_nipc;
  p->jobs[p->njob].type = SrvType::End;
  p->njob += 1U;
  _sem_service_lock.inc();
  _sem_service.inc();

  {
    lock_guard<mutex> lock(_m_entries);
    _flag_quit = true; }
  _cv_entries_wait.notify_one();
  _cv_entries_push.notify_one();

  _th_worker_srv.join();
  _th_worker_push.join();
  _th_worker_wait.join();

  _sem_service_lock.close();
  _sem_service.close();
  _mmap_service.close();

  for (uint u = 0; u < _nipc; ++u) {
    _sem_ipc[u].inc();
    _sem_ipc[u].close();
    _mmap_ipc[u].close(); } }
