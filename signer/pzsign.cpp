// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Leonardo Boulay
// pzsign - the two-party ECDSA signer of the shared kangaroo network (docs/en/NETWORK.md 7.6), on the two-party ECDSA of cb-mpc
// (signer/vendor). pzpeer starts one `pzsign session` per signing and relays its messages to the partner's pzsign; signer/README.md
// has the frames, the job and the steps. stdout carries only frames; everything for people goes to stderr.
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

#include <cbmpc/internal/core/strext.h>
#include <cbmpc/internal/crypto/base.h>
#include <cbmpc/internal/crypto/base_ecc.h>
#include <cbmpc/internal/protocol/ec_dkg.h>
#include <cbmpc/internal/protocol/ecdsa_2p.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

using namespace coinbase;
using coinbase::crypto::bn_t;
using coinbase::crypto::ecc_point_t;
using bytes = std::vector<uint8_t>;
namespace ecdsa2pc = coinbase::mpc::ecdsa2pc;

enum : uint8_t { F_JOB = 0x01, F_FROM_PARTNER = 0x02, F_TO_PARTNER = 0x81, F_RESULT = 0x82, F_ERROR = 0x83 };   // the frames
enum : uint8_t { VERSION = 2, IMPORT = 1, SIGN = 2, SIGS = 3 };   // the version of the job and of the envelopes; the phases
const size_t MAX_FRAME = 1 << 20, MAX_DIGESTS = 64, JOB_FIXED = 166;

struct fail {
  std::string why;
};

static const crypto::ecurve_t& curve() { return crypto::curve_secp256k1; }

static double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// ---- frames: a 4-byte big-endian length L (1 to MAX_FRAME), then L bytes, the type and the payload ----

static FILE* frames_out;  // stdout as it was at the start: stdout itself then leads to stderr (session)

static std::string read_frame(uint8_t& type, bytes& payload) {
  uint8_t h[5];
  if (fread(h, 1, 5, stdin) != 5) return "stdin closed";
  const uint32_t len = uint32_t(h[0]) << 24 | uint32_t(h[1]) << 16 | uint32_t(h[2]) << 8 | h[3];
  if (len == 0 || len > MAX_FRAME) return "frame of " + std::to_string(len) + " bytes on stdin";
  type = h[4];
  payload.resize(len - 1);
  return fread(payload.data(), 1, payload.size(), stdin) == payload.size() ? "" : "stdin closed";
}

static bool write_frame(uint8_t type, const uint8_t* p, size_t n) {
  const size_t len = n + 1;
  const uint8_t h[5] = {uint8_t(len >> 24), uint8_t(len >> 16), uint8_t(len >> 8), uint8_t(len), type};
  return len <= MAX_FRAME && fwrite(h, 1, 5, frames_out) == 5 && fwrite(p, 1, n, frames_out) == n && fflush(frames_out) == 0;
}

// ---- the channel to the partner: envelopes of three bytes (version, sender, phase) and the library's message ----

struct link_t : coinbase::api::data_transport_i {
  int me;
  uint8_t phase = IMPORT;
  size_t digests = 1;
  std::string why;  // the first failure of the channel or of the partner's messages: the library only sees an error code
  explicit link_t(int me) : me(me) {}
  virtual bool put(bytes&& envelope) = 0;
  virtual std::string get(bytes& envelope) = 0;

