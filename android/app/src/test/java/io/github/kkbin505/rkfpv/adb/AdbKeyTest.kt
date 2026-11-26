package io.github.kkbin505.rkfpv.adb

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.math.BigInteger
import java.security.KeyFactory
import java.security.spec.RSAPublicKeySpec
import javax.crypto.Cipher
import kotlin.random.Random

class AdbKeyTest {
    @get:Rule val tmp = TemporaryFolder()

    /** test_adbkey / test_adbkey.pub were made by `adb keygen` (a throwaway key, tests only). */
    private fun resource(name: String) = javaClass.classLoader!!.getResource("adb/$name")!!.readText()

    @Test fun publicKeyMatchesAdbKeygen() {
        val key = AdbKey.fromPem(resource("test_adbkey"))
        val expected = resource("test_adbkey.pub").trim().split(" ")[0]
        assertEquals(expected, key.publicKeyBase64())
    }

    @Test fun publicKeyPayloadIsNulTerminated() {
        val key = AdbKey.fromPem(resource("test_adbkey"))
        val p = String(key.publicKeyPayload("me@phone"), Charsets.UTF_8)
        assertEquals(key.publicKeyBase64() + " me@phone\u0000", p)
    }

    /** What adbd's RSA_verify(NID_sha1, token, ...) accepts: PKCS#1 v1.5 over SHA-1 DigestInfo + token. */
    @Test fun signatureVerifiesAsPkcs1Sha1() {
        val key = AdbKey.fromPem(resource("test_adbkey"))
        val token = Random(1).nextBytes(20)
        val sig = key.sign(token)
        assertEquals(256, sig.size)
        val pub = KeyFactory.getInstance("RSA").generatePublic(RSAPublicKeySpec(key.modulus, key.publicExponent))
        val c = Cipher.getInstance("RSA/ECB/PKCS1Padding")
        c.init(Cipher.DECRYPT_MODE, pub)
        val digestInfo = BigInteger("3021300906052b0e03021a05000414", 16).toByteArray()
        assertArrayEquals(digestInfo + token, c.doFinal(sig))
    }

    @Test fun loadOrCreateKeepsTheKey() {
        val f = File(tmp.root, "adbkey")
        val a = AdbKey.loadOrCreate(f)
        val b = AdbKey.loadOrCreate(f)
        assertEquals(a.publicKeyBase64(), b.publicKeyBase64())
    }
}
