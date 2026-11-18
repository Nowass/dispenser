/*
 * JNI binding of the native Noise core for cz.nowass.dispenser.crypto.NativeNoise (M1.10e.1).
 * Glue only: copies Java byte arrays into native buffers (GetByteArrayRegion, never pinned
 * element pointers), calls dspnoise_core, returns new arrays, and clears every native buffer that
 * held key material. Failures throw cz.nowass.dispenser.crypto.NoiseException(code) without any
 * secret in the message. Handles are opaque jlong pointers owned by the Kotlin wrappers, which
 * free them exactly once.
 */
#include <jni.h>
#include <stdint.h>
#include <string.h>

#include <sodium.h>

#include "dspnoise_core.h"

#define CLS "cz/nowass/dispenser/crypto/NativeNoise"
#define EXC "cz/nowass/dispenser/crypto/NoiseException"
#define BUF_MAX (DSPN_MSG_MAX + DSPN_TAG_LEN)

void dspn_throw(JNIEnv *env, int code);
void dspn_throw(JNIEnv *env, int code)
{
    if ((*env)->ExceptionCheck(env)) {
        return;
    }
    jclass c = (*env)->FindClass(env, EXC);
    if (c == NULL) {
        return;
    }
    jmethodID ctor = (*env)->GetMethodID(env, c, "<init>", "(I)V");
    if (ctor == NULL) {
        return;
    }
    jobject e = (*env)->NewObject(env, c, ctor, (jint)code);
    if (e != NULL) {
        (*env)->Throw(env, (jthrowable)e);
    }
}

/* Copies a Java array of exactly `want` bytes (want < 0: any length up to cap) into out. */
int dspn_get(JNIEnv *env, jbyteArray a, uint8_t *out, size_t cap, long want, size_t *len);
int dspn_get(JNIEnv *env, jbyteArray a, uint8_t *out, size_t cap, long want, size_t *len)
{
    if (a == NULL) {
        return 0;
    }
    jsize n = (*env)->GetArrayLength(env, a);
    if (n < 0 || (size_t)n > cap || (want >= 0 && n != want)) {
        return 0;
    }
    (*env)->GetByteArrayRegion(env, a, 0, n, (jbyte *)out);
    if ((*env)->ExceptionCheck(env)) {
        return 0;
    }
    if (len != NULL) {
        *len = (size_t)n;
    }
    return 1;
}

jbyteArray dspn_new(JNIEnv *env, const uint8_t *b, size_t n);
jbyteArray dspn_new(JNIEnv *env, const uint8_t *b, size_t n)
{
    jbyteArray a = (*env)->NewByteArray(env, (jsize)n);
    if (a != NULL && n > 0) {
        (*env)->SetByteArrayRegion(env, a, 0, (jsize)n, (const jbyte *)b);
    }
    return a;
}

JNIEXPORT jint JNICALL Java_cz_nowass_dispenser_crypto_NativeNoise_nativeInit(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    return dspn_init();
}

JNIEXPORT jstring JNICALL Java_cz_nowass_dispenser_crypto_NativeNoise_rngName(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, dspn_rng_name());
}

JNIEXPORT jbyteArray JNICALL Java_cz_nowass_dispenser_crypto_NativeNoise_psk(JNIEnv *env, jclass cls, jint purpose,
                                                                            jbyteArray setup, jbyteArray token)
{
    (void)cls;
    uint8_t s[DSPN_SETUP_LEN], t[DSPN_TOKEN_LEN], out[DSPN_PSK_LEN];
    jbyteArray r = NULL;
    int rc = DSPN_E_ARG;
    if (dspn_get(env, setup, s, sizeof s, DSPN_SETUP_LEN, NULL) &&
        (token == NULL || dspn_get(env, token, t, sizeof t, DSPN_TOKEN_LEN, NULL))) {
        rc = dspn_psk((uint8_t)purpose, s, token != NULL ? t : NULL, out);
        if (rc == DSPN_OK) {
            r = dspn_new(env, out, sizeof out);
        }
    }
    sodium_memzero(s, sizeof s);
    sodium_memzero(t, sizeof t);
    sodium_memzero(out, sizeof out);
    if (rc != DSPN_OK) {
        dspn_throw(env, rc);
    }
    return r;
}

JNIEXPORT jbyteArray JNICALL Java_cz_nowass_dispenser_crypto_NativeNoise_keypair(JNIEnv *env, jclass cls)
{
    (void)cls;
    uint8_t kp[2 * DSPN_KEY_LEN];
    jbyteArray r = NULL;
    int rc = dspn_keypair(kp, kp + DSPN_KEY_LEN);
    if (rc == DSPN_OK) {
        r = dspn_new(env, kp, sizeof kp); /* private || public */
    }
    sodium_memzero(kp, sizeof kp);
    if (rc != DSPN_OK) {
        dspn_throw(env, rc);
    }
    return r;
}

