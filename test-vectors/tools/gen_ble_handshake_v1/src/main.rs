//! Independent generator of `test-vectors/ble_handshake_v1.txt` (M1.10e, decision E7).
//!
//! Test tooling only. The Noise implementation is the Rust crate `snow` (independent of noise-c,
//! which the firmware and the Android app use); the PSK, prologue and `device_id_hash` use the
//! RustCrypto `hkdf`/`sha2` crates; frame encryption with the header as associated data uses the
//! RustCrypto `chacha20poly1305` AEAD with the split keys from `snow`. No primitive is implemented
//! here. Deterministic ephemeral keys (`fixed_ephemeral_key_for_testing_only`) exist only in this
//! tool.
//!
//! Before writing anything the tool proves itself:
//!   1. the official Noise vectors for Noise_XXpsk3_25519_ChaChaPoly_SHA256 and
//!      Noise_IK_25519_ChaChaPoly_SHA256 (noise-c's bundled cacophony.txt and noise-c-basic.txt)
//!      reproduce byte for byte through `snow` (0 skipped);
//!   2. the AEAD-with-AD path equals `snow`'s own transport encryption for an empty AD;
//!   3. every psk / psk_not / prologue / idhash record of ble_session_v1.txt (Python standard
//!      library reference) reproduces.
//! Any mismatch exits non-zero without output.
//!
//! Usage: gen_ble_handshake_v1 <noise-c tests/vector dir> <ble_session_v1.txt> > ble_handshake_v1.txt

use chacha20poly1305::aead::{Aead, KeyInit, Payload};
use chacha20poly1305::ChaCha20Poly1305;
use hkdf::Hkdf;
use sha2::{Digest, Sha256};
use snow::params::{DHChoice, NoiseParams};
use snow::resolvers::{CryptoResolver, DefaultResolver};
use snow::{Builder, HandshakeState};
use std::collections::HashMap;
use std::fmt::Write as _;
use std::process::exit;

const XX: &str = "Noise_XXpsk3_25519_ChaChaPoly_SHA256";
const IK: &str = "Noise_IK_25519_ChaChaPoly_SHA256";

// Protocol constants (docs/BLE_PROTOCOL.md sections 4, 5, 11).
const T_HS: u8 = 0x02;
const T_ENROLL_RESULT: u8 = 0x08;
const T_INVITE_CREATE: u8 = 0x72;
const T_END_SESSION: u8 = 0x7E;
const T_EV_CONFIRM_ON_DEVICE: u8 = 0x83;
const F_RESPONSE: u8 = 0x01;
const F_EVENT: u8 = 0x04;
const F_ENCRYPTED: u8 = 0x08;
const PURPOSE_SESSION: u8 = 1;
const PURPOSE_CLAIM: u8 = 2;
const PURPOSE_ADD: u8 = 3;
const PURPOSE_RECOVERY: u8 = 4;

fn die(msg: &str) -> ! {
    eprintln!("gen_ble_handshake_v1: {msg}");
    exit(1);
}

fn hex(b: &[u8]) -> String {
    let mut s = String::with_capacity(b.len() * 2);
    for x in b {
        let _ = write!(s, "{x:02x}");
    }
    s
}

fn xhex(b: &[u8]) -> String {
    if b.is_empty() {
        "-".to_string()
    } else {
        format!("x:{}", hex(b))
    }
}

fn unhex(s: &str) -> Vec<u8> {
    let s = s.strip_prefix("x:").unwrap_or(s);
    if s == "-" {
        return Vec::new();
    }
    if s.len() % 2 != 0 {
        die("odd hex length");
    }
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap_or_else(|_| die("bad hex")))
        .collect()
}

/// Visibly synthetic 32-byte pattern (never a real key).
fn pattern32(base: u8) -> [u8; 32] {
    let mut k = [0u8; 32];
    for (i, b) in k.iter_mut().enumerate() {
        *b = base.wrapping_add(i as u8);
    }
    k
}

fn x25519_pub(private: &[u8; 32]) -> [u8; 32] {
    let mut dh = DefaultResolver
        .resolve_dh(&DHChoice::Curve25519)
        .unwrap_or_else(|| die("no Curve25519 in snow's resolver"));
    dh.set(private);
    dh.pubkey().try_into().unwrap()
}

fn params(name: &str) -> NoiseParams {
    name.parse().unwrap_or_else(|_| die("snow does not parse the protocol name"))
}

// ---- Derivations (docs/BLE_PROTOCOL.md 11.1) --------------------------------------------------

fn psk(purpose: u8, setup: &[u8], token: &[u8]) -> [u8; 32] {
    let mut info = vec![purpose];
    info.extend_from_slice(token);
    let hk = Hkdf::<Sha256>::new(Some(b"DSP2 psk"), setup);
    let mut out = [0u8; 32];
    hk.expand(&info, &mut out).unwrap_or_else(|_| die("hkdf"));
    out
}