  error_t send(coinbase::api::party_idx_t, mem_t msg) override {
    bytes e = {VERSION, uint8_t(me), phase};
    e.insert(e.end(), msg.data, msg.data + msg.size);
    if (put(std::move(e))) return SUCCESS;
    if (why.empty()) why = "the message for the partner could not be written";
    return E_GENERAL;
  }
  error_t receive(coinbase::api::party_idx_t, buf_t& msg) override {
    bytes e;
    std::string w = get(e);
    if (w.empty() && (e.size() < 3 || e[0] != VERSION || e[1] != 1 - me || e[2] != phase))
      w = "malformed message from the partner (" + strext::to_hex(mem_t(e.data(), int(e.size() < 3 ? e.size() : 3))) +
          ", phase " + std::to_string(phase) + " expected)";
    // the largest a message of the phase can be (36 KB, then about 4 KB a digest): the library sizes its arrays by what a message says,
    // and a partner would make it allocate hundreds of MB with a message of 1 MiB
    const size_t cap = phase == IMPORT ? 65536 : phase == SIGN ? 16384 + 4608 * digests : 64 * digests;
    if (w.empty() && e.size() - 3 > cap) w = "a message of " + std::to_string(e.size()) + " bytes from the partner";
    if (!w.empty()) {
      if (why.empty()) why = w;
      return E_FORMAT;
    }
    msg = buf_t(e.data() + 3, int(e.size() - 3));
    return SUCCESS;
  }
  error_t receive_all(const std::vector<coinbase::api::party_idx_t>& from, std::vector<buf_t>& msgs) override {
    msgs.resize(from.size());
    for (size_t k = 0; k < from.size(); k++)
      if (error_t rv = receive(from[k], msgs[k])) return rv;
    return SUCCESS;
  }
};

struct stdio_link : link_t {
  using link_t::link_t;
  bool put(bytes&& e) override { return write_frame(F_TO_PARTNER, e.data(), e.size()); }
  std::string get(bytes& e) override {
    uint8_t type = 0;
    const std::string w = read_frame(type, e);
    return !w.empty() || type == F_FROM_PARTNER ? w : "frame of type " + strext::to_hex(type) + " during the session";
  }
};

// ---- the job: [VERSION][i][first][eid 32][share 32][pub0 33][pub1 33][key 33][digest 32]... (1 to MAX_DIGESTS digests) ----

struct job_t {
  int i, first;  // this party; the party that gets the signatures first (P1 of the protocol, which gives them to the other)
  bytes eid;
  bn_t share;
  ecc_point_t pub[2], key;
  std::vector<bytes> digests;
};

static ecc_point_t point(const uint8_t* p, const char* what) {
  ecc_point_t P;
  if (P.from_bin(curve(), mem_t(p, 33)) || P.is_infinity()) throw fail{std::string("job: ") + what + " is not a point of the curve"};
  return P;
}

// Parses the job and wipes its share from the payload, whatever happens.
static job_t parse_job(bytes& p) {
  job_t j;
  const size_t n = p.size() >= JOB_FIXED ? (p.size() - JOB_FIXED) / 32 : 0;
  const bool ok = p.size() >= JOB_FIXED && p[0] == VERSION && p[1] <= 1 && p[2] <= 1 && (p.size() - JOB_FIXED) % 32 == 0 && n >= 1 &&
                  n <= MAX_DIGESTS;
  if (ok) j.share = bn_t::from_bin(mem_t(&p[35], 32));
  if (p.size() >= 67) OPENSSL_cleanse(&p[35], 32);
  if (!ok) throw fail{"malformed job (" + std::to_string(p.size()) + " bytes)"};
  if (j.share <= 0 || j.share >= curve().order().value()) throw fail{"job: the share is not a scalar in [1, n-1]"};
  j.i = p[1], j.first = p[2];
  j.eid.assign(&p[3], &p[35]);
  j.pub[0] = point(&p[67], "pub[0]"), j.pub[1] = point(&p[100], "pub[1]"), j.key = point(&p[133], "key");
  for (size_t k = 0; k < n; k++) j.digests.emplace_back(&p[JOB_FIXED + 32 * k], &p[JOB_FIXED + 32 * k + 32]);
  return j;
}

// ---- one party ----