JNIEXPORT jbyteArray JNICALL Java_cz_nowass_dispenser_crypto_NativeNoise_publicKey(JNIEnv *env, jclass cls,
                                                                                  jbyteArray priv)
{
    (void)cls;
    uint8_t k[DSPN_KEY_LEN], pub[DSPN_KEY_LEN];
    jbyteArray r = NULL;
    int rc = DSPN_E_ARG;
    if (dspn_get(env, priv, k, sizeof k, DSPN_KEY_LEN, NULL)) {
        rc = dspn_public_key(k, pub);
        if (rc == DSPN_OK) {
            r = dspn_new(env, pub, sizeof pub);
        }
    }
    sodium_memzero(k, sizeof k);
    if (rc != DSPN_OK) {
        dspn_throw(env, rc);
    }
    return r;
}

/* shared with the test hooks: builds a handshake from Java arrays */
jlong dspn_hs_from_java(JNIEnv *env, jint pattern, int initiator, jbyteArray prologue, jbyteArray priv,
                        jbyteArray remote_pub, jbyteArray psk, jbyteArray fixed_eph);
jlong dspn_hs_from_java(JNIEnv *env, jint pattern, int initiator, jbyteArray prologue, jbyteArray priv,
                        jbyteArray remote_pub, jbyteArray psk, jbyteArray fixed_eph)
{
    uint8_t pro[64], k[DSPN_KEY_LEN], rs[DSPN_KEY_LEN], p[DSPN_PSK_LEN], e[DSPN_KEY_LEN];
    size_t pro_len = 0;
    dspn_hs *hs = NULL;
    int rc = DSPN_E_ARG;
    if (dspn_get(env, prologue, pro, sizeof pro, -1, &pro_len) && dspn_get(env, priv, k, sizeof k, DSPN_KEY_LEN, NULL) &&
        (remote_pub == NULL || dspn_get(env, remote_pub, rs, sizeof rs, DSPN_KEY_LEN, NULL)) &&
        (psk == NULL || dspn_get(env, psk, p, sizeof p, DSPN_PSK_LEN, NULL)) &&
        (fixed_eph == NULL || dspn_get(env, fixed_eph, e, sizeof e, DSPN_KEY_LEN, NULL))) {
        rc = dspn_hs_create(&hs, (int)pattern, initiator, pro, pro_len, k, remote_pub != NULL ? rs : NULL,
                            psk != NULL ? p : NULL, fixed_eph != NULL ? e : NULL);
    }
    sodium_memzero(k, sizeof k);
    sodium_memzero(p, sizeof p);
    sodium_memzero(e, sizeof e);
    if (rc != DSPN_OK) {
        dspn_throw(env, rc);
        return 0;
    }
    return (jlong)(intptr_t)hs;
}

JNIEXPORT jlong JNICALL Java_cz_nowass_dispenser_crypto_NativeNoise_hsNew(JNIEnv *env, jclass cls, jint pattern,
                                                                         jbyteArray prologue, jbyteArray priv,
                                                                         jbyteArray remote_pub, jbyteArray psk)
{
    (void)cls;
    return dspn_hs_from_java(env, pattern, 1, prologue, priv, remote_pub, psk, NULL);
}

JNIEXPORT jbyteArray JNICALL Java_cz_nowass_dispenser_crypto_NativeNoise_hsWrite(JNIEnv *env, jclass cls, jlong h,
                                                                                jbyteArray payload)
{
    (void)cls;
    uint8_t in[DSPN_MSG_MAX], out[DSPN_MSG_MAX];
    size_t in_len = 0, out_len = 0;
    jbyteArray r = NULL;
    int rc = DSPN_E_ARG;
    if (payload == NULL || dspn_get(env, payload, in, sizeof in, -1, &in_len)) {
        rc = dspn_hs_write((dspn_hs *)(intptr_t)h, in_len > 0 ? in : NULL, in_len, out, sizeof out, &out_len);
        if (rc == DSPN_OK) {
            r = dspn_new(env, out, out_len);
        }
    }
    sodium_memzero(in, sizeof in);
    sodium_memzero(out, sizeof out);
    if (rc != DSPN_OK) {
        dspn_throw(env, rc);
    }
    return r;
}

JNIEXPORT jbyteArray JNICALL Java_cz_nowass_dispenser_crypto_NativeNoise_hsRead(JNIEnv *env, jclass cls, jlong h,
                                                                               jbyteArray msg)
{
    (void)cls;
    uint8_t in[DSPN_MSG_MAX], out[DSPN_MSG_MAX];
    size_t in_len = 0, out_len = 0;
    jbyteArray r = NULL;
    int rc = DSPN_E_ARG;
    if (dspn_get(env, msg, in, sizeof in, -1, &in_len)) {
        rc = dspn_hs_read((dspn_hs *)(intptr_t)h, in, in_len, out, sizeof out, &out_len);
        if (rc == DSPN_OK) {
            r = dspn_new(env, out, out_len);
        }
    }
    sodium_memzero(out, sizeof out);
    if (rc != DSPN_OK) {
        dspn_throw(env, rc);
    }
    return r;
}