fn prologue(major: u8, minor: u8, purpose: u8, hash8: &[u8], session_id: u32) -> Vec<u8> {
    let mut p = b"DSP2 BLE v1".to_vec();
    p.extend_from_slice(&[major, minor, purpose]);
    p.extend_from_slice(hash8);
    p.extend_from_slice(&session_id.to_le_bytes());
    p
}

fn idhash(label: &[u8], data: &[u8]) -> Vec<u8> {
    let mut h = Sha256::new();
    h.update(label);
    h.update(data);
    h.finalize()[..8].to_vec()
}

// ---- Frames (docs/BLE_PROTOCOL.md section 4) --------------------------------------------------

fn header(t: u8, flags: u8, txn: u16, len: usize) -> [u8; 8] {
    let l = u16::try_from(len).unwrap_or_else(|_| die("frame too long"));
    let tx = txn.to_le_bytes();
    let lb = l.to_le_bytes();
    [1, 0, t, flags, tx[0], tx[1], lb[0], lb[1]]
}

fn plain_frame(t: u8, flags: u8, txn: u16, body: &[u8]) -> Vec<u8> {
    let mut f = header(t, flags, txn, body.len()).to_vec();
    f.extend_from_slice(body);
    if f.len() > 520 {
        die("frame above 520 bytes");
    }
    f
}

fn hs_request(purpose: u8, idx: u8, txn: u16, noise: &[u8]) -> Vec<u8> {
    let mut b = vec![purpose, idx];
    b.extend_from_slice(noise);
    plain_frame(T_HS, 0, txn, &b)
}

fn hs_response(idx: u8, txn: u16, noise: &[u8]) -> Vec<u8> {
    let mut b = vec![idx];
    b.extend_from_slice(noise);
    plain_frame(T_HS, F_RESPONSE, txn, &b)
}

/// Noise ChaChaPoly nonce: 32 zero bits followed by the 64-bit little-endian counter.
fn nonce(n: u64) -> [u8; 12] {
    let mut iv = [0u8; 12];
    iv[4..].copy_from_slice(&n.to_le_bytes());
    iv
}

fn aead_encrypt(key: &[u8; 32], n: u64, ad: &[u8], plain: &[u8]) -> Vec<u8> {
    ChaCha20Poly1305::new(key.into())
        .encrypt(&nonce(n).into(), Payload { msg: plain, aad: ad })
        .unwrap_or_else(|_| die("aead encrypt"))
}

fn aead_decrypt(key: &[u8; 32], n: u64, ad: &[u8], ct: &[u8]) -> Option<Vec<u8>> {
    ChaCha20Poly1305::new(key.into()).decrypt(&nonce(n).into(), Payload { msg: ct, aad: ad }).ok()
}

/// Encrypted frame: the header (with ENCRYPTED and len = plaintext + 16) is the associated data.
fn enc_frame(key: &[u8; 32], n: u64, t: u8, flags: u8, txn: u16, plain: &[u8]) -> (Vec<u8>, Vec<u8>) {
    let h = header(t, flags | F_ENCRYPTED, txn, plain.len() + 16);
    let ct = aead_encrypt(key, n, &h, plain);
    let mut f = h.to_vec();
    f.extend_from_slice(&ct);
    (h.to_vec(), f)
}

// ---- Self-checks -------------------------------------------------------------------------------

fn field(v: &serde_json::Value, k: &str) -> Vec<u8> {
    unhex(v[k].as_str().unwrap_or_else(|| die(&format!("vector field {k} missing"))))
}

fn psks(v: &serde_json::Value, k: &str) -> Vec<[u8; 32]> {
    v[k].as_array()
        .map(|a| a.iter().map(|p| unhex(p.as_str().unwrap()).try_into().unwrap()).collect())
        .unwrap_or_default()
}

fn check_official_vectors(dir: &str) -> (usize, usize) {
    let (mut xx, mut ik) = (0, 0);
    for file in ["cacophony.txt", "noise-c-basic.txt"] {
        let path = format!("{dir}/{file}");
        let text = std::fs::read_to_string(&path).unwrap_or_else(|_| die(&format!("cannot read {path}")));
        let json: serde_json::Value = serde_json::from_str(&text).unwrap_or_else(|_| die("bad vector JSON"));
        for v in json["vectors"].as_array().unwrap_or_else(|| die("no vectors array")) {
            let name = v["protocol_name"].as_str().or_else(|| v["name"].as_str()).unwrap_or("");
            if name == XX {
                run_official(v, name);
                xx += 1;
            } else if name == IK {
                run_official(v, name);
                ik += 1;
            }
        }
    }
    if xx < 1 || ik < 2 {
        die(&format!("official vectors missing: XXpsk3 {xx}, IK {ik} (need >= 1, >= 2)"));
    }
    (xx, ik)
}