// The key generation of the library (ecdsa2pc::dkg) with the party's own piece instead of a random one: the commitments, the proofs of
// knowledge of both pieces and the Paillier encryption of P1's piece with its proofs, all as in dkg; the key must come out as `expected`.
static error_t import_piece(mpc::job_2p_t& job, const bn_t& piece, const ecc_point_t& expected, ecdsa2pc::key_t& key) {
  error_t rv = UNINITIALIZED_ERROR;
  key.curve = curve();
  key.role = job.get_party();
  key.x_share = piece;
  ecdsa2pc::paillier_gen_interactive_t paillier_gen(job.get_pid(mpc::party_t::p1));
  mpc::eckey::dkg_2p_t ec_dkg(curve(), job.get_pid(mpc::party_t::p1));
  if (job.is_p1()) {
    ec_dkg.step1_p1_to_p2(key.x_share);
    paillier_gen.step1_p1_to_p2(key.paillier, key.x_share, curve().order(), key.c_key);
  }
  if (rv = job.p1_to_p2(ec_dkg.msg1, paillier_gen.msg1, key.c_key)) return rv;
  if (paillier_gen.c_key != key.c_key) return E_CRYPTO;
  if (job.is_p2()) {
    ec_dkg.step2_p2_to_p1(key.x_share);
    paillier_gen.step2_p2_to_p1();
  }
  if (rv = job.p2_to_p1(ec_dkg.msg2, paillier_gen.msg2)) return rv;
  if (job.is_p1()) {
    if (rv = ec_dkg.step3_p1_to_p2(key.Q)) return rv;
    paillier_gen.step3_p1_to_p2(key.paillier, key.x_share, ec_dkg.Q1, job.get_pid(mpc::party_t::p1), ec_dkg.sid);
  }
  if (rv = job.p1_to_p2(ec_dkg.msg3, paillier_gen.msg3)) return rv;
  if (job.is_p2()) {
    if (rv = ec_dkg.step4_output_p2(key.Q)) return rv;
    if (rv = paillier_gen.step4_p2_output(key.paillier, ec_dkg.Q1, paillier_gen.c_key, job.get_pid(mpc::party_t::p1), ec_dkg.sid))
      return rv;
  }
  if (key.Q != expected) return E_CRYPTO;
  return SUCCESS;
}

static void check(error_t rv, const link_t& l, const char* what) {
  if (rv) throw fail{l.why.empty() ? std::string(what) + " failed (error " + std::to_string(rv) + ")" : l.why};
}

// Imports the pieces, signs every digest in one run, and returns r || s (big-endian, s low) for each: P1 gets them from the protocol
// and gives them to P2, and each side verifies every signature against the key before it reports it.
static bytes run(const job_t& j, link_t& l) {
  const auto& G = curve().generator();
  if (j.share * G != j.pub[j.i]) throw fail{"the share does not match its public piece"};
  if (j.pub[0] + j.pub[1] != j.key) throw fail{"the public pieces do not add up to the key"};
  const std::string id = "pznet1/" + strext::to_hex(mem_t(j.eid.data(), 32));
  mpc::job_2p_t job(j.i == j.first ? mpc::party_t::p1 : mpc::party_t::p2, id + "/p1", id + "/p2", l);
  const auto t0 = std::chrono::steady_clock::now();

  ecdsa2pc::key_t key;
  l.phase = IMPORT, l.digests = j.digests.size();
  check(import_piece(job, j.share, j.key, key), l, "the import of the pieces");
  // ECDSA uses the digest only as a number mod n, and the library takes it already reduced (in its proofs and in its verification): the
  // same signatures as for the digests as they came
  std::vector<buf_t> reduced;
  std::vector<mem_t> msgs;
  const bn_t& n = curve().order().value();
  for (const bytes& d : j.digests) {
    const bn_t e = bn_t::from_bin(mem_t(d.data(), 32));  // below 2^256, so below 2n
    reduced.push_back((e >= n ? e - n : e).to_bin(32));
  }
  for (const buf_t& e : reduced) msgs.emplace_back(e);
  std::vector<buf_t> der;
  buf_t sid;
  l.phase = SIGN;
  check(ecdsa2pc::sign_batch(job, sid, key, msgs, der), l, "the signing");

  bytes rs;
  l.phase = SIGS;
  if (job.is_p1()) {
    for (const buf_t& d : der) {
      crypto::ecdsa_signature_t sig;
      if (sig.from_der(curve(), d)) throw fail{"the library gave a signature that does not decode"};
      const buf_t r = sig.get_r().to_bin(32), s = sig.get_s().to_bin(32);
      rs.insert(rs.end(), r.data(), r.data() + 32), rs.insert(rs.end(), s.data(), s.data() + 32);
    }
    check(l.send(1 - j.i, mem_t(rs.data(), int(rs.size()))), l, "sending the signatures");
  } else {
    buf_t got;
    check(l.receive(1 - j.i, got), l, "receiving the signatures");
    if (size_t(got.size()) != 64 * msgs.size()) throw fail{"the partner gave " + std::to_string(got.size()) + " bytes of signatures"};
    rs.assign(got.data(), got.data() + got.size());
  }
  // the check that counts: a signature that does not verify, or whose s is not low, never leaves
  const bn_t half = n >> 1;
  for (size_t k = 0; k < msgs.size(); k++) {
    const bn_t r = bn_t::from_bin(mem_t(&rs[64 * k], 32)), s = bn_t::from_bin(mem_t(&rs[64 * k + 32], 32));
    if (r <= 0 || r >= n || s <= 0 || s > half ||
        crypto::ecc_pub_key_t(j.key).verify(msgs[k], crypto::ecdsa_signature_t(curve(), r, s).to_der()))
      throw fail{"signature " + std::to_string(k) + " does not verify"};
  }
  fprintf(stderr, "pzsign[%d]: %zu signatures in %.2f s\n", j.i, msgs.size(), seconds_since(t0));
  return rs;
}

