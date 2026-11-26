package io.github.kkbin505.rkfpv.adb

import java.io.File
import java.math.BigInteger
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.interfaces.RSAPrivateCrtKey
import java.security.spec.PKCS8EncodedKeySpec
import java.util.Base64

/**
 * The RSA key this app authenticates with, like ~/.android/adbkey on a PC.
 *
 * Signing and the public key format are done by hand (BigInteger), so they
 * behave the same on every Android version and JCE provider:
 *  - adbd checks the AUTH signature with RSA_verify(NID_sha1, token): PKCS#1
 *    v1.5, the 20-byte token taken as an already computed SHA-1 digest.
 *  - adbd stores public keys in the mincrypt RSAPublicKey layout, base64.
 */
class AdbKey private constructor(private val key: RSAPrivateCrtKey) {

    val modulus: BigInteger get() = key.modulus
    val publicExponent: BigInteger get() = key.publicExponent

    fun pkcs8(): ByteArray = key.encoded

    fun sign(token: ByteArray): ByteArray {
        val k = byteLength()
        val t = SHA1_DIGEST_INFO + token
        require(t.size + 11 <= k) { "token too long" }
        // EMSA-PKCS1-v1_5: 00 01 FF..FF 00 T
        val em = ByteArray(k)
        em[1] = 1
        for (i in 2 until k - t.size - 1) em[i] = 0xff.toByte()
        System.arraycopy(t, 0, em, k - t.size, t.size)
        val s = BigInteger(1, em).modPow(key.privateExponent, key.modulus)
        return toFixed(s, k)
    }

    /** "<base64 RSAPublicKey> <name>\0", the AUTH_RSAPUBLICKEY payload. */
    fun publicKeyPayload(name: String): ByteArray =
        (publicKeyBase64() + " " + name + "\u0000").toByteArray(Charsets.UTF_8)

    fun publicKeyBase64(): String = Base64.getEncoder().encodeToString(androidPublicKey())

    private fun androidPublicKey(): ByteArray {
        val n = key.modulus
        val words = byteLength() / 4
        val r32 = BigInteger.ONE.shiftLeft(32)
        val n0inv = r32.subtract(n.mod(r32).modInverse(r32)).mod(r32)
        val rr = BigInteger.ONE.shiftLeft(byteLength() * 8 * 2).mod(n)
        val b = ByteBuffer.allocate(4 + 4 + 2 * byteLength() + 4).order(ByteOrder.LITTLE_ENDIAN)
        b.putInt(words)
        b.putInt(n0inv.toInt())
        b.put(toFixed(n, byteLength()).reversedArray())
        b.put(toFixed(rr, byteLength()).reversedArray())
        b.putInt(key.publicExponent.toInt())
        return b.array()
    }

    private fun byteLength() = (key.modulus.bitLength() + 7) / 8

    companion object {
        private val SHA1_DIGEST_INFO = byteArrayOf(
            0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14,
        )

        fun generate(): AdbKey {
            val g = KeyPairGenerator.getInstance("RSA")
            g.initialize(2048)
            return AdbKey(g.generateKeyPair().private as RSAPrivateCrtKey)
        }

        fun fromPkcs8(der: ByteArray): AdbKey =
            AdbKey(KeyFactory.getInstance("RSA").generatePrivate(PKCS8EncodedKeySpec(der)) as RSAPrivateCrtKey)

        /** A PEM "PRIVATE KEY" file, as written by `adb keygen`. */
        fun fromPem(pem: String): AdbKey {
            val body = pem.lineSequence()
                .filter { it.isNotBlank() && !it.startsWith("-----") }
                .joinToString("")
            return fromPkcs8(Base64.getDecoder().decode(body))
        }

        /** Load the app's key, or make one on first use. */
        fun loadOrCreate(file: File): AdbKey {
            if (file.isFile) {
                runCatching { return fromPkcs8(file.readBytes()) }
            }
            val k = generate()
            val tmp = File(file.path + ".tmp")
            tmp.writeBytes(k.pkcs8())
            tmp.renameTo(file)
            return k
        }

        private fun toFixed(v: BigInteger, len: Int): ByteArray {
            val raw = v.toByteArray()
            val out = ByteArray(len)
            val n = minOf(raw.size, len)
            System.arraycopy(raw, raw.size - n, out, len - n, n)
            return out
        }
    }
}