fn run_official(v: &serde_json::Value, name: &str) {
    let (i_pro, r_pro) = (field(v, "init_prologue"), field(v, "resp_prologue"));
    let (i_s, i_e) = (field(v, "init_static"), field(v, "init_ephemeral"));
    let (r_s, r_e) = (field(v, "resp_static"), field(v, "resp_ephemeral"));
    let i_rs = v["init_remote_static"].as_str().map(unhex);
    let (i_psks, r_psks) = (psks(v, "init_psks"), psks(v, "resp_psks"));
    let mut ib = Builder::new(params(name))
        .prologue(&i_pro)
        .unwrap()
        .local_private_key(&i_s)
        .unwrap()
        .fixed_ephemeral_key_for_testing_only(&i_e);
    if let Some(rs) = &i_rs {
        ib = ib.remote_public_key(rs).unwrap();
    }
    if let Some(p) = i_psks.first() {
        ib = ib.psk(3, p).unwrap();
    }
    let mut rb = Builder::new(params(name))
        .prologue(&r_pro)
        .unwrap()
        .local_private_key(&r_s)
        .unwrap()
        .fixed_ephemeral_key_for_testing_only(&r_e);
    if let Some(p) = r_psks.first() {
        rb = rb.psk(3, p).unwrap();
    }
    let mut init = Some(ib.build_initiator().unwrap_or_else(|_| die("official: initiator build")));
    let mut resp = Some(rb.build_responder().unwrap_or_else(|_| die("official: responder build")));
    let (mut it, mut rt) = (None, None);
    let mut buf = vec![0u8; 65535];
    let mut out = vec![0u8; 65535];
    for (i, m) in v["messages"].as_array().unwrap_or_else(|| die("official: messages")).iter().enumerate() {
        let payload = field(m, "payload");
        let want = field(m, "ciphertext");
        let from_init = i % 2 == 0;
        if let (Some(ih), Some(rh)) = (init.as_mut(), resp.as_mut()) {
            let (w, r) = if from_init { (ih, rh) } else { (rh, ih) };
            let n = w.write_message(&payload, &mut buf).unwrap_or_else(|_| die("official: write"));
            if buf[..n] != want[..] {
                die(&format!("official vector {name}: message {i} differs"));
            }
            let pn = r.read_message(&buf[..n], &mut out).unwrap_or_else(|_| die("official: read"));
            if out[..pn] != payload[..] {
                die("official: payload differs");
            }
            if init.as_ref().unwrap().is_handshake_finished() {
                if let Some(hh) = v["handshake_hash"].as_str().map(unhex) {
                    if init.as_ref().unwrap().get_handshake_hash() != hh.as_slice()
                        || resp.as_ref().unwrap().get_handshake_hash() != hh.as_slice()
                    {
                        die(&format!("official vector {name}: handshake hash differs"));
                    }
                }
                it = Some(init.take().unwrap().into_transport_mode().unwrap());
                rt = Some(resp.take().unwrap().into_transport_mode().unwrap());
            }
            continue;
        }
        let (w, r) = if from_init {
            (it.as_mut().unwrap(), rt.as_mut().unwrap())
        } else {
            (rt.as_mut().unwrap(), it.as_mut().unwrap())
        };
        let n = w.write_message(&payload, &mut buf).unwrap_or_else(|_| die("official: transport write"));
        if buf[..n] != want[..] {
            die(&format!("official vector {name}: transport message {i} differs"));
        }
        let pn = r.read_message(&buf[..n], &mut out).unwrap_or_else(|_| die("official: transport read"));
        if out[..pn] != payload[..] {
            die("official: transport payload differs");
        }
    }
}

/// The AEAD-with-AD path must equal snow's transport encryption when the AD is empty.
fn check_aead_path() {
    let r_pub = x25519_pub(&pattern32(0x21));
    let (is, rs, ie, re) = (pattern32(0x01), pattern32(0x21), pattern32(0x41), pattern32(0x61));
    let mut i = Builder::new(params(IK))
        .local_private_key(&is)
        .unwrap()
        .remote_public_key(&r_pub)
        .unwrap()
        .fixed_ephemeral_key_for_testing_only(&ie)
        .build_initiator()
        .unwrap();
    let mut r = Builder::new(params(IK))
        .local_private_key(&rs)
        .unwrap()
        .fixed_ephemeral_key_for_testing_only(&re)
        .build_responder()
        .unwrap();
    let (mut b, mut p) = ([0u8; 1024], [0u8; 1024]);
    let n = i.write_message(&[], &mut b).unwrap();
    r.read_message(&b[..n], &mut p).unwrap();
    let n = r.write_message(&[], &mut b).unwrap();
    i.read_message(&b[..n], &mut p).unwrap();
    let (k1, k2) = i.dangerously_get_raw_split();
    if (k1, k2) != r.dangerously_get_raw_split() {
        die("raw split differs between roles");
    }
    let mut it = i.into_transport_mode().unwrap();
    let mut rt = r.into_transport_mode().unwrap();
    let msg = b"DSP2 aead self-check";
    for nn in 0..3u64 {
        let n = it.write_message(msg, &mut b).unwrap();
        if aead_encrypt(&k1, nn, &[], msg) != b[..n] {
            die("AEAD path (initiator -> responder) differs from snow");
        }
        let n = rt.write_message(msg, &mut b).unwrap();
        if aead_encrypt(&k2, nn, &[], msg) != b[..n] {
            die("AEAD path (responder -> initiator) differs from snow");
        }
    }
}