// ---- the commands ----

// No core file, and on Linux no other process of the user may read this one: it holds a piece of a key.
static void harden() {
#ifndef _WIN32
  const struct rlimit none = {0, 0};
  setrlimit(RLIMIT_CORE, &none);
#ifdef __linux__
  prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#endif
  // a standard descriptor that is closed is opened on the null device: else the copy of stdout below could take number 2, and the log
  // lines would go into the frames
  for (int fd = 0; fd <= 2; fd++)
    if (fcntl(fd, F_GETFD) < 0) {
      const int n = open("/dev/null", O_RDWR);
      if (n >= 0 && n != fd) dup2(n, fd), close(n);
    }
#endif
}

static int session() {
  harden();
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
#endif
  setvbuf(stdin, nullptr, _IONBF, 0);  // the job (and its piece) only in the vector that is wiped, not in a buffer of stdio
  // frames go to a copy of stdout, and stdout itself to stderr: nothing the library might print can break a frame
  frames_out = fdopen(dup(1), "wb");
  if (!frames_out || dup2(2, 1) < 0) return fputs("pzsign: stdout\n", stderr), 1;
#ifdef _WIN32
  _setmode(_fileno(frames_out), _O_BINARY);
#endif
  int me = -1;
  try {
    uint8_t type = 0;
    bytes payload;
    if (std::string w = read_frame(type, payload); !w.empty()) throw fail{w};
    if (type != F_JOB) throw fail{"first frame of type " + strext::to_hex(type) + ", not a job"};
    const job_t job = parse_job(payload);
    me = job.i;
    stdio_link link(me);
    const bytes rs = run(job, link);
    return write_frame(F_RESULT, rs.data(), rs.size()) ? 0 : 1;
  } catch (const fail& f) {
    fprintf(stderr, "pzsign[%d]: %s\n", me, f.why.c_str());
    write_frame(F_ERROR, (const uint8_t*)f.why.data(), f.why.size());
  } catch (const std::exception& e) {  // an assertion of the library: the session still ends with an error frame
    const std::string why = std::string("internal error: ") + e.what();
    fprintf(stderr, "pzsign[%d]: %s\n", me, why.c_str());
    write_frame(F_ERROR, (const uint8_t*)why.data(), why.size());
  }
  return 1;
}

// The selftest's link: the partner is a thread of the same process.
struct queues_t {
  std::mutex m;
  std::condition_variable cv;
  std::deque<bytes> q[2];
};

