/*
 * TEST-ONLY JNI entry points (M1.10e.1), compiled only into the host test library
 * libdspnoise_hosttest (with DSPN_TEST_HOOKS) and never into the APK: the responder role and fixed
 * ephemeral keys, used by the JVM tests to replay test-vectors/ble_handshake_v1.txt and the
 * official Noise vectors, and by the simulated device peer. Class:
 * cz.nowass.dispenser.crypto.NativeNoiseTestHooks (test source set).
 */
#ifndef DSPN_TEST_HOOKS
#error "dspnoise_test_hooks.c belongs to the host test library only"
#endif

#include <jni.h>
#include <stdint.h>

#include "dspnoise_core.h"

jlong dspn_hs_from_java(JNIEnv *env, jint pattern, int initiator, jbyteArray prologue, jbyteArray priv,
                        jbyteArray remote_pub, jbyteArray psk, jbyteArray fixed_eph);

JNIEXPORT jlong JNICALL Java_cz_nowass_dispenser_crypto_NativeNoiseTestHooks_hsNewTest(
    JNIEnv *env, jclass cls, jint pattern, jboolean initiator, jbyteArray prologue, jbyteArray priv,
    jbyteArray remote_pub, jbyteArray psk, jbyteArray fixed_eph)
{
    (void)cls;
    return dspn_hs_from_java(env, pattern, initiator == JNI_TRUE, prologue, priv, remote_pub, psk, fixed_eph);
}

JNIEXPORT jboolean JNICALL Java_cz_nowass_dispenser_crypto_NativeNoiseTestHooks_isTestBuild(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    return JNI_TRUE;
}