fn check_session_vectors(path: &str) -> usize {
    let text = std::fs::read_to_string(path).unwrap_or_else(|_| die(&format!("cannot read {path}")));
    let mut n = 0;
    for line in text.lines().map(str::trim).filter(|l| !l.is_empty() && !l.starts_with('#')) {
        let mut it = line.split_whitespace();
        let rec = it.next().unwrap();
        let kv: HashMap<&str, &str> = it.filter_map(|t| t.split_once('=')).collect();
        let get = |k: &str| kv.get(k).copied().unwrap_or_else(|| die(&format!("{rec}: missing {k}")));
        let ok = match rec {
            "psk" | "psk_not" => {
                let got = psk(get("purpose").parse().unwrap(), &unhex(get("setup")), &unhex(get("token")));
                got.as_slice() == unhex(get("psk")).as_slice()
                    && (rec == "psk" || got.as_slice() != unhex(get("wrong")).as_slice())
            }
            "prologue" => {
                prologue(
                    get("major").parse().unwrap(),
                    get("minor").parse().unwrap(),
                    get("purpose").parse().unwrap(),
                    &unhex(get("hash")),
                    get("session_id").parse().unwrap(),
                ) == unhex(get("prologue"))
            }
            "idhash" => {
                let got = match get("kind") {
                    "claimed" => idhash(b"DSP2 id", &unhex(get("input"))),
                    "device" => idhash(b"DSP2 dev", &unhex(get("input"))),
                    k => die(&format!("ble_session_v1 idhash: unknown kind {k}")),
                };
                got == unhex(get("hash"))
            }
            _ => die(&format!("ble_session_v1: unknown record {rec}")),
        };
        if !ok {
            die(&format!("ble_session_v1 {rec} {} does not reproduce", kv.get("name").unwrap_or(&"?")));
        }
        n += 1;
    }
    if n == 0 {
        die("ble_session_v1: no records");
    }
    n
}

// ---- Scenario ------------------------------------------------------------------------------------

#[derive(Default)]
struct Out(String);

impl Out {
    fn line(&mut self, l: impl AsRef<str>) {
        self.0.push_str(l.as_ref());
        self.0.push('\n');
    }
}

/// One handshake's inputs (all synthetic test values).
#[derive(Clone)]
struct Hs {
    id: &'static str,
    purpose: u8,
    session_id: u32,
    claim_state: u8,
    hello_flags: u8,
    hash8: Vec<u8>,
    phone_static: [u8; 32],
    phone_eph: [u8; 32],
    device_static: [u8; 32],
    device_eph: [u8; 32],
    setup: Vec<u8>,
    token: Vec<u8>,
    name: Vec<u8>,
}

impl Hs {
    fn xx(&self) -> bool {
        self.purpose != PURPOSE_SESSION
    }
    fn pro(&self) -> Vec<u8> {
        prologue(1, 0, self.purpose, &self.hash8, self.session_id)
    }
    fn psk(&self) -> [u8; 32] {
        if self.xx() {
            psk(self.purpose, &self.setup, &self.token)
        } else {
            [0u8; 32]
        }
    }
    fn name_payload(&self) -> Vec<u8> {
        let mut p = vec![u8::try_from(self.name.len()).unwrap()];
        p.extend_from_slice(&self.name);
        p
    }
}

#[derive(Clone, Copy)]
struct Session {
    k_phone: [u8; 32], // phone -> device (initiator -> responder)
    k_dev: [u8; 32],   // device -> phone
    n_phone: u64,
    n_dev: u64,
}

/// Phone (initiator) with the given prologue, PSK and pinned device key.
fn phone(h: &Hs, pro: &[u8], psk_v: &[u8; 32], pinned: &[u8; 32]) -> HandshakeState {
    let mut b = Builder::new(params(if h.xx() { XX } else { IK }))
        .prologue(pro)
        .unwrap()
        .local_private_key(&h.phone_static)
        .unwrap()
        .fixed_ephemeral_key_for_testing_only(&h.phone_eph);
    b = if h.xx() { b.psk(3, psk_v).unwrap() } else { b.remote_public_key(pinned).unwrap() };
    b.build_initiator().unwrap()
}

/// Device (responder) with the base inputs.
fn device(h: &Hs) -> HandshakeState {
    let (pro, psk_v) = (h.pro(), h.psk());
    let mut b = Builder::new(params(if h.xx() { XX } else { IK }))
        .prologue(&pro)
        .unwrap()
        .local_private_key(&h.device_static)
        .unwrap()
        .fixed_ephemeral_key_for_testing_only(&h.device_eph);
    if h.xx() {
        b = b.psk(3, &psk_v).unwrap();
    }
    b.build_responder().unwrap()
}