struct queue_link : link_t {
  queues_t& qs;
  queue_link(int me, queues_t& qs) : link_t(me), qs(qs) {}
  bool put(bytes&& e) override {
    std::lock_guard<std::mutex> lock(qs.m);
    qs.q[1 - me].push_back(std::move(e));
    qs.cv.notify_all();
    return true;
  }
  std::string get(bytes& e) override {
    std::unique_lock<std::mutex> lock(qs.m);
    if (!qs.cv.wait_for(lock, std::chrono::seconds(60), [&] { return !qs.q[me].empty(); })) return "the partner is silent";
    e = std::move(qs.q[me].front());
    qs.q[me].pop_front();
    return "";
  }
};

static void put_point(bytes& out, const ecc_point_t& P) {
  const buf_t b = P.to_compressed_bin();
  out.insert(out.end(), b.data(), b.data() + b.size());
}

// Both parties in one process, once with each of them first: a random key split in two pieces, three digests (the last one above the
// group order, to cover its reduction), and the same verified signatures on both sides.
static int selftest() {
  const mod_t& q = curve().order();
  const auto& G = curve().generator();
  bn_t x[2], k;
  x[0] = bn_t::rand(q), x[1] = bn_t::rand(q);
  MODULO(q) k = x[0] + x[1];
  bytes eid(32), digests(96, 0xff);
  RAND_bytes(eid.data(), 32), RAND_bytes(digests.data(), 64);
  for (int first = 0; first < 2; first++) {
    job_t jobs[2];
    for (int i = 0; i < 2; i++) {
      bytes p = {VERSION, uint8_t(i), uint8_t(first)};
      p.insert(p.end(), eid.begin(), eid.end());
      const buf_t s = x[i].to_bin(32);
      p.insert(p.end(), s.data(), s.data() + 32);
      put_point(p, x[0] * G), put_point(p, x[1] * G), put_point(p, k * G);
      p.insert(p.end(), digests.begin(), digests.end());
      jobs[i] = parse_job(p);
    }
    queues_t qs;
    bytes rs[2];
    std::string why[2];
    const auto t0 = std::chrono::steady_clock::now();
    auto party = [&](int i) {
      queue_link link(i, qs);
      try {
        rs[i] = run(jobs[i], link);
      } catch (const fail& f) {
        why[i] = f.why;
      } catch (const std::exception& e) {
        why[i] = std::string("internal error: ") + e.what();
      }
    };
    std::thread other(party, 1);
    party(0);
    other.join();
    if (!why[0].empty() || !why[1].empty() || rs[0] != rs[1] || rs[0].size() != digests.size() * 2) {
      fprintf(stderr, "pzsign: selftest (party %d first): %s / %s\n", first, why[0].c_str(), why[1].c_str());
      return 1;
    }
    printf("party %d first: 3 signatures, the same on both sides, verified, in %.2f s\n", first, seconds_since(t0));
  }
  printf("selftest OK\n");
  return 0;
}

int main(int argc, char** argv) {
  const std::string cmd = argc == 2 ? argv[1] : "";
  try {
    if (cmd == "session") return session();
    if (cmd == "selftest") return selftest();
  } catch (const std::exception& e) {
    fprintf(stderr, "pzsign: internal error: %s\n", e.what());
    return 1;
  }
  if (cmd == "version") {
    printf("pzsign %s\n", PZ_VERSION);
    printf("Copyright (C) 2026 Leonardo Boulay\n");
    printf("License AGPL-3.0-or-later: GNU Affero General Public License version 3 or later (LICENSE, NOTICE).\n");
    printf("This is free software: you are free to change and redistribute it.\n");
    printf("There is NO WARRANTY, to the extent permitted by law.\n");
    return 0;
  }
  fputs("usage: pzsign session     a signing session, framed on stdin and stdout (signer/README.md)\n"
        "       pzsign selftest    both parties in one process\n"
        "       pzsign version\n",
        stderr);
  return 2;
}