JNIEXPORT jboolean JNICALL Java_cz_nowass_dispenser_crypto_NativeNoise_hsFinished(JNIEnv *env, jclass cls, jlong h)
{
    (void)env;
    (void)cls;
    return dspn_hs_finished((const dspn_hs *)(intptr_t)h) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jbyteArray JNICALL Java_cz_nowass_dispenser_crypto_NativeNoise_hsRemoteStatic(JNIEnv *env, jclass cls,
                                                                                       jlong h)
{
    (void)cls;
    uint8_t out[DSPN_KEY_LEN];
    int rc = dspn_hs_remote_static((const dspn_hs *)(intptr_t)h, out);
    if (rc != DSPN_OK) {
        dspn_throw(env, rc);
        return NULL;
    }
    return dspn_new(env, out, sizeof out);
}

JNIEXPORT jbyteArray JNICALL Java_cz_nowass_dispenser_crypto_NativeNoise_hsHash(JNIEnv *env, jclass cls, jlong h)
{
    (void)cls;
    uint8_t out[DSPN_HASH_LEN];
    int rc = dspn_hs_hash((const dspn_hs *)(intptr_t)h, out);
    if (rc != DSPN_OK) {
        dspn_throw(env, rc);
        return NULL;
    }
    return dspn_new(env, out, sizeof out);
}

JNIEXPORT jlong JNICALL Java_cz_nowass_dispenser_crypto_NativeNoise_hsSplit(JNIEnv *env, jclass cls, jlong h)
{
    (void)cls;
    dspn_cs *cs = NULL;
    int rc = dspn_hs_split((dspn_hs *)(intptr_t)h, &cs);
    if (rc != DSPN_OK) {
        dspn_throw(env, rc);
        return 0;
    }
    return (jlong)(intptr_t)cs;
}

JNIEXPORT void JNICALL Java_cz_nowass_dispenser_crypto_NativeNoise_hsFree(JNIEnv *env, jclass cls, jlong h)
{
    (void)env;
    (void)cls;
    dspn_hs_free((dspn_hs *)(intptr_t)h);
}

JNIEXPORT jbyteArray JNICALL Java_cz_nowass_dispenser_crypto_NativeNoise_csEncrypt(JNIEnv *env, jclass cls, jlong c,
                                                                                  jbyteArray ad, jbyteArray plain)
{
    (void)cls;
    uint8_t a[16], in[DSPN_MSG_MAX], out[BUF_MAX];
    size_t a_len = 0, in_len = 0, out_len = 0;
    jbyteArray r = NULL;
    int rc = DSPN_E_ARG;
    if (dspn_get(env, ad, a, sizeof a, -1, &a_len) && (plain == NULL || dspn_get(env, plain, in, sizeof in, -1, &in_len))) {
        rc = dspn_cs_encrypt((dspn_cs *)(intptr_t)c, a, a_len, in_len > 0 ? in : NULL, in_len, out, sizeof out, &out_len);
        if (rc == DSPN_OK) {
            r = dspn_new(env, out, out_len);
        }
    }
    sodium_memzero(in, sizeof in);
    sodium_memzero(out, sizeof out);
    if (rc != DSPN_OK) {
        dspn_throw(env, rc);
    }
    return r;
}

JNIEXPORT jbyteArray JNICALL Java_cz_nowass_dispenser_crypto_NativeNoise_csDecrypt(JNIEnv *env, jclass cls, jlong c,
                                                                                  jbyteArray ad, jbyteArray ct)
{
    (void)cls;
    uint8_t a[16], in[BUF_MAX], out[BUF_MAX];
    size_t a_len = 0, in_len = 0, out_len = 0;
    jbyteArray r = NULL;
    int rc = DSPN_E_ARG;
    if (dspn_get(env, ad, a, sizeof a, -1, &a_len) && dspn_get(env, ct, in, sizeof in, -1, &in_len)) {
        rc = dspn_cs_decrypt((dspn_cs *)(intptr_t)c, a, a_len, in, in_len, out, sizeof out, &out_len);
        if (rc == DSPN_OK) {
            r = dspn_new(env, out, out_len);
        }
    }
    sodium_memzero(out, sizeof out);
    if (rc != DSPN_OK) {
        dspn_throw(env, rc);
    }
    return r;
}

JNIEXPORT void JNICALL Java_cz_nowass_dispenser_crypto_NativeNoise_csFree(JNIEnv *env, jclass cls, jlong c)
{
    (void)env;
    (void)cls;
    dspn_cs_free((dspn_cs *)(intptr_t)c);
}