/// Runs the handshake, writes its records and returns the transport keys and the HS frames.
fn handshake(o: &mut Out, h: &Hs, device_id: &[u8]) -> (Session, Vec<Vec<u8>>) {
    let (pro, psk_v) = (h.pro(), h.psk());
    let device_pub = x25519_pub(&h.device_static);
    let phone_pub = x25519_pub(&h.phone_static);
    o.line(format!(
        "hs id={} purpose={} pattern={} proto=1 minor=0 device_id={} session_id={} claim_state={} hello_flags={} \
         device_id_hash={} prologue={} setup={} token={} psk={} phone_static={} phone_static_pub={} \
         phone_ephemeral={} device_static={} device_static_pub={} device_ephemeral={} name={}",
        h.id,
        h.purpose,
        if h.xx() { "XXpsk3" } else { "IK" },
        xhex(device_id),
        h.session_id,
        h.claim_state,
        h.hello_flags,
        xhex(&h.hash8),
        xhex(&pro),
        if h.xx() { xhex(&h.setup) } else { "-".into() },
        xhex(&h.token),
        if h.xx() { xhex(&psk_v) } else { "-".into() },
        xhex(&h.phone_static),
        xhex(&phone_pub),
        xhex(&h.phone_eph),
        xhex(&h.device_static),
        xhex(&device_pub),
        xhex(&h.device_eph),
        xhex(&h.name),
    ));
    let mut init = phone(h, &pro, &psk_v, &device_pub);
    let mut resp = device(h);
    let (mut buf, mut pl) = ([0u8; 1024], [0u8; 1024]);
    let mut frames = Vec::new();
    // message 1 (phone, HS msg_index 0, txn 2 after HELLO txn 1): empty payload
    let n = init.write_message(&[], &mut buf).unwrap();
    frames.push(hs_request(h.purpose, 0, 2, &buf[..n]));
    o.line(format!("frame id={} n=1 dir=p2d note=hs_msg0 bytes={}", h.id, xhex(&frames[0])));
    if resp.read_message(&buf[..n], &mut pl).unwrap() != 0 {
        die("message 1 payload not empty");
    }
    // message 2 (device, HS response msg_index 1): empty payload
    let n = resp.write_message(&[], &mut buf).unwrap();
    frames.push(hs_response(1, 2, &buf[..n]));
    o.line(format!("frame id={} n=2 dir=d2p note=hs_msg1 bytes={}", h.id, xhex(&frames[1])));
    if init.read_message(&buf[..n], &mut pl).unwrap() != 0 || init.get_remote_static() != Some(&device_pub[..]) {
        die("message 2: payload or device static key");
    }
    if h.xx() {
        // message 3 (phone, HS msg_index 2, txn 3): payload = name len u8 || UTF-8 bytes
        let payload = h.name_payload();
        let n = init.write_message(&payload, &mut buf).unwrap();
        frames.push(hs_request(h.purpose, 2, 3, &buf[..n]));
        o.line(format!("frame id={} n=3 dir=p2d note=hs_msg2 bytes={}", h.id, xhex(&frames[2])));
        let pn = resp.read_message(&buf[..n], &mut pl).unwrap();
        if pl[..pn] != payload[..] || resp.get_remote_static() != Some(&phone_pub[..]) {
            die("message 3: payload or phone static key");
        }
    }
    if !init.is_handshake_finished() || !resp.is_handshake_finished() {
        die("handshake not finished");
    }
    if init.get_handshake_hash() != resp.get_handshake_hash() {
        die("handshake hash differs between roles");
    }
    o.line(format!("hash id={} handshake_hash={}", h.id, xhex(init.get_handshake_hash())));
    let (k1, k2) = init.dangerously_get_raw_split();
    if (k1, k2) != resp.dangerously_get_raw_split() {
        die("split differs between roles");
    }
    (Session { k_phone: k1, k_dev: k2, n_phone: 0, n_dev: 0 }, frames)
}

#[allow(clippy::too_many_arguments)]
fn enc(o: &mut Out, id: &str, n: u32, s: &mut Session, from_dev: bool, note: &str, t: u8, flags: u8, txn: u16, plain: &[u8]) -> Vec<u8> {
    let (key, ctr) = if from_dev { (s.k_dev, &mut s.n_dev) } else { (s.k_phone, &mut s.n_phone) };
    let (h, f) = enc_frame(&key, *ctr, t, flags, txn, plain);
    if aead_decrypt(&key, *ctr, &h, &f[8..]).as_deref() != Some(plain) {
        die("encrypted frame does not decrypt");
    }
    o.line(format!(
        "enc id={id} n={n} dir={} note={note} nonce={} header={} plain={} bytes={}",
        if from_dev { "d2p" } else { "p2d" },
        *ctr,
        xhex(&h),
        xhex(plain),
        xhex(&f)
    ));
    *ctr += 1;
    f
}

fn end_session(o: &mut Out, id: &str, s: &mut Session, n0: u32, txn: u16) {
    enc(o, id, n0, s, false, "end_session", T_END_SESSION, 0, txn, &[]);
    enc(o, id, n0 + 1, s, true, "end_session_rsp", T_END_SESSION, F_RESPONSE, txn, &[]);
}

fn negative(o: &mut Out, id: &str, base: &str, change: &str, expect: &str, frame_n: u32, bytes: &[u8]) {
    o.line(format!("neg id={id} base={base} change={change} expect={expect} frame={frame_n} bytes={}", xhex(bytes)));
}

/// Phone message 1 (idx 0) or XX message 3 (idx 2, after the base device message 2) produced by a
/// phone with the given prologue / PSK / pinned key; None if the phone rejects message 2.
fn phone_frame(h: &Hs, pro: &[u8], psk_v: &[u8; 32], pinned: &[u8; 32], base: &[Vec<u8>], idx: usize) -> Option<Vec<u8>> {
    let mut init = phone(h, pro, psk_v, pinned);
    let (mut buf, mut pl) = ([0u8; 1024], [0u8; 1024]);
    let n = init.write_message(&[], &mut buf).unwrap();
    if idx == 0 {
        return Some(hs_request(h.purpose, 0, 2, &buf[..n]));
    }
    init.read_message(&base[1][9..], &mut pl).ok()?; // device message 2 after header + msg_index
    let n = init.write_message(&h.name_payload(), &mut buf).unwrap();
    Some(hs_request(h.purpose, 2, 3, &buf[..n]))
}

/// True if the base device accepts every phone frame but the last and rejects the last.
fn device_rejects_last(h: &Hs, phone_frames: &[&[u8]]) -> bool {
    let mut resp = device(h);
    let (mut buf, mut pl) = ([0u8; 1024], [0u8; 1024]);
    for (i, f) in phone_frames.iter().enumerate() {
        let last = i + 1 == phone_frames.len();
        if resp.read_message(&f[10..], &mut pl).is_err() {
            return last; // after header + purpose + msg_index
        }
        if !last {
            resp.write_message(&[], &mut buf).unwrap();
        }
    }
    false
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        die("usage: gen_ble_handshake_v1 <noise-c tests/vector dir> <ble_session_v1.txt>");
    }
    let (xx, ik) = check_official_vectors(&args[1]);
    check_aead_path();
    let session_records = check_session_vectors(&args[2]);

    let device_id = unhex("a0b1c2d3e4f5");
    let setup = unhex("303132333435363738393a3b3c3d3e3f");
    let token = unhex("bacd0fa8879ea4dd104c412f93e1a7cb");
    let (d1, d2) = (pattern32(0x40), pattern32(0xc0)); // identities generated at CLAIM / RECOVERY
    let (phone_a, phone_b, phone_c) = (pattern32(0x10), pattern32(0x60), pattern32(0x90));
    let (d1_pub, d2_pub) = (x25519_pub(&d1), x25519_pub(&d2));
    let other_pub = x25519_pub(&pattern32(0xe0));

    let mut o = Out::default();
    for l in [
        "# BLE handshake transcripts v1 (docs/BLE_PROTOCOL.md section 11; docs/ANDROID_BLE_DESIGN.md 3.5).".to_string(),
        "# Generated by tools/gen_ble_handshake_v1 (Rust; snow =0.10.0 for Noise, RustCrypto hkdf/sha2 for the".into(),
        "# derivations, chacha20poly1305 for frames with the header as AD; every version pinned by Cargo.lock),".into(),
        "# an implementation independent of noise-c. Before writing, the tool reproduced the official Noise".into(),
        format!("# vectors (XXpsk3 {xx}, IK {ik}, 0 skipped), its AEAD path against snow, and {session_records} records of"),
        "# ble_session_v1.txt. These are NOT conformance vectors (the official Noise files are run separately).".into(),
        "# Every key, ephemeral, setup code and token is a visibly synthetic TEST value, never a real secret.".into(),
        "#".into(),
        "# One device lifetime, in order: claim_a (phone A claims; the device generates identity D1),".into(),
        "# session_a (A: IK, then INVITE_CREATE), add_b (phone B joins with the invitation token, slot 1),".into(),
        "# session_b, recovery_c (phone C recovers with the recovery armed; the device generates identity D2;".into(),
        "# EV_CONFIRM_ON_DEVICE, three presses, ENROLL_RESULT {0}), session_c (IK with D2). Each transcript".into(),
        "# ends with an encrypted END_SESSION from the phone and its encrypted response. txn: HELLO 1 (not".into(),
        "# recorded: codec vectors), HS message 0 = 2, HS message 2 = 3, then increasing.".into(),
        "#".into(),
        "# Records (bytes \"x:<hex>\", \"-\" = empty):".into(),
        "#   hs    one handshake: inputs and derived values. session_id = HELLO session ID (decimal; LE in the".into(),
        "#         prologue); claim_state, hello_flags, device_id_hash = the HELLO response fields; *_ephemeral".into(),
        "#         = ephemeral PRIVATE keys (test-only fixed ephemerals); device_static = the device identity".into(),
        "#         private key in use (generated by the device at claim/recovery); name = XX message 3 name.".into(),
        "#   frame plain HS frames on the wire (header + body); n = order within the transcript.".into(),
        "#   hash  Noise handshake hash after the last handshake message.".into(),
        "#   enc   encrypted frames: nonce = per-direction counter, header = the AD, plain = body plaintext.".into(),
        "#   neg   negative case: base transcript, the changed input, which side fails at which frame (n),".into(),
        "#         and the exact bytes that side receives. device_rejects = the device answers".into(),
        "#         HANDSHAKE_FAILED (plain) and closes; device_drops = it closes without any reply;".into(),
        "#         phone_rejects = the phone's Noise processing fails; phone_aborts = the phone's identity".into(),
        "#         check refuses (before sending message 3).".into(),
        String::new(),
    ] {
        o.line(l);
    }

    let claim = Hs {
        id: "claim_a",
        purpose: PURPOSE_CLAIM,
        session_id: 0x5e55_1001,
        claim_state: 0,
        hello_flags: 0,
        hash8: idhash(b"DSP2 dev", &device_id),
        phone_static: phone_a,
        phone_eph: pattern32(0x20),
        device_static: d1,
        device_eph: pattern32(0x50),
        setup: setup.clone(),
        token: Vec::new(),
        name: b"Phone A".to_vec(),
    };
    let (mut s, claim_frames) = handshake(&mut o, &claim, &device_id);
    let claim_keys = s;
    enc(&mut o, claim.id, 4, &mut s, true, "enroll_result", T_ENROLL_RESULT, F_RESPONSE, 3, &[0]);
    end_session(&mut o, claim.id, &mut s, 5, 4);
    o.line("");

    let session_a = Hs {
        id: "session_a",
        purpose: PURPOSE_SESSION,
        session_id: 0x5e55_1002,
        claim_state: 1,
        hash8: idhash(b"DSP2 id", &d1_pub),
        phone_eph: pattern32(0x21),
        device_eph: pattern32(0x51),
        setup: Vec::new(),
        name: Vec::new(),
        ..claim.clone()
    };
    let (mut s, session_a_frames) = handshake(&mut o, &session_a, &device_id);
    enc(&mut o, session_a.id, 3, &mut s, false, "invite_create", T_INVITE_CREATE, 0, 3, &[1, b'B']);
    let mut rsp = token.clone();
    rsp.extend_from_slice(&600u16.to_le_bytes());
    enc(&mut o, session_a.id, 4, &mut s, true, "invite_create_rsp", T_INVITE_CREATE, F_RESPONSE, 3, &rsp);
    end_session(&mut o, session_a.id, &mut s, 5, 4);
    o.line("");

    let add_b = Hs {
        id: "add_b",
        purpose: PURPOSE_ADD,
        session_id: 0x5e55_1003,
        claim_state: 1,
        hello_flags: 0,
        hash8: idhash(b"DSP2 id", &d1_pub),
        phone_static: phone_b,
        phone_eph: pattern32(0x70),
        device_static: d1,
        device_eph: pattern32(0x52),
        setup: setup.clone(),
        token: token.clone(),
        name: b"Phone B".to_vec(),
    };
    let (mut s, add_frames) = handshake(&mut o, &add_b, &device_id);
    enc(&mut o, add_b.id, 4, &mut s, true, "enroll_result", T_ENROLL_RESULT, F_RESPONSE, 3, &[1]);
    end_session(&mut o, add_b.id, &mut s, 5, 4);
    o.line("");

    let session_b = Hs {
        id: "session_b",
        purpose: PURPOSE_SESSION,
        session_id: 0x5e55_1004,
        phone_eph: pattern32(0x71),
        device_eph: pattern32(0x53),
        setup: Vec::new(),
        token: Vec::new(),
        name: Vec::new(),
        ..add_b.clone()
    };
    let (mut s, _) = handshake(&mut o, &session_b, &device_id);
    end_session(&mut o, session_b.id, &mut s, 3, 3);
    o.line("");

    let recovery_c = Hs {
        id: "recovery_c",
        purpose: PURPOSE_RECOVERY,
        session_id: 0x5e55_1005,
        claim_state: 1,
        hello_flags: 1,
        hash8: idhash(b"DSP2 id", &d1_pub),
        phone_static: phone_c,
        phone_eph: pattern32(0xa0),
        device_static: d2,
        device_eph: pattern32(0x54),
        setup: setup.clone(),
        token: Vec::new(),
        name: b"Phone C".to_vec(),
    };
    let (mut s, _) = handshake(&mut o, &recovery_c, &device_id);
    enc(&mut o, recovery_c.id, 4, &mut s, true, "ev_confirm_on_device", T_EV_CONFIRM_ON_DEVICE, F_EVENT, 0, &[1, 20]);
    enc(&mut o, recovery_c.id, 5, &mut s, true, "enroll_result", T_ENROLL_RESULT, F_RESPONSE, 3, &[0]);
    end_session(&mut o, recovery_c.id, &mut s, 6, 4);
    o.line("");

    let session_c = Hs {
        id: "session_c",
        purpose: PURPOSE_SESSION,
        session_id: 0x5e55_1006,
        hello_flags: 0,
        hash8: idhash(b"DSP2 id", &d2_pub),
        phone_eph: pattern32(0xa1),
        device_eph: pattern32(0x55),
        setup: Vec::new(),
        name: Vec::new(),
        ..recovery_c.clone()
    };
    let (mut s, _) = handshake(&mut o, &session_c, &device_id);
    end_session(&mut o, session_c.id, &mut s, 3, 3);
    o.line("");

    o.line("# negative cases");
    // CLAIM with a wrong setup code: the device fails reading message 3 (psk3)
    let bad_setup = unhex("303132333435363738393a3b3c3d3e3e");
    let f3 = phone_frame(&claim, &claim.pro(), &psk(PURPOSE_CLAIM, &bad_setup, &[]), &d1_pub, &claim_frames, 2)
        .unwrap_or_else(|| die("neg claim_wrong_setup: phone rejected message 2"));
    if !device_rejects_last(&claim, &[&claim_frames[0], &f3]) {
        die("neg claim_wrong_setup: device accepted");
    }
    negative(&mut o, "claim_wrong_setup", "claim_a", &format!("setup={}", xhex(&bad_setup)), "device_rejects", 3, &f3);
    // ADD with a wrong invitation token: the device fails reading message 3
    let bad_token = unhex("bacd0fa8879ea4dd104c412f93e1a7ca");
    let f3 = phone_frame(&add_b, &add_b.pro(), &psk(PURPOSE_ADD, &setup, &bad_token), &d1_pub, &add_frames, 2)
        .unwrap_or_else(|| die("neg add_wrong_token: phone rejected message 2"));
    if !device_rejects_last(&add_b, &[&add_frames[0], &f3]) {
        die("neg add_wrong_token: device accepted");
    }
    negative(&mut o, "add_wrong_token", "add_b", &format!("token={}", xhex(&bad_token)), "device_rejects", 3, &f3);
    // ADD with an invitation naming another device identity: the phone aborts after message 2
    negative(&mut o, "add_invitation_other_device", "add_b", &format!("invitation_device_static_pub={}", xhex(&other_pub)), "phone_aborts", 2, &add_frames[1]);
    // IK with a wrong pinned device key: the device fails reading message 1
    let f1 = phone_frame(&session_a, &session_a.pro(), &[0; 32], &other_pub, &session_a_frames, 0).unwrap();
    if !device_rejects_last(&session_a, &[&f1]) {
        die("neg session_wrong_pinned_key: device accepted");
    }
    negative(&mut o, "session_wrong_pinned_key", "session_a", &format!("pinned_device_static_pub={}", xhex(&other_pub)), "device_rejects", 1, &f1);
    // IK with a phone key that is not enrolled: Noise reads message 1, the authorization refuses
    let unknown = Hs { phone_static: pattern32(0xf0), ..session_a.clone() };
    let f1 = phone_frame(&unknown, &session_a.pro(), &[0; 32], &d1_pub, &session_a_frames, 0).unwrap();
    if device_rejects_last(&session_a, &[&f1]) {
        die("neg session_unknown_phone_key: Noise rejected (expected an authorization refusal)");
    }
    negative(&mut o, "session_unknown_phone_key", "session_a", &format!("phone_static={}", xhex(&unknown.phone_static)), "device_rejects", 1, &f1);
    // IK with the prologue of another connection (session ID): the device fails reading message 1
    let other_sid = session_a.session_id ^ 1;
    let f1 = phone_frame(&session_a, &prologue(1, 0, PURPOSE_SESSION, &session_a.hash8, other_sid), &[0; 32], &d1_pub, &session_a_frames, 0).unwrap();
    if !device_rejects_last(&session_a, &[&f1]) {
        die("neg session_prologue_session_id: device accepted");
    }
    negative(&mut o, "session_prologue_session_id", "session_a", &format!("session_id={other_sid}"), "device_rejects", 1, &f1);
    // CLAIM with a prologue naming another purpose: the phone fails reading message 2
    let wrong_pro = prologue(1, 0, PURPOSE_ADD, &claim.hash8, claim.session_id);
    if phone_frame(&claim, &wrong_pro, &claim.psk(), &d1_pub, &claim_frames, 2).is_some() {
        die("neg claim_prologue_purpose: phone accepted message 2");
    }
    negative(&mut o, "claim_prologue_purpose", "claim_a", &format!("prologue={}", xhex(&wrong_pro)), "phone_rejects", 2, &claim_frames[1]);
    // tampered encrypted frames of claim_a
    let mut sink = Out::default();
    let mut s = claim_keys;
    let mut er = enc(&mut sink, "x", 0, &mut s, true, "x", T_ENROLL_RESULT, F_RESPONSE, 3, &[0]);
    er[8] ^= 0x01;
    negative(&mut o, "claim_enroll_result_bitflip", "claim_a", "frame4_ciphertext_byte0_bit0", "phone_rejects", 4, &er);
    let mut es = enc(&mut sink, "x", 0, &mut s, false, "x", T_END_SESSION, 0, 4, &[]);
    let truncated = es[..es.len() - 1].to_vec();
    let last = es.len() - 1;
    es[last] ^= 0x80;
    negative(&mut o, "claim_end_session_tag_bitflip", "claim_a", "frame5_tag_last_byte_bit7", "device_drops", 5, &es);
    negative(&mut o, "claim_end_session_truncated", "claim_a", "frame5_truncated_by_1", "device_drops", 5, &truncated);
    print!("{}", o.0);
}
